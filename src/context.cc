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

#include "context.h"

#include <cassert>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <limits>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va.h>
}

#include "config.h"
#include "driver.h"
#include "format.h"
#include "h264.h"
#include "mpeg2.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"
#include "vp8.h"
#ifdef ENABLE_VP9
#include "vp9.h"
#endif

namespace {
thread_local std::unordered_map<const Context*, VASurfaceID> current_surfaces;
}

std::set<VAProfile> Context::supported_profiles(const std::vector<V4L2M2MDevice>& devices)
{
    std::set<VAProfile> result;
    for (auto&& device : devices) {
        for (auto&& profile : MPEG2Context::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : H264Context::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : VP8Context::supported_profiles(device)) {
            result.insert(profile);
        }
#ifdef ENABLE_VP9
        for (auto&& profile : VP9Context::supported_profiles(device)) {
            result.insert(profile);
        }
#endif
    }
    return result;
}

Context* Context::create(DriverData* driver_data, VAProfile profile, int picture_width, int picture_height,
    std::span<VASurfaceID> surface_ids)
{
    for (auto&& device : driver_data->devices) {
        if (MPEG2Context::supported_profiles(device).contains(profile)) {
            return new MPEG2Context(driver_data, device, picture_width, picture_height, surface_ids);
        }
        if (H264Context::supported_profiles(device).contains(profile)) {
            return new H264Context(driver_data, device, profile, picture_width, picture_height, surface_ids);
        }
        if (VP8Context::supported_profiles(device).contains(profile)) {
            return new VP8Context(driver_data, device, picture_width, picture_height, surface_ids);
        }
#ifdef ENABLE_VP9
        if (VP9Context::supported_profiles(device).contains(profile)) {
            return new VP9Context(driver_data, device, picture_width, picture_height, surface_ids);
        }
#endif
    }

    throw std::invalid_argument("Unimplemented profile");
}

Context::Context(DriverData* driver_data, V4L2M2MDevice& dev, fourcc pixelformat, int picture_width, int picture_height,
    std::span<VASurfaceID> surface_ids)
    : picture_width(picture_width)
    , picture_height(picture_height)
    , driver_data(driver_data)
    , device(dev)
    , pixelformat(pixelformat)
    , queues_initialized(false)
    , capture_initialized(false)
{
    if (!surface_ids.empty())
        initialize(surface_ids);
}

Context::~Context()
{
    if (queues_initialized) {
        device.set_streaming(false);
        device.request_buffers(device.capture_buf_type, 0);
        device.request_buffers(device.output_buf_type, 0);
    }
}

