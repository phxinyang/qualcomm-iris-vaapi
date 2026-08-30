/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2023 Max Schettler <max.schettler@posteo.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <span>
#include <map>
#include <set>
#include <vector>
#include <optional>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

extern "C" {
#include <va/va_backend.h>
}

#include "buffer.h"
#include "v4l2.h"

struct DriverData;

// Stateful capture normally keeps the complete CAPTURE pool queued. The
// optional scheduled mode is deliberately opt-in because it can starve
// firmware reorder/flush paths when a surface is not yet associated.
    bool stateful_capture_scheduled();

class Context {
public:
    static Context* create(DriverData* driver_data, VAProfile profile, int picture_width, int picture_height,
        std::span<VASurfaceID> surface_ids);
    static std::set<VAProfile> supported_profiles(const V4L2M2MDevice& device);
    static std::set<VAProfile> supported_profiles(const std::deque<V4L2M2MDevice>& devices);

    Context(DriverData* driver_data, V4L2M2MDevice& device, fourcc pixelformat, int picture_width, int picture_height,
        std::span<VASurfaceID> surface_ids);
    virtual ~Context();

    void initialize(std::span<VASurfaceID> surface_ids);
    bool start_capture();
    // True when the capture queue is backed directly by the stable DMA-BUFs
    // exported for the VA surfaces. This is an opt-in experiment; MMAP queues
    // continue through the existing copy path.
    bool capture_uses_dmabuf() const;
    bool capture_slots_scheduled() const;
    bool zero_copy_codec_supported() const;
    bool zero_copy_capture_allowed() const { return !zero_copy_disabled_; }
    void queue_zero_copy_capture();
    void queue_zero_copy_drain_capture();
    VAStatus append_stateful_picture(VASurfaceID surface_id);
    VAStatus flush_stateful_batch();
    // Chrome submits VA pictures asynchronously. Reap any completed V4L2
    // CAPTURE/OUTPUT buffers before claiming another OUTPUT slot.
    void service_stateful_queues();
    void discard_stateful_error_frame();
    void remove_stateful_batch_order(unsigned index);
    void mark_source_buffer_dequeued(unsigned index);
    bool has_pending_stateful_batch() const { return !stateful_pending.empty(); }
    bool capture_draining() const { return stateful_draining; }
    bool has_queued_stateful_output() const;
    // Permit at most one HEVC timeout recovery after enough history has been
    // submitted to distinguish a startup delay from a trailing reorder.
    bool try_begin_stateful_timeout_recovery();
    void reset_stateful_timeout_recovery() { stateful_timeout_recovery_used = false; }
    void discard_stateful_surface(VASurfaceID surface_id);
    bool has_stateful_history() const { return stateful_submitted_count != 0; }
    unsigned stateful_submitted_frames() const { return stateful_submitted_count; }
    unsigned stateful_completed_frames() const { return stateful_completed_count; }

    // Whether a full cold-start wait has already been spent without the
    // decoder producing anything. Once that has happened the generous startup
    // allowance is not worth paying again, because the stream is not going to
    // decode at all.
    bool stateful_cold_start_exhausted() const { return stateful_cold_start_exhausted_; }
    void mark_stateful_cold_start_exhausted() { stateful_cold_start_exhausted_ = true; }
    // Pictures that timed out while the decoder has still never produced a
    // single frame. A stream the firmware cannot handle at all is only
    // distinguishable from a slow one by how long this goes on.
    unsigned stateful_barren_syncs() const { return stateful_barren_syncs_; }
    void note_stateful_barren_sync() { stateful_barren_syncs_++; }
    // A stateful STOP is complete only after CAPTURE returns V4L2_BUF_FLAG_LAST.
    // Keep the result explicit so callers never restart a decoder on timeout.
    bool reset_stateful_decoder();
    bool drain_stateful_decoder();
    void resume_after_drain();
    // Chrome never calls back into the backend once it has submitted the last
    // access unit, so an idle watchdog is the only place that can notice the
    // end of a stream and STOP-drain the pictures Iris still holds.
    void note_stateful_submission();
    bool stateful_input_consumed() const;
    bool initialized() const { return queues_initialized; }
    bool capture_started() const { return capture_initialized; }
    std::recursive_mutex& synchronization_mutex() const { return synchronization_mutex_; }
    bool bind_surface(VASurfaceID surface_id);
    // VA clients may reuse one context while allocating a new surface pool
    // after a mid-stream resolution change. Rebuild the stateful V4L2 queues
    // before the first AU for the new geometry is submitted.
    bool reconfigure_stateful_dimensions(VASurfaceID surface_id);
    void begin_surface(VASurfaceID surface_id);
    void end_surface();
    VASurfaceID current_surface() const;
    std::optional<VASurfaceID> surface_for_buffer(v4l2_buf_type type, unsigned index) const;
    // Copy the timestamp before matching. V4L2 DQBUF updates the device's
    // scratch timestamp on every dequeue, and callers may service another
    // queue while resolving the current CAPTURE buffer.
    std::optional<VASurfaceID> surface_for_timestamp(timeval timestamp, uint32_t capture_flags = 0);
    std::optional<VASurfaceID> surface_for_capture_flags(uint32_t capture_flags);

