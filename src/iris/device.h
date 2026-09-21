// SPDX-License-Identifier: MIT
//
// The one seam between the decoder logic and /dev/videoN. Session talks to
// this interface only; the real implementation wraps ioctl/poll/mmap and a
// FakeDevice in test/unit scripts responses so the state machine can be
// driven without a tablet.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
}

namespace iris {

enum class Queue { Output, Capture };

struct FormatInfo {
    uint32_t pixelformat = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned num_planes = 0;
    unsigned bytesperline = 0; // plane 0
    unsigned sizeimage = 0; // plane 0
};

struct Selection {
    unsigned left = 0, top = 0, width = 0, height = 0;
};

struct BufferInfo {
    unsigned index = 0;
    unsigned length = 0; // plane 0 length in bytes
    unsigned mem_offset = 0; // plane 0 mmap offset
    uint32_t flags = 0; // QUERYBUF flags
};

struct Dequeued {
    unsigned index = 0;
    uint32_t flags = 0;
    uint64_t timestamp_us = 0;
    unsigned bytesused = 0;
    unsigned data_offset = 0;
    bool last() const { return flags & V4L2_BUF_FLAG_LAST; }
    bool error() const { return flags & V4L2_BUF_FLAG_ERROR; }
};

enum class Event { None, SourceChange, Eos };

class Device {
public:
    virtual ~Device() = default;

    virtual std::string path() const = 0;
    virtual int fd() const = 0;

    // Format negotiation.
    virtual FormatInfo get_format(Queue queue) = 0;
    virtual FormatInfo set_format(Queue queue, const FormatInfo& request) = 0;
    virtual std::optional<Selection> get_compose(Queue queue) = 0;
    virtual bool format_supported(Queue queue, uint32_t pixelformat) = 0;

    // Controls.
    virtual std::optional<int32_t> get_control(uint32_t id) = 0;
    // Returns false if the control does not exist; throws on other errors.
    virtual bool set_control(uint32_t id, int32_t value) = 0;

    // Buffers.
    // memory: V4L2_MEMORY_MMAP (driver-owned) or V4L2_MEMORY_DMABUF (client
    // buffers queued per picture, see queue_buffer_dmabuf).
    virtual unsigned request_buffers(Queue queue, unsigned count, uint32_t memory = V4L2_MEMORY_MMAP) = 0;
    virtual BufferInfo query_buffer(Queue queue, unsigned index) = 0;
    virtual int export_buffer(Queue queue, unsigned index) = 0; // returns fd
    virtual void* map_buffer(Queue queue, const BufferInfo& info) = 0;
    virtual void unmap_buffer(void* mapping, unsigned length) = 0;
    virtual void queue_buffer(Queue queue, unsigned index, unsigned bytesused, uint64_t timestamp_us,
        uint32_t flags) = 0;
    // One DMA-BUF of `size` bytes the decoder can write (system DMA heap);
    // the caller closes it. Used for the scratch buffer an Import pool lends
    // the firmware for LAST.
    virtual int allocate_dmabuf(unsigned size) = 0;
    // QBUF an external DMA-BUF into slot `index` of a DMABUF queue.
    virtual void queue_buffer_dmabuf(Queue queue, unsigned index, int dmabuf_fd, unsigned length,
        unsigned bytesused, uint64_t timestamp_us, uint32_t flags) = 0;
    // Non-blocking. nullopt on EAGAIN; throws iris::Error on EIO/EPIPE.
    virtual std::optional<Dequeued> dequeue_buffer(Queue queue) = 0;

    // Streaming and commands.
    virtual void stream(Queue queue, bool on) = 0;
    virtual void subscribe(uint32_t event_type) = 0;
    virtual Event dequeue_event() = 0;
    // Returns false on EBUSY (retryable), true on success, throws otherwise.
    virtual bool decoder_command(uint32_t cmd) = 0;

    // Wait for POLLIN/POLLPRI/POLLOUT activity; returns revents or 0 on timeout.
    virtual short poll(short events, int timeout_ms) = 0;
};

// Open the Iris decoder node. With an explicit path the node is verified to
// be an iris_driver M2M decoder; otherwise /dev/video* is scanned for the
// first node whose QUERYCAP driver is "iris_driver" and which enumerates a
// compressed OUTPUT format. Cameras never match.
std::unique_ptr<Device> open_device(const std::string& explicit_path);

} // namespace iris