void Context::initialize(std::span<VASurfaceID> surface_ids)
{
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va context initialize this=%p surfaces=%zu initialized=%d\n", this, surface_ids.size(),
            queues_initialized);
    if (queues_initialized)
        return;
    if (surface_ids.empty())
        throw std::invalid_argument("No surfaces to initialize");

    this->surface_ids.assign(surface_ids.begin(), surface_ids.end());

    if (uses_stateful_streaming()) {
        const auto& first_surface = driver_data->surfaces.at(surface_ids.front());
        auto format = std::ranges::find_if(formats, [&](const auto& candidate) {
            return candidate.va.rt_format == first_surface.format
                && device.format_supported(device.capture_buf_type, candidate.v4l2.format);
        });
        if (format == formats.end())
            throw std::invalid_argument("No matching stateful capture format");
        // Iris requires an initial CAPTURE S_FMT before OUTPUT S_FMT. The
        // driver rounds the visible height up (e.g. 368 -> 384); SOURCE_CHANGE
        // later refreshes this format after parsing the stream.
        device.set_format(device.capture_buf_type, format->v4l2.format, picture_width, picture_height);
    }

    device.set_format(device.output_buf_type, pixelformat, picture_width, picture_height);

    // The capture queue is configured after the compressed output format and
    // uses one buffer for each VA render target.
    // Iris pairs the stateful OUTPUT/CAPTURE queue sizes. Eight OUTPUT
    // buffers produce the six-buffer CAPTURE queue used by the native tool.
    constexpr unsigned reserved_buffers = 32;
    // For stateful streams the CAPTURE pool is created only after SOURCE_CHANGE
    // (matching the native v4l2-ctl client). Allocating it earlier makes
    // VIDIOC_DECODER_CMD(START) return EBUSY and the decoder never produce
    // frames.
    if (!uses_stateful_streaming())
        createSurfacesDeferred(driver_data, *this, surface_ids, reserved_buffers);

    if (device.request_buffers(device.output_buf_type, reserved_buffers) == 0)
        throw std::runtime_error("V4L2 output queue returned too few buffers");

    for (unsigned i = 0; i < surface_ids.size(); i++) {
        surface_buffer_indices.emplace(surface_ids[i], i);
        auto& surface = driver_data->surfaces.at(surface_ids[i]);
        if (uses_stateful_streaming()) {
            // OUTPUT index 0 is only an owner/identity for syncSurface; the
            // actual bitstream is copied into a rotating batch buffer later.
            surface.source_buffer = std::cref(device.buffer(device.output_buf_type, 0));
            surface.source_buffer_index = 0;
            surface.source_buffer_queued = false;
            surface.stateful_bitstream.resize(SOURCE_SIZE_MAX);
        } else if (i < device.buffer_count(device.output_buf_type)) {
            surface.source_buffer = std::cref(device.buffer(device.output_buf_type, i));
            surface.source_buffer_index = i;
            surface.source_buffer_queued = false;
        }
    }

    if (uses_stateful_streaming()) {
        device.subscribe_source_change();
        // Iris must see the first compressed buffer in the OUTPUT queue
        // before STREAMON. The first VA picture is not available until
        // endPicture(), so stream_output() is intentionally deferred there.
    } else {
        for (unsigned i = 0; i < device.buffer_count(device.capture_buf_type); i++)
            device.buffer(device.capture_buf_type, i).queue();
        for (auto& [surface_id, buffer_index] : surface_buffer_indices)
            driver_data->surfaces.at(surface_id).destination_buffer_queued = true;
        device.set_streaming(true);
        capture_initialized = true;
    }
    queues_initialized = true;
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va context initialized this=%p capture=%u output=%u\n", this,
            device.buffer_count(device.capture_buf_type), device.buffer_count(device.output_buf_type));
}

bool Context::start_capture()
{
    if (!uses_stateful_streaming() || capture_initialized)
        return true;

    if (!device.wait_for_source_change())
        return false;

    // The native v4l2-ctl client renegotiates the CAPTURE queue after the
    // SOURCE_CHANGE event: STREAMOFF, REQBUFS(0), S_FMT with the final
    // geometry, REQBUFS, re-queue, STREAMON. Rebuild the pool with the driver's
    // actual format rather than reusing the pre-stream buffers.
    if (device.capture_streaming)
        device.stream_capture(false);
    device.request_buffers(device.capture_buf_type, 0);
    device.refresh_capture_format();
    {
        const auto& pix = device.capture_format.fmt.pix_mp;
        const auto& v4l2_spec = lookup_format(pix.pixelformat).v4l2;
        device.set_format(device.capture_buf_type, v4l2_spec.format, pix.width, pix.height);
    }

    // Keep enough slots for VA's decoded-picture pool (Chromium commonly
    // exports 16-24 surfaces).
    const unsigned reserved_buffers = std::max<unsigned>(32, surface_ids.size());
    device.request_buffers(device.capture_buf_type, reserved_buffers);

    // Bind every VA surface to the freshly allocated MMAP objects.
    for (auto& [surface_id, buffer_index] : surface_buffer_indices)
        bind_surface(surface_id);

    for (unsigned i = 0; i < device.buffer_count(device.capture_buf_type); i++)
        device.buffer(device.capture_buf_type, i).queue();
    for (auto& [surface_id, buffer_index] : surface_buffer_indices) {
        auto& surface = driver_data->surfaces.at(surface_id);
        if (buffer_index < device.buffer_count(device.capture_buf_type))
            surface.destination_buffer_queued = true;
    }
    device.stream_capture(true);
    capture_initialized = true;
    // CAPTURE is now streaming, so re-bind every VA surface to its backing
    // MMAAP buffer (the first bind ran before capture_initialized was set and
    // could only reserve the ordinal + source buffer).
    for (auto& [surface_id, buffer_index] : surface_buffer_indices)
        bind_surface(surface_id);
    if (std::getenv("V4L2_VA_WAIT_FIRST")) {
        auto capture_index = device.dequeue_ready(device.capture_buf_type, 2000);
        std::fprintf(stderr, "v4l2 first capture=%s\\n", capture_index ? std::to_string(*capture_index).c_str() : "none");
        if (capture_index)
            device.buffer(device.capture_buf_type, *capture_index).queue();
    }
    return true;
}

