// SPDX-License-Identifier: MIT

#include "slots.h"

#include "../util/error.h"

#include <atomic>

extern "C" {
#include <sys/mman.h>
#include <unistd.h>
}

namespace iris {

uint64_t Frame::next_id()
{
    static std::atomic<uint64_t> counter { 1 };
    return counter.fetch_add(1);
}

Frame::~Frame()
{
    if (mapping_)
        munmap(mapping_, length_);
    if (fd_ >= 0)
        close(fd_);
}

uint8_t* Frame::map()
{
    if (!mapping_) {
        void* mapping = mmap(nullptr, length_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        require(mapping != MAP_FAILED, "mmap CAPTURE DMA-BUF");
        mapping_ = mapping;
    }
    return static_cast<uint8_t*>(mapping_);
}

SlotPool::~SlotPool()
{
    for (auto& slot : output_)
        device_.unmap_buffer(slot.mapping, slot.length);
}

void SlotPool::allocate_capture(unsigned count, const Layout& layout)
{
    require(capture_.empty(), "CAPTURE pool already allocated");
    const unsigned granted = device_.request_buffers(Queue::Capture, count);
    require(granted > 0 && granted <= 64, "REQBUFS CAPTURE returned an unusable count",
        VA_STATUS_ERROR_ALLOCATION_FAILED);
    capture_layout_ = layout;
    capture_.reserve(granted);
    for (unsigned i = 0; i < granted; ++i) {
        const BufferInfo info = device_.query_buffer(Queue::Capture, i);
        const int fd = device_.export_buffer(Queue::Capture, i);
        auto frame = std::make_shared<Frame>(device_, i, fd, info.length);
        frame->layout() = layout;
        frame->layout().size = info.length;
        capture_.push_back(std::move(frame));
    }
}

void SlotPool::allocate_output(unsigned count)
{
    require(output_.empty(), "OUTPUT pool already allocated");
    const unsigned granted = device_.request_buffers(Queue::Output, count);
    require(granted > 0, "REQBUFS OUTPUT returned zero", VA_STATUS_ERROR_ALLOCATION_FAILED);
    output_.reserve(granted);
    for (unsigned i = 0; i < granted; ++i) {
        const BufferInfo info = device_.query_buffer(Queue::Output, i);
        OutputSlot slot;
        slot.index = i;
        slot.length = info.length;
        slot.mapping = device_.map_buffer(Queue::Output, info);
        output_.push_back(slot);
    }
}

void SlotPool::release_capture()
{
    for (const auto& frame : capture_)
        require(frame.use_count() == 1, "CAPTURE slot still held during release");
    capture_.clear();
    device_.request_buffers(Queue::Capture, 0);
}

void SlotPool::release_output()
{
    for (auto& slot : output_) {
        device_.unmap_buffer(slot.mapping, slot.length);
        slot.mapping = nullptr;
    }
    output_.clear();
    device_.request_buffers(Queue::Output, 0);
}

unsigned SlotPool::requeue_capture()
{
    unsigned queued = 0;
    for (auto& frame : capture_) {
        if (frame->queued_ || frame.use_count() != 1)
            continue;
        device_.queue_buffer(Queue::Capture, frame->index_, 0, 0, 0);
        frame->queued_ = true;
        ++queued;
    }
    return queued;
}

FrameRef SlotPool::take_capture(unsigned index)
{
    require(index < capture_.size(), "CAPTURE DQBUF index out of range", VA_STATUS_ERROR_DECODING_ERROR);
    auto& frame = capture_[index];
    frame->queued_ = false;
    return frame;
}

unsigned SlotPool::capture_queued() const
{
    unsigned count = 0;
    for (const auto& frame : capture_)
        count += frame->queued_ ? 1 : 0;
    return count;
}

unsigned SlotPool::capture_held() const
{
    unsigned count = 0;
    for (const auto& frame : capture_)
        count += frame.use_count() > 1 ? 1 : 0;
    return count;
}

OutputSlot* SlotPool::free_output()
{
    for (auto& slot : output_)
        if (!slot.queued)
            return &slot;
    return nullptr;
}

unsigned SlotPool::output_queued() const
{
    unsigned count = 0;
    for (const auto& slot : output_)
        count += slot.queued ? 1 : 0;
    return count;
}

void SlotPool::mark_all_capture_returned()
{
    for (auto& frame : capture_)
        frame->queued_ = false;
}

void SlotPool::mark_all_output_returned()
{
    for (auto& slot : output_) {
        slot.queued = false;
        slot.token = 0;
    }
}

} // namespace iris
