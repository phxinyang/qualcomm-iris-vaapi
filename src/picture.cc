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

#include "picture.h"

#include "trace.h"

#include <cassert>
#include <cstring>
#include <functional>
#include <cstdlib>
#include <cstdio>
#include <system_error>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <va/va.h>
}

#include "context.h"
#include "driver.h"
#include "media.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"

using fourcc = uint32_t;

VAStatus beginPicture(VADriverContextP va_context, VAContextID context_id, VASurfaceID surface_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    // Surface/context lifetime is shared with destroySurfaces() and
    // destroyContext(). Keep the same driver -> context lock order used by
    // syncSurface() so map lookups cannot race teardown.
    std::lock_guard<std::recursive_mutex> driver_guard(driver_data->mutex);

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    auto& context = *driver_data->contexts.at(context_id);
    std::lock_guard<std::recursive_mutex> guard(context.synchronization_mutex());

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);
    const auto owner_context = driver_data->contexts.find(surface.owner_context);
    if (surface.owner_context != VA_INVALID_ID && surface.owner_context != context_id
        && owner_context != driver_data->contexts.end()
        && owner_context->second->uses_stateful_streaming() && context.uses_stateful_streaming()) {
        if (trace_enabled())
            std::fprintf(stderr, "va surface context mismatch surface=%u owner=%u caller=%u\n", surface_id,
                surface.owner_context, context_id);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (trace_enabled())
        std::fprintf(stderr, "va begin_picture tid=%ld ctx=%u surface=%u status=%u initialized=%d\n",
            static_cast<long>(syscall(SYS_gettid)), context_id, surface_id, surface.status, context.initialized());
    if (trace_enabled())
        error_log(va_context, "trace begin surface=%u status=%u initialized=%d\\n", surface_id, surface.status,
            context.initialized());

    if (!context.initialized()) {
        std::vector<VASurfaceID> surface_ids;
        surface_ids.push_back(surface_id);
        for (const auto& [id, candidate] : driver_data->surfaces) {
            // Surface export may have populated destination_buffer before the
            // first decode call. Include those still-ready targets as well;
            // otherwise stateful batching sees only the first surface and
            // immediately runs out of bindings on the second picture.
            if (id != surface_id && candidate.format == surface.format
                && candidate.width == surface.width && candidate.height == surface.height
                && candidate.status == VASurfaceReady) {
                surface_ids.push_back(id);
            }
        }
        try {
            context.initialize(surface_ids);
        } catch (const std::exception& e) {
            error_log(va_context, "Unable to initialize V4L2 queues: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (surface.status == VASurfaceRendering) {
        // A Surface owns one input bitstream scratch area. It cannot safely be
        // reused while the previous picture is still in flight: appending the
        // next AU would retain the previous AU's bytes and eventually make the
        // stateful OUTPUT batch invalid. Reap the completed CAPTURE frame
        // before accepting a new picture for either V4L2 mode.
        if (syncSurface(va_context, surface_id) != VA_STATUS_SUCCESS || surface.status == VASurfaceRendering)
            return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    if (context.initialized() && context.uses_stateful_streaming()
        && (surface.width != static_cast<unsigned>(context.picture_width)
            || surface.height != static_cast<unsigned>(context.picture_height))) {
        // Exporting a future Chrome surface pool is deliberately side-effect
        // free. Move the stateful decoder to that geometry only when the first
        // picture for the pool is actually submitted.
        try {
            surface.capture_source_layout.clear();
            if (!context.reconfigure_stateful_dimensions(surface_id)) {
                error_log(va_context, "Unable to reconfigure stateful decoder for surface %u\n", surface_id);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        } catch (const std::exception& e) {
            error_log(va_context, "Unable to reconfigure stateful decoder: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (!context.bind_surface(surface_id)) {
        error_log(va_context, "No free V4L2 buffer for surface %u\n", surface_id);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    // Pooled direct-export zero-copy holds the completed MMAP slot until
    // the VA surface is reused. Return it here so decode never stalls with
    // the whole pool held for display; the next completion adopts a free
    // slot. This is the same requeue-on-reuse discipline as the reference
    // MMAP pool, and compositor-held frames stay valid because EXPBUF hands
    // out dup'd fds, not the queue slot itself.
    if ((!context.uses_stateful_streaming() || context.capture_started()) && !context.capture_draining()
        && (!context.capture_uses_dmabuf() || context.zero_copy_direct_enabled())
        && !surface.destination_buffer_queued && surface.destination_buffer
        && (surface.has_completed_frame || !context.zero_copy_direct_enabled())) {
        try {
            surface.destination_buffer->get().queue();
            surface.destination_buffer_queued = true;
        } catch (std::system_error& e) {
            error_log(va_context, "Unable to requeue capture buffer: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    surface.status = VASurfaceRendering;
    surface.release_error = false;
    context.begin_surface(surface_id);

    return VA_STATUS_SUCCESS;
}

VAStatus renderPicture(VADriverContextP va_context, VAContextID context_id, VABufferID* buffers_ids, int buffers_count)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);
    int rc;
    int i;

    std::lock_guard<std::recursive_mutex> driver_guard(driver_data->mutex);

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    const auto& context = *driver_data->contexts.at(context_id);
    std::lock_guard<std::recursive_mutex> guard(context.synchronization_mutex());

    const auto render_surface_id = context.current_surface();
    if (!driver_data->surfaces.contains(render_surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    const auto& render_surface = driver_data->surfaces.at(render_surface_id);
    if (!render_surface.source_buffer && !context.uses_stateful_streaming()) {
        error_log(va_context, "Surface %u has no V4L2 source buffer\n", render_surface_id);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    for (i = 0; i < buffers_count; i++) {
        if (!driver_data->buffers.contains(buffers_ids[i])) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        rc = context.store_buffer(driver_data->buffers.at(buffers_ids[i]));
        if (rc != VA_STATUS_SUCCESS)
            return rc;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus endPicture(VADriverContextP va_context, VAContextID context_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);
    VAStatus status;

    std::lock_guard<std::recursive_mutex> driver_guard(driver_data->mutex);

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    auto& context = *driver_data->contexts.at(context_id);
    std::lock_guard<std::recursive_mutex> guard(context.synchronization_mutex());
    const auto render_surface_id = context.current_surface();
    if (!driver_data->surfaces.contains(render_surface_id))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    auto& surface = driver_data->surfaces.at(render_surface_id);
    if (trace_enabled()) {
        struct timespec wall = {};
        clock_gettime(CLOCK_REALTIME, &wall);
        std::fprintf(stderr, "va end_picture ctx=%u surface=%u bytes=%u stateful=%d wall=%lld%03ld\n", context_id,
            render_surface_id, surface.source_size_used, context.uses_stateful_streaming(),
            static_cast<long long>(wall.tv_sec), wall.tv_nsec / 1000000);
    }
    if (trace_enabled())
        error_log(va_context, "trace end surface=%u bytes=%u request=%d\\n", render_surface_id,
            surface.source_size_used, surface.request_fd);
    if (std::getenv("V4L2_VA_DUMP") && surface.source_size_used > 0) {
        const char* dump_dir = std::getenv("V4L2_VA_DUMP");
        char path[256];
        std::snprintf(path, sizeof(path), "%s/va-au-%u.bin", dump_dir, render_surface_id);
        if (FILE* file = std::fopen(path, "wb")) {
            const uint8_t* dump_src = context.uses_stateful_streaming()
                ? surface.stateful_bitstream.data()
                : surface.source_buffer->get().mapping()[0].data();
            std::fwrite(dump_src, 1, surface.source_size_used, file);
            std::fclose(file);
        }
    }
    if (!surface.source_buffer || (!surface.destination_buffer && !context.uses_stateful_streaming())) {
        error_log(va_context, "Surface %u has incomplete V4L2 bindings (source=%d destination=%d)\n",
            render_surface_id, surface.source_buffer.has_value(), surface.destination_buffer.has_value());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    // V4L2's TIMESTAMP_COPY contract uses CLOCK_MONOTONIC. gettimeofday()
    // produces wall-clock seconds since 1970, which Iris does not match to
    // its firmware timestamps and can leave CAPTURE permanently pending.
    timespec monotonic = {};
    clock_gettime(CLOCK_MONOTONIC, &monotonic);
    surface.timestamp.tv_sec = monotonic.tv_sec;
    surface.timestamp.tv_usec = monotonic.tv_nsec / 1000;

    if (context.device.media_fd >= 0 && context.uses_request_api()) {
        if (surface.request_fd < 0) {
            surface.request_fd = media_request_alloc(context.device.media_fd);
        }

        status = context.set_controls();
        if (status != VA_STATUS_SUCCESS)
            return status;
    }

    if (context.uses_stateful_streaming()) {
        status = context.stateful_submit_picture(render_surface_id);
        if (status != VA_STATUS_SUCCESS) {
            // A codec translator can reject a VA payload before an OUTPUT AU
            // is queued (for example AV1 tile data without its OBU headers).
            // Release the rendering target immediately so the caller gets a
            // clean decode error instead of repeatedly seeing SURFACE_BUSY or
            // invalid-surface failures on the same target.
            context.discard_stateful_surface(render_surface_id);
            surface.status = VASurfaceReady;
            surface.source_size_used = 0;
        }
        context.end_surface();
        memset(&surface.params, 0, sizeof(surface.params));
        return status;
    }

    try {
        // Iris uses the OUTPUT timestamp when matching decoded CAPTURE
        // frames and DPB references. Keep it populated for stateful streams
        // as well as request-api decoders.
        surface.source_buffer->get().queue(surface.request_fd, &surface.timestamp, surface.source_size_used);
        surface.source_buffer_queued = true;
        if (context.uses_stateful_streaming() && !context.device.output_streaming) {
            context.device.stream_output(true);
        }
    } catch (std::system_error& e) {
        error_log(va_context, "Unable to queue buffer: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (surface.request_fd >= 0) {
        try {
            media_request_queue(surface.request_fd);
            media_request_wait_completion(surface.request_fd);
            media_request_reinit(surface.request_fd);
        } catch (std::runtime_error& e) {
            close(surface.request_fd);
            surface.request_fd = -1;
            error_log(va_context, "Failed to process request: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (!context.uses_request_api() && !context.start_capture()) {
        error_log(va_context, "Stateful decoder did not report a source-change event\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    surface.source_size_used = 0;

    context.end_surface();
    memset(&surface.params, 0, sizeof(surface.params));

    return VA_STATUS_SUCCESS;
}