VAStatus Context::append_stateful_picture(VASurfaceID surface_id)
{
    if (!uses_stateful_streaming() || !driver_data->surfaces.contains(surface_id))
        return VA_STATUS_ERROR_OPERATION_FAILED;
    auto& surface = driver_data->surfaces.at(surface_id);
    if (surface.source_size_used == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (stateful_pending.empty())
        stateful_pending_timestamp = surface.timestamp;
    if (stateful_pending_size + surface.source_size_used > SOURCE_SIZE_MAX)
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    stateful_pending.push_back(surface_id);
    stateful_pending_size += surface.source_size_used;
    // Eight pictures fit comfortably in one Iris MMAP OUTPUT buffer and
    // match the queue depth used by native clients.
    if (stateful_pending.size() >= 8)
        return flush_stateful_batch();
    return VA_STATUS_SUCCESS;
}

VAStatus Context::flush_stateful_batch()
{
    if (!uses_stateful_streaming() || stateful_pending.empty())
        return VA_STATUS_SUCCESS;
    if (stateful_draining)
        return VA_STATUS_SUCCESS;

    unsigned batch_index = std::numeric_limits<unsigned>::max();
    for (unsigned i = 0; i < device.buffer_count(device.output_buf_type); i++) {
        if (!stateful_batches.contains(i)) {
            batch_index = i;
            break;
        }
    }
    if (batch_index == std::numeric_limits<unsigned>::max())
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    // Iris consumes one compressed access unit per OUTPUT buffer; submit each
    // pending picture independently (the native v4l2-ctl client does the same).
    try {
        while (!stateful_pending.empty()) {
            unsigned batch_index = std::numeric_limits<unsigned>::max();
            for (unsigned i = 0; i < device.buffer_count(device.output_buf_type); i++) {
                if (!stateful_batches.contains(i)) {
                    batch_index = i;
                    break;
                }
            }
            if (batch_index == std::numeric_limits<unsigned>::max())
                return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

            VASurfaceID surface_id = stateful_pending.front();
            auto& surface = driver_data->surfaces.at(surface_id);
            auto destination = device.buffer(device.output_buf_type, batch_index).mapping()[0];
            if (surface.source_size_used > destination.size())
                return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
            std::memcpy(destination.data(), surface.stateful_bitstream.data(), surface.source_size_used);

            device.buffer(device.output_buf_type, batch_index)
                .queue(-1, &surface.timestamp, surface.source_size_used);
            surface.source_buffer_queued = true;
            stateful_batches.emplace(batch_index, std::vector<VASurfaceID>{surface_id});
            stateful_pending.erase(stateful_pending.begin());
            stateful_pending_size -= surface.source_size_used;
            if (!device.output_streaming)
                device.stream_output(true);
            if (!capture_initialized) {
                // Issued after STREAMON and before CAPTURE is allocated so it
                // does not return EBUSY; Iris only decodes once START arrives.
                bool started = false;
                for (int attempt = 0; attempt < 50 && !started; attempt++) {
                    try {
                        device.decoder_start();
                        started = true;
                    } catch (const std::system_error&) {
                        // Firmware may still be moving LOAD_RESOURCES ->
                        // STREAMING; retry briefly before falling back to the
                        // SOURCE_CHANGE handshake.
                        struct timespec delay = { 0, 10 * 1000 * 1000 };
                        nanosleep(&delay, nullptr);
                    }
                }
                if (!start_capture())
                    return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        }
        stateful_pending_timestamp = {};
    } catch (const std::system_error&) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

void Context::mark_source_buffer_dequeued(unsigned index)
{
    auto batch = stateful_batches.find(index);
    if (batch == stateful_batches.end())
        return;
    for (auto surface_id : batch->second)
        driver_data->surfaces.at(surface_id).source_buffer_queued = false;
    stateful_batches.erase(batch);
}

void Context::resume_after_drain()
{
    if (!stateful_draining)
        return;
    // STREAMON/STOP returns completed CAPTURE buffers to userspace. Requeue
    // only targets that are no longer being rendered before restarting the
    // firmware; otherwise the resumed batch can run with an empty queue.
    for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
        auto& surface = driver_data->surfaces.at(surface_id);
        if (surface.destination_buffer && !surface.destination_buffer_queued
            && surface.status != VASurfaceRendering) {
            surface.destination_buffer->get().queue();
            surface.destination_buffer_queued = true;
        }
    }
    device.decoder_start();
    stateful_draining = false;
    // A producer may have accumulated the next VA pictures while the
    // previous drain was completing. Submit them only after START succeeds.
    if (!stateful_pending.empty())
        flush_stateful_batch();
}

bool Context::bind_surface(VASurfaceID surface_id)
{
    if (!queues_initialized || !driver_data->surfaces.contains(surface_id))
        return false;
    if (surface_buffer_indices.contains(surface_id)) {
        if (!uses_stateful_streaming() || driver_data->surfaces.at(surface_id).destination_buffer)
            return true;
        // The surface was discovered during lazy setup, before the capture
        // pool existed. Continue below to attach its capture buffer now.
    }

    if (uses_stateful_streaming()) {
        // VA may export/create more targets after the context's lazy setup.
        // Stateful OUTPUT is batched, so a new target does not consume an
        // OUTPUT slot; reserve its CAPTURE ordinal and bind it when the
        // source-change negotiation allocates the capture queue.
        const unsigned index = surface_buffer_indices.contains(surface_id)
            ? surface_buffer_indices.at(surface_id)
            : surface_buffer_indices.size();
        surface_buffer_indices.emplace(surface_id, index);
        if (std::ranges::find(surface_ids, surface_id) == surface_ids.end())
            surface_ids.push_back(surface_id);
        auto& surface = driver_data->surfaces.at(surface_id);
        surface.source_buffer = std::cref(device.buffer(device.output_buf_type, 0));
        surface.source_buffer_index = 0;
        surface.source_buffer_queued = false;
        surface.stateful_bitstream.resize(SOURCE_SIZE_MAX);
        // The CAPTURE pool is allocated before SOURCE_CHANGE/STREAMON so
        // Chromium can export newly-created VA surfaces immediately. Bind a
        // buffer as soon as it exists; start_capture() queues it when the
        // queue begins streaming.
        if (capture_initialized) {
            if (index >= device.buffer_count(device.capture_buf_type))
                return false;
            const auto& driver_format = device.capture_format.fmt.pix_mp;
            const auto& v4l2_format = lookup_format(driver_format.pixelformat).v4l2;
            surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, index));
            surface.destination_buffer_index = index;
            // Capture was already STREAMON when this target was exported;
            // start_capture queued every allocated buffer up front.
            surface.destination_buffer_queued = true;
            surface.logical_destination_layout.clear();
            if (v4l2_format.derive_layout) {
                surface.logical_destination_layout = v4l2_format.derive_layout(driver_format.width, driver_format.height);
            } else {
                for (unsigned plane = 0; plane < driver_format.num_planes; plane++) {
                    surface.logical_destination_layout.push_back({ plane, driver_format.plane_fmt[plane].sizeimage,
                        driver_format.plane_fmt[plane].bytesperline,
                        plane ? surface.logical_destination_layout[plane - 1].offset
                                    + surface.logical_destination_layout[plane - 1].size
                              : 0 });
                }
            }
        }
        return true;
    }

    unsigned index = surface_buffer_indices.size();
    if (index >= device.buffer_count(device.output_buf_type) || index >= device.buffer_count(device.capture_buf_type)) {
        return false;
    }

    auto& surface = driver_data->surfaces.at(surface_id);
    const auto& driver_format = device.capture_format.fmt.pix_mp;
    const auto& v4l2_format = lookup_format(driver_format.pixelformat).v4l2;
    surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, index));
    surface.destination_buffer_index = index;
    surface.destination_buffer_queued = capture_initialized;
    surface.logical_destination_layout.clear();
    if (v4l2_format.derive_layout) {
        surface.logical_destination_layout = v4l2_format.derive_layout(driver_format.width, driver_format.height);
    } else {
        for (unsigned plane = 0; plane < driver_format.num_planes; plane++) {
            surface.logical_destination_layout.push_back({ plane, driver_format.plane_fmt[plane].sizeimage,
                driver_format.plane_fmt[plane].bytesperline,
                plane ? surface.logical_destination_layout[plane - 1].offset
                            + surface.logical_destination_layout[plane - 1].size
                      : 0 });
        }
    }
    surface.source_buffer = std::cref(device.buffer(device.output_buf_type, index));
    surface.source_buffer_index = index;
    surface.source_buffer_queued = false;
    surface_buffer_indices.emplace(surface_id, index);
    return true;
}

