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
#include <deque>

extern "C" {
#include <va/va_backend.h>
}

#include "buffer.h"
#include "v4l2.h"

struct DriverData;

class Context {
public:
    static Context* create(DriverData* driver_data, VAProfile profile, int picture_width, int picture_height,
        std::span<VASurfaceID> surface_ids);
    static std::set<VAProfile> supported_profiles(const std::vector<V4L2M2MDevice>& devices);

    Context(DriverData* driver_data, V4L2M2MDevice& device, fourcc pixelformat, int picture_width, int picture_height,
        std::span<VASurfaceID> surface_ids);
    virtual ~Context();

    void initialize(std::span<VASurfaceID> surface_ids);
    bool start_capture();
    VAStatus append_stateful_picture(VASurfaceID surface_id);
    VAStatus flush_stateful_batch();
    // Chrome submits VA pictures asynchronously. Reap any completed V4L2
    // CAPTURE/OUTPUT buffers before claiming another OUTPUT slot.
    void service_stateful_queues();
    void discard_stateful_error_frame();
    void mark_source_buffer_dequeued(unsigned index);
    bool has_pending_stateful_batch() const { return !stateful_pending.empty(); }
    bool capture_draining() const { return stateful_draining; }
    bool has_queued_stateful_output() const;
    void discard_stateful_surface(VASurfaceID surface_id);
    bool has_stateful_history() const { return stateful_submitted_count != 0; }
    void reset_stateful_decoder();
    // Flush the firmware reorder queue before STREAMOFF. Stateful V4L2
    // STREAMOFF discards any frames still held in the decoder DPB.
    void drain_stateful_decoder();
    void resume_after_drain();
    bool initialized() const { return queues_initialized; }
    bool capture_started() const { return capture_initialized; }
    bool bind_surface(VASurfaceID surface_id);
    void begin_surface(VASurfaceID surface_id);
    void end_surface();
    VASurfaceID current_surface() const;
    std::optional<VASurfaceID> surface_for_buffer(v4l2_buf_type type, unsigned index) const;
    std::optional<VASurfaceID> surface_for_timestamp(const timeval& timestamp, uint32_t capture_flags = 0);
    std::optional<VASurfaceID> surface_for_capture_flags(uint32_t capture_flags);

    virtual VAStatus store_buffer(const Buffer& buffer) const = 0;
    virtual int set_controls() = 0;
    // Stateful V4L2 decoders submit ordinary QBUF operations. Stateless
    // request-api decoders attach controls and their output buffer to a media
    // request before queueing it.
    virtual bool uses_request_api() const { return true; }
    virtual bool uses_stateful_streaming() const { return false; }
    virtual bool stateful_sequence_start(VASurfaceID) { return false; }

    int picture_width;
    int picture_height;
    DriverData* driver_data;
    V4L2M2MDevice& device;

private:
    fourcc pixelformat;
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
    bool stateful_last_marker_seen = false;
    bool stateful_queue_restart_pending = false;
    timeval stateful_last_timestamp = {};
};

VAStatus createContext(VADriverContextP va_context, VAConfigID config_id, int picture_width, int picture_height,
    int flags, VASurfaceID* surfaces_ids, int surfaces_count, VAContextID* context_id);
VAStatus destroyContext(VADriverContextP va_context, VAContextID context_id);
