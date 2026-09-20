// SPDX-License-Identifier: MIT
//
// CAPTURE and OUTPUT slot ownership. A Frame is one CAPTURE slot's DMA-BUF;
// its shared_ptr use count is the slot state (docs/architecture.md §3.2):
//   1 and queued  -> firmware owns it
//   1 and !queued -> free, requeued on the next service pass
//   >1            -> held by a Surface or a copy engine view
// There is no separate "held" flag to drift out of sync with reality.

#pragma once

#include "device.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace iris {

struct Layout {
    uint32_t fourcc = 0; // VA fourcc (NV12 / P010)
    unsigned width = 0, height = 0; // visible
    unsigned stride = 0;
    unsigned storage_height = 0; // rows before the chroma plane
    unsigned data_offset = 0;
    unsigned size = 0;
    unsigned chroma_offset() const { return data_offset + stride * storage_height; }
};

class Frame {
public:
    Frame(Device& device, unsigned index, int fd, unsigned length)
        : device_(device)
        , index_(index)
        , fd_(fd)
        , length_(length)
    {
    }
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    unsigned index() const { return index_; }
    int fd() const { return fd_; }
    uint64_t id() const { return id_; }
    unsigned length() const { return length_; }
    bool queued() const { return queued_; }
    const Layout& layout() const { return layout_; }
    Layout& layout() { return layout_; }
    // CPU view of the DMA-BUF, mapped on first use. Only the CPU copy
    // engine and vaGetImage need it.
    uint8_t* map();

private:
    friend class SlotPool;
    static uint64_t next_id();
    uint64_t id_ = next_id();
    Device& device_;
    unsigned index_;
    int fd_;
    unsigned length_;
    bool queued_ = false;
    void* mapping_ = nullptr;
    Layout layout_;
};

using FrameRef = std::shared_ptr<Frame>;

// One OUTPUT slot: MMAP'd bitstream memory for exactly one access unit.
struct OutputSlot {
    unsigned index = 0;
    unsigned length = 0;
    void* mapping = nullptr;
    bool queued = false;
    uint64_t token = 0; // the AU token this slot carries while queued
};

class SlotPool {
public:
    explicit SlotPool(Device& device)
        : device_(device)
    {
    }
    ~SlotPool();
    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;

    // Allocate and export a CAPTURE pool. Every slot is exported once here;
    // the fd lives as long as the Frame.
    void allocate_capture(unsigned count, const Layout& layout);
    void allocate_output(unsigned count);
    // REQBUFS(0) both queues; requires no Frame to be held (use count 1).
    void release_capture();
    void release_output();

    // QBUF every free CAPTURE slot. Returns the number queued.
    unsigned requeue_capture();
    // Called on CAPTURE DQBUF: mark the slot returned and hand out the ref.
    FrameRef take_capture(unsigned index);
    // The number of slots the firmware currently owns.
    unsigned capture_queued() const;
    unsigned capture_held() const;
    unsigned capture_count() const { return static_cast<unsigned>(capture_.size()); }
    const Layout& capture_layout() const { return capture_layout_; }

    // OUTPUT slot bookkeeping.
    OutputSlot* free_output();
    OutputSlot& output(unsigned index) { return output_.at(index); }
    unsigned output_queued() const;
    unsigned output_count() const { return static_cast<unsigned>(output_.size()); }
    std::vector<OutputSlot>& outputs() { return output_; }

    // After STREAMOFF the kernel has returned every queued buffer without a
    // DQBUF; reflect that.
    void mark_all_capture_returned();
    void mark_all_output_returned();

private:
    Device& device_;
    std::vector<FrameRef> capture_;
    std::vector<OutputSlot> output_;
    Layout capture_layout_;
};

} // namespace iris
