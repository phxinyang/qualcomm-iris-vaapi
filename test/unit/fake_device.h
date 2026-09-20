// SPDX-License-Identifier: MIT
//
// A scripted stand-in for the Iris decoder node. It models the parts of the
// stateful contract the Session depends on: SOURCE_CHANGE after the first
// OUTPUT queue, completion of one CAPTURE per OUTPUT in decode or display
// order, LAST on STOP, EBUSY on START while an OUTPUT is queued, error
// markers, and a mid-stream resolution change. Tests use it to assert the
// ioctl sequence and the ownership rules without hardware.

#pragma once

#include "../../src/iris/device.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
}

namespace iris::test {

struct FakeConfig {
    bool decode_order_control = true; // kernel has the display-delay patch
    bool display_order_reorder = false; // hold every Nth completion until STOP
    unsigned reorder_depth = 2;
    unsigned capture_max = 64;
    unsigned firmware_min_capture = 4;
    unsigned coded_width = 1920, coded_height = 1088;
    unsigned visible_width = 1920, visible_height = 1080;
    unsigned stride = 1920;
    unsigned au_capacity = 2u << 20;
};

class FakeDevice final : public Device {
public:
    explicit FakeDevice(FakeConfig config = {})
        : config_(config)
    {
    }

    // ----- scripting knobs -----
    // Fail the next N OUTPUT completions with the ERROR flag.
    void inject_output_errors(unsigned count) { output_errors_ = count; }
    // Emit N firmware error markers (ERROR flag, timestamp 0) on CAPTURE.
    void inject_error_markers(unsigned count) { error_markers_ = count; }
    // Return EBUSY from START this many times.
    void inject_start_busy(unsigned count) { start_busy_ = count; }
    // After the next AU, announce a new resolution.
    void inject_resolution_change(unsigned width, unsigned height)
    {
        pending_drc_ = { width, height };
    }
    // Make the next DQBUF on CAPTURE fail with EIO.
    void inject_capture_eio() { capture_eio_ = true; }
    // Stop completing until released (models a stalled reorder window).
    void stall(bool on) { stalled_ = on; }
    // By default every poll() completes one queued AU, modelling firmware
    // progress while the driver waits. Off = only explicit tick().
    void set_auto_tick(bool on) { auto_tick_ = on; }
    // Advance the firmware: complete up to `n` queued AUs.
    void tick(unsigned n = 1);

    const std::vector<std::string>& log() const { return log_; }
    std::string joined_log() const;
    unsigned capture_queued() const { return static_cast<unsigned>(capture_queue_.size()); }
    unsigned capture_allocated() const { return capture_count_; }
    bool output_streaming() const { return output_on_; }
    bool capture_streaming() const { return capture_on_; }
    int32_t control(uint32_t id) const
    {
        auto it = controls_.find(id);
        return it == controls_.end() ? -1 : it->second;
    }

    // ----- Device -----
    std::string path() const override { return "/dev/fake-iris"; }
    int fd() const override { return -1; }
    FormatInfo get_format(Queue queue) override;
    FormatInfo set_format(Queue queue, const FormatInfo& request) override;
    std::optional<Selection> get_compose(Queue queue) override;
    bool format_supported(Queue, uint32_t) override { return true; }
    std::optional<int32_t> get_control(uint32_t id) override;
    bool set_control(uint32_t id, int32_t value) override;
    unsigned request_buffers(Queue queue, unsigned count) override;
    BufferInfo query_buffer(Queue queue, unsigned index) override;
    int export_buffer(Queue queue, unsigned index) override;
    void* map_buffer(Queue queue, const BufferInfo& info) override;
    void unmap_buffer(void* mapping, unsigned length) override;
    void queue_buffer(Queue queue, unsigned index, unsigned bytesused, uint64_t timestamp_us, uint32_t flags)
        override;
    std::optional<Dequeued> dequeue_buffer(Queue queue) override;
    void stream(Queue queue, bool on) override;
    void subscribe(uint32_t) override { }
    Event dequeue_event() override;
    bool decoder_command(uint32_t cmd) override;
    short poll(short events, int timeout_ms) override;

private:
    struct QueuedOutput {
        unsigned index;
        uint64_t timestamp;
    };
    void note(const std::string& line) { log_.push_back(line); }
    void complete_one();

    FakeConfig config_;
    std::vector<std::string> log_;
    std::map<uint32_t, int32_t> controls_;
    bool output_on_ = false, capture_on_ = false;
    bool source_change_pending_ = false;
    bool output_format_set_ = false;
    unsigned output_count_ = 0, capture_count_ = 0;
    std::vector<std::vector<uint8_t>> output_memory_;
    std::deque<QueuedOutput> output_queue_; // AUs the firmware has not consumed
    std::deque<unsigned> output_done_; // OUTPUT indices ready for DQBUF
    std::deque<uint32_t> output_done_flags_;
    std::deque<unsigned> capture_queue_; // free CAPTURE slots the firmware owns
    std::deque<Dequeued> capture_done_; // completions ready for DQBUF
    std::deque<QueuedOutput> reorder_hold_; // display-order: held until STOP
    bool stopped_ = false;
    bool stalled_ = false;
    bool drc_blocked_ = false; // new-resolution AU waits for CAPTURE restart
    unsigned output_errors_ = 0, error_markers_ = 0, start_busy_ = 0;
    std::optional<std::pair<unsigned, unsigned>> pending_drc_;
    bool capture_eio_ = false;
    bool auto_tick_ = true;
    unsigned decoded_since_stream_ = 0;
};

} // namespace iris::test