    virtual VAStatus store_buffer(const Buffer& buffer) const = 0;
    virtual int set_controls() = 0;
    // Stateful V4L2 decoders submit ordinary QBUF operations. Stateless
    // request-api decoders attach controls and their output buffer to a media
    // request before queueing it.
    virtual bool uses_request_api() const { return true; }
    virtual bool uses_stateful_streaming() const { return false; }
    virtual bool stateful_timeout_drain() const { return uses_stateful_streaming(); }
    virtual unsigned stateful_frame_type(VASurfaceID) const { return 255; }
    virtual bool stateful_sequence_start(VASurfaceID) { return false; }

    // Submission seam for codecs that cannot finish an access unit at
    // vaEndPicture time. AV1 is the only such case today: its frame header
    // needs refresh_frame_flags, which VA-API does not provide and which can
    // only be recovered from the following frame's reference map.
    virtual VAStatus stateful_submit_picture(VASurfaceID surface_id)
    {
        return append_stateful_picture(surface_id);
    }
    // Release anything a codec is still holding back. Called before any drain
    // or reset so a deferred access unit cannot be stranded across an EOS.
    virtual VAStatus stateful_flush_deferred() { return VA_STATUS_SUCCESS; }

    int picture_width;
    int picture_height;
    DriverData* driver_data;
    V4L2M2MDevice& device;

private:
    fourcc pixelformat;
    bool stateful_cold_start_exhausted_ = false;
    unsigned stateful_barren_syncs_ = 0;
    bool queues_initialized;
    bool capture_initialized;
    std::map<VASurfaceID, unsigned> surface_buffer_indices;
    std::vector<VASurfaceID> surface_ids;
    std::map<unsigned, std::vector<VASurfaceID>> stateful_batches;
    std::map<std::pair<long long, long>, unsigned> stateful_batch_timestamps;
    std::set<unsigned> stateful_output_dequeued;
    std::set<unsigned> stateful_capture_done;
    std::deque<unsigned> stateful_batch_order;
    // Sequence-start decisions must be captured while VA H.264 parameters
    // are still attached to the surface. endPicture() clears them after
    // append_stateful_picture() returns, before the batch is flushed.
    std::set<VASurfaceID> stateful_sequence_starts;
    std::vector<VASurfaceID> stateful_pending;
    size_t stateful_pending_size = 0;
    bool stateful_draining = false;
    unsigned stateful_submitted_count = 0;
    unsigned stateful_completed_count = 0;
    bool stateful_last_marker_seen = false;
    bool stateful_timeout_recovery_used = false;
    bool stateful_queue_restart_pending = false;
    bool zero_copy_disabled_ = false;
    timeval stateful_last_timestamp = {};
    mutable std::recursive_mutex synchronization_mutex_;

    void stateful_watchdog_loop();
    void stop_stateful_watchdog();
    std::thread stateful_watchdog_;
    std::mutex stateful_watchdog_mutex_;
    std::condition_variable stateful_watchdog_cv_;
    std::chrono::steady_clock::time_point stateful_last_submission_ = {};
    bool stateful_watchdog_stop_ = false;
    bool stateful_watchdog_handled_ = false;
    bool source_change_subscribed_ = false;
};

VAStatus createContext(VADriverContextP va_context, VAConfigID config_id, int picture_width, int picture_height,
    int flags, VASurfaceID* surfaces_ids, int surfaces_count, VAContextID* context_id);
VAStatus destroyContext(VADriverContextP va_context, VAContextID context_id);