void Context::begin_surface(VASurfaceID surface_id)
{
    current_surfaces[this] = surface_id;
}

void Context::end_surface()
{
    current_surfaces.erase(this);
}

VASurfaceID Context::current_surface() const
{
    auto it = current_surfaces.find(this);
    return it == current_surfaces.end() ? VA_INVALID_ID : it->second;
}

std::optional<VASurfaceID> Context::surface_for_buffer(v4l2_buf_type type, unsigned index) const
{
    for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
        if (buffer_index != index)
            continue;
        if (type == device.output_buf_type || type == device.capture_buf_type)
            return surface_id;
    }
    return std::nullopt;
}

VAStatus createContext(VADriverContextP va_context, VAConfigID config_id, int picture_width, int picture_height,
    int flags, VASurfaceID* surface_ids, int surfaces_count, VAContextID* context_id)
{
    // FIXME: Should create own V4L2M2MDevice to localize settings?
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    if (!driver_data->configs.contains(config_id)) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    const auto& config = driver_data->configs.at(config_id);

    auto surfaces = std::span(surface_ids, surfaces_count);
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va create_context config=%u size=%dx%d surfaces=%d\n", config_id, picture_width,
            picture_height, surfaces_count);
    for (auto&& surface : surfaces) {
        if (!driver_data->surfaces.contains(surface)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    try {
        *context_id = smallest_free_key(driver_data->contexts);
        auto [context, inserted] = driver_data->contexts.emplace(std::make_pair(
            *context_id, Context::create(driver_data, config.profile, picture_width, picture_height, surfaces)));
        if (!inserted) {
            error_log(va_context, "Failed to create context\n");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    } catch (std::exception& e) {
        error_log(va_context, "Failed to create context: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va create_context done id=%u ptr=%p\n", *context_id,
            driver_data->contexts.at(*context_id).get());

    return VA_STATUS_SUCCESS;
}

VAStatus destroyContext(VADriverContextP va_context, VAContextID context_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va destroy_context id=%u ptr=%p\n", context_id,
            driver_data->contexts.at(context_id).get());
    driver_data->contexts.erase(context_id);

    return VA_STATUS_SUCCESS;
}
