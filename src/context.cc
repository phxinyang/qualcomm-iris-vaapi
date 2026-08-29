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
#include <chrono>
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

size_t stateful_batch_limit_from_env()
{
    // Keep the stable Iris submission cadence. A value of one remains useful
    // for timestamp diagnostics, while the default groups a short GOP into a
    // single OUTPUT transaction and avoids visible pacing jitter.
    size_t limit = 8;
    if (const char* value = std::getenv("V4L2_VA_BATCH_SIZE")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed >= 1)
            limit = std::min<unsigned long>(parsed, 8);
    }
    return limit;
}

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
}

Context::~Context()
{
    if (queues_initialized) {
        // Virtual dispatch is unavailable from the base destructor. Stateful
        // codecs are identified by their compressed OUTPUT fourcc instead.
        bool stateful_format = pixelformat == V4L2_PIX_FMT_H264 || pixelformat == V4L2_PIX_FMT_VP9
            || pixelformat == V4L2_PIX_FMT_HEVC;
#ifdef V4L2_PIX_FMT_AV1
        stateful_format = stateful_format || pixelformat == V4L2_PIX_FMT_AV1;
#endif
        if (stateful_format)
            drain_stateful_decoder();
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
        // Qualcomm Iris requires an initial CAPTURE S_FMT before OUTPUT S_FMT
        // to establish the NV12 geometry; the driver rounds the padded height
        // up (e.g. 360 -> 384) and createSurfacesDeferred() later re-reads it.
        device.set_format(device.capture_buf_type, format->v4l2.format, picture_width, picture_height);
    }

    device.set_format(device.output_buf_type, pixelformat, picture_width, picture_height);

    // Chrome can keep more decoded render targets in flight than the native
    // smoke test. Iris accepts a 25/25 OUTPUT/CAPTURE pair once every AU has
    // its SPS/PPS, and this covers Chrome's complete VA surface pool without
    // transient export failures.
    // Chrome may keep one extra render target while recycling decoded
    // frames. Keep a margin above the usual 24-surface pool so late export
    // requests do not fail with VA_STATUS_ERROR_INVALID_SURFACE.
    constexpr unsigned capture_buffers = 32;
    constexpr unsigned output_buffers = 32;
    // Allocate the CAPTURE pool before the first SOURCE_CHANGE so Chromium can
    // export VA surfaces during context setup. The pool is queued only after
    // the first compressed AU has been submitted.
    createSurfacesDeferred(driver_data, *this, surface_ids, capture_buffers);

    if (device.request_buffers(device.output_buf_type, output_buffers) == 0)
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
        // Iris must see the first compressed buffer in OUTPUT before STREAMON;
        // flush_stateful_batch() starts OUTPUT after it queues that AU.
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

    // Reuse the preallocated CAPTURE pool so exported dmabuf FDs remain stable
    // for Chromium. SOURCE_CHANGE is the point at which Iris allows CAPTURE to
    // be queued and streamed.
    for (auto& [surface_id, buffer_index] : surface_buffer_indices)
        bind_surface(surface_id);
    for (unsigned i = 0; i < device.buffer_count(device.capture_buf_type); i++)
        device.buffer(device.capture_buf_type, i).queue();
    for (auto& [surface_id, buffer_index] : surface_buffer_indices) {
        auto& surface = driver_data->surfaces.at(surface_id);
        // The complete CAPTURE pool is queued before STREAMON. A completed
        // buffer is marked available again only after its timestamp is
        // associated with a VA surface in syncSurface().
        surface.destination_buffer_queued = true;
    }
    device.stream_capture(true);
    capture_initialized = true;
    return true;
}

VAStatus Context::append_stateful_picture(VASurfaceID surface_id)
{
    if (!uses_stateful_streaming() || !driver_data->surfaces.contains(surface_id))
        return VA_STATUS_ERROR_OPERATION_FAILED;
    auto& surface = driver_data->surfaces.at(surface_id);
    if (surface.source_size_used == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (stateful_pending_size + surface.source_size_used > SOURCE_SIZE_MAX)
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    if (surface.params.h264.slice)
        surface.stateful_frame_type = surface.params.h264.slice->slice_type % 5;
    else
        surface.stateful_frame_type = 255;
    // CLOCK_MONOTONIC is stored with microsecond precision in timeval, while
    // Chromium can submit several AUs inside one microsecond. Keep timestamps
    // strictly increasing so the timestamp -> VA-surface map cannot overwrite
    // an earlier AU.
    if (surface.timestamp.tv_sec < stateful_last_timestamp.tv_sec
        || (surface.timestamp.tv_sec == stateful_last_timestamp.tv_sec
            && surface.timestamp.tv_usec <= stateful_last_timestamp.tv_usec)) {
        surface.timestamp = stateful_last_timestamp;
        if (++surface.timestamp.tv_usec >= 1000000) {
            surface.timestamp.tv_sec++;
            surface.timestamp.tv_usec = 0;
        }
    }
    stateful_last_timestamp = surface.timestamp;
    if (std::getenv("V4L2_VA_RESET_ON_IDR") && stateful_sequence_start(surface_id))
        stateful_sequence_starts.insert(surface_id);
    stateful_pending.push_back(surface_id);
    stateful_pending_size += surface.source_size_used;
    if (stateful_pending.size() >= stateful_batch_limit_from_env())
        return flush_stateful_batch();
    return VA_STATUS_SUCCESS;
}

VAStatus Context::flush_stateful_batch()
{
    if (!uses_stateful_streaming() || stateful_pending.empty())
        return VA_STATUS_SUCCESS;
    if (stateful_draining)
        return VA_STATUS_SUCCESS;

    try {
        // VA-API does not require the producer to call vaSyncSurface before it
        // submits the next picture. Chrome therefore fills the OUTPUT queue
        // while decoded CAPTURE/OUTPUT buffers are already ready; service
        // them first so stateful_batches reflects the driver's queue state.
        service_stateful_queues();

        unsigned batch_index = std::numeric_limits<unsigned>::max();
        for (unsigned i = 0; i < device.buffer_count(device.output_buf_type); i++) {
            if (!stateful_batches.contains(i)) {
                batch_index = i;
                break;
            }
        }
        if (batch_index == std::numeric_limits<unsigned>::max())
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

        // Iris accepts a bounded Annex-B stream per OUTPUT buffer and returns
        // all decoded frames with that buffer's timestamp. Keep the surfaces
        // in FIFO order so CAPTURE DQBUF can resolve them deterministically.
        const size_t batch_limit = stateful_batch_limit_from_env();
        const size_t batch_count = std::min(batch_limit, stateful_pending.size());
        std::vector<VASurfaceID> batch_surfaces;
        batch_surfaces.reserve(batch_count);
        auto destination = device.buffer(device.output_buf_type, batch_index).mapping()[0];
        size_t aggregate_size = 0;
        for (size_t i = 0; i < batch_count; i++) {
            const VASurfaceID surface_id = stateful_pending[i];
            auto& surface = driver_data->surfaces.at(surface_id);
            if (stateful_sequence_starts.erase(surface_id) != 0) {
                if (std::getenv("V4L2_VA_TRACE"))
                    std::fprintf(stderr, "stateful reset before IDR surface=%u\n", surface_id);
                reset_stateful_decoder();
            }
            const size_t source_size = surface.source_size_used;
            if (aggregate_size + source_size > destination.size())
                return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
            std::memcpy(destination.data() + aggregate_size, surface.stateful_bitstream.data(), source_size);
            aggregate_size += source_size;
            batch_surfaces.push_back(surface_id);
        }
        auto batch_timestamp = driver_data->surfaces.at(batch_surfaces.front()).timestamp;
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful flush batch=%u surfaces=%zu bytes=%zu ts=%lld.%06ld pending=%zu\n",
                batch_index, batch_surfaces.size(), aggregate_size,
                static_cast<long long>(batch_timestamp.tv_sec), static_cast<long>(batch_timestamp.tv_usec),
                stateful_pending.size());
        if (std::getenv("V4L2_VA_DUMP_BATCH")) {
            char path[160];
            std::snprintf(path, sizeof(path), "%s/iris-va-batch-%u.h264",
                std::getenv("V4L2_VA_DUMP_BATCH"), batch_index);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fwrite(destination.data(), 1, aggregate_size, file);
                std::fclose(file);
            }
        }
        device.buffer(device.output_buf_type, batch_index).queue(-1, &batch_timestamp, aggregate_size);
        for (const auto surface_id : batch_surfaces)
            driver_data->surfaces.at(surface_id).source_buffer_queued = true;
        stateful_batches.emplace(batch_index, std::move(batch_surfaces));
        stateful_batch_order.push_back(batch_index);
        stateful_batch_timestamps.emplace(
            std::make_pair(static_cast<long long>(batch_timestamp.tv_sec), static_cast<long>(batch_timestamp.tv_usec)),
            batch_index);
        for (size_t i = 0; i < batch_count; i++) {
            stateful_pending_size -= driver_data->surfaces.at(stateful_pending[i]).source_size_used;
            stateful_submitted_count++;
        }
        stateful_pending.erase(stateful_pending.begin(), stateful_pending.begin() + batch_count);
        if (!device.output_streaming)
            device.stream_output(true);
        if (!capture_initialized) {
            // OUTPUT STREAMON raises Iris' initial SOURCE_CHANGE event.
            if (!start_capture())
                return VA_STATUS_ERROR_OPERATION_FAILED;
            stateful_queue_restart_pending = false;
        }
    } catch (const std::system_error& error) {
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful flush failed: %s\n", error.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

void Context::service_stateful_queues()
{
    if (!uses_stateful_streaming() || !capture_initialized)
        return;

    // Iris returns CAPTURE before the corresponding compressed OUTPUT buffer.
    // Resolve the timestamp while the mapping still owns the batch, then make
    // the returned CAPTURE buffer available for the next VA surface.
    while (auto capture_index = device.dequeue_ready(device.capture_buf_type, 0)) {
        bool requeued = false;
        bool capture_requeued = false;
        auto completed = surface_for_timestamp(device.last_dequeued_timestamp(), device.last_dequeued_flags());
        if (!completed && !uses_stateful_streaming())
            completed = surface_for_capture_flags(device.last_dequeued_flags());
        if (completed && driver_data->surfaces.contains(*completed)) {
            auto& surface = driver_data->surfaces.at(*completed);
            const bool staged = copy_surfaces_enabled()
                && stage_surface_frame(surface, device.buffer(device.capture_buf_type, *capture_index));
            if (staged && publish_surface_frame(surface)) {
                // The browser imports the private stable DMA-BUF. Return the
                // rotating Iris CAPTURE slot immediately for the next frame.
                device.buffer(device.capture_buf_type, *capture_index).queue();
                surface.destination_buffer_queued = true;
                capture_requeued = true;
            } else {
                surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, *capture_index));
                surface.destination_buffer_index = *capture_index;
                surface.destination_buffer_queued = false;
            }
            // A surface is serialized by beginPicture(): a Rendering target
            // is synchronized before it can be reused. Therefore every
            // timestamp completion retires the input scratch and releases the
            // target for the next VA picture.
            if (!surface.pending_frame_ready)
                surface.status = VASurfaceDisplaying;
            surface.source_size_used = 0;
            if (device.last_dequeued_error() && !capture_requeued)
                device.buffer(device.capture_buf_type, *capture_index).queue();
        } else {
            // A dropped/incomplete frame has no VA target, but its CAPTURE
            // slot must still be returned to the driver immediately.
            device.buffer(device.capture_buf_type, *capture_index).queue();
            requeued = true;
        }
        if (device.last_dequeued_error() && !requeued && !capture_requeued)
            device.buffer(device.capture_buf_type, *capture_index).queue();
    }

    while (auto output_index = device.dequeue_ready(device.output_buf_type, 0))
        mark_source_buffer_dequeued(*output_index);
}

void Context::discard_stateful_error_frame()
{
    while (!stateful_batch_order.empty()) {
        const unsigned index = stateful_batch_order.front();
        stateful_batch_order.pop_front();
        auto batch = stateful_batches.find(index);
        if (batch == stateful_batches.end())
            continue;

        for (const auto surface_id : batch->second) {
            auto surface = driver_data->surfaces.find(surface_id);
            if (surface == driver_data->surfaces.end())
                continue;
            for (auto timestamp = stateful_batch_timestamps.begin(); timestamp != stateful_batch_timestamps.end();) {
                if (timestamp->second == index)
                    timestamp = stateful_batch_timestamps.erase(timestamp);
                else
                    ++timestamp;
            }
            if (surface->second.status != VASurfaceRendering) {
                surface->second.status = VASurfaceDisplaying;
                surface->second.source_size_used = 0;
            }
        }

        // The OUTPUT DQBUF may have arrived before the error CAPTURE buffer.
        // In that case both halves are complete and the slot can be removed;
        // otherwise mark CAPTURE done and let mark_source_buffer_dequeued()
        // release it when the driver recycles OUTPUT.
        if (stateful_output_dequeued.erase(index) != 0)
            stateful_batches.erase(batch);
        else
            stateful_capture_done.insert(index);
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful discard error batch=%u remaining=%zu\n", index, stateful_batches.size());
        return;
    }
}

bool Context::has_queued_stateful_output() const
{
    if (!uses_stateful_streaming())
        return false;
    for (const auto& [index, surfaces] : stateful_batches) {
        if (!stateful_output_dequeued.contains(index))
            return true;
    }
    return false;
}

void Context::discard_stateful_surface(VASurfaceID surface_id)
{
    if (!uses_stateful_streaming())
        return;
    for (auto batch = stateful_batches.begin(); batch != stateful_batches.end();) {
        auto surface = std::ranges::find(batch->second, surface_id);
        if (surface == batch->second.end()) {
            ++batch;
            continue;
        }
        batch->second.erase(surface);
        if (!batch->second.empty()) {
            ++batch;
            continue;
        }
        for (auto timestamp = stateful_batch_timestamps.begin(); timestamp != stateful_batch_timestamps.end();) {
            if (timestamp->second == batch->first)
                timestamp = stateful_batch_timestamps.erase(timestamp);
            else
                ++timestamp;
        }
        if (stateful_output_dequeued.erase(batch->first) != 0) {
            stateful_capture_done.erase(batch->first);
            batch = stateful_batches.erase(batch);
        } else {
            // Keep an OUTPUT slot reserved until its DQBUF arrives. When it
            // does, mark_source_buffer_dequeued() will release the batch.
            stateful_capture_done.insert(batch->first);
            ++batch;
        }
    }
}

void Context::reset_stateful_decoder()
{
    if (!uses_stateful_streaming() || !capture_initialized)
        return;
    device.decoder_stop();
    for (unsigned attempt = 0; attempt < 200; attempt++) {
        auto capture_index = device.dequeue_ready(device.capture_buf_type, 20);
        if (!capture_index)
            break;
        const bool capture_was_last = device.last_dequeued_last();
        bool capture_requeued = false;
        if (auto completed = surface_for_timestamp(device.last_dequeued_timestamp());
            completed && driver_data->surfaces.contains(*completed)) {
            auto& surface = driver_data->surfaces.at(*completed);
            const bool staged = copy_surfaces_enabled()
                && stage_surface_frame(surface, device.buffer(device.capture_buf_type, *capture_index));
            if (staged && publish_surface_frame(surface)) {
                device.buffer(device.capture_buf_type, *capture_index).queue();
                surface.destination_buffer_queued = true;
                capture_requeued = true;
            } else {
                surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, *capture_index));
                surface.destination_buffer_index = *capture_index;
                surface.destination_buffer_queued = false;
            }
            if (!surface.pending_frame_ready)
                surface.status = VASurfaceDisplaying;
            surface.source_size_used = 0;
        }
        // STOP is a drain operation, not a queue teardown. Iris may still
        // need a free CAPTURE slot to retire OUTPUT buffers that were queued
        // before STOP (including the tail of a B-frame GOP). Requeue each
        // returned CAPTURE buffer immediately instead of waiting until the
        // OUTPUT drain has completed.
        if (!capture_requeued)
            device.buffer(device.capture_buf_type, *capture_index).queue();
        for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
            if (!driver_data->surfaces.contains(surface_id))
                continue;
            auto& surface = driver_data->surfaces.at(surface_id);
            if (surface.destination_buffer_index == *capture_index)
                surface.destination_buffer_queued = true;
        }
        if (capture_was_last)
            break;
    }
    // OUTPUT completion can trail the final CAPTURE buffer by a few
    // scheduler ticks. Give the driver a short drain window before START;
    // issuing DECODER_CMD_START with one old OUTPUT still queued returns EBUSY
    // on Iris after repeated seeks/loops.
    const auto output_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (unsigned attempt = 0; attempt < 200 && !stateful_batches.empty(); attempt++) {
        auto output_index = device.dequeue_ready(device.output_buf_type, 20);
        if (!output_index) {
            if (std::chrono::steady_clock::now() < output_deadline)
                continue;
            else
                break;
        }
        mark_source_buffer_dequeued(*output_index);
    }
    if (std::getenv("V4L2_VA_TRACE") && !stateful_batches.empty())
        std::fprintf(stderr, "stateful reset output drain incomplete remaining=%zu\n", stateful_batches.size());
    stateful_batches.clear();
    stateful_batch_timestamps.clear();
    stateful_output_dequeued.clear();
    stateful_capture_done.clear();
    stateful_batch_order.clear();
    stateful_sequence_starts.clear();
    const bool restart_queues = std::getenv("V4L2_VA_RESET_OUTPUT_STREAM") != nullptr;
    for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
        if (!driver_data->surfaces.contains(surface_id))
            continue;
        auto& surface = driver_data->surfaces.at(surface_id);
        surface.source_buffer_queued = false;
        if (!restart_queues && surface.destination_buffer && !surface.destination_buffer_queued) {
            surface.destination_buffer->get().queue();
            surface.destination_buffer_queued = true;
        }
    }
    // Iris treats DECODER_CMD_STOP as a drain/pause operation. For a new
    // H.264 sequence, the driver documents a queue STREAMOFF/STREAMON pair as
    // the resume path. On the SM8550 kernel, stopping OUTPUT also stops the
    // CAPTURE queue internally, so restart both queues and keep the userspace
    // streaming flags in sync with the kernel.
    if (restart_queues) {
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful reset streamoff capture\n");
        device.stream_capture(false);
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful reset streamoff output\n");
        device.stream_output(false);
        // STREAMOFF returns every CAPTURE buffer to DEQUEUED. Leave the pool
        // unqueued until the first new AU raises SOURCE_CHANGE; start_capture()
        // performs the CAPTURE QBUF/STREAMON handshake at that point.
        for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
            if (!driver_data->surfaces.contains(surface_id))
                continue;
            auto& surface = driver_data->surfaces.at(surface_id);
            surface.source_buffer_queued = false;
            if (surface.destination_buffer)
                surface.destination_buffer_queued = false;
        }
        // Keep CAPTURE stopped until the first new OUTPUT AU has restarted
        // OUTPUT. Iris requires OUTPUT STREAMON before CAPTURE STREAMON.
        capture_initialized = false;
        stateful_queue_restart_pending = true;
    } else {
        device.decoder_start();
        stateful_queue_restart_pending = false;
    }
    stateful_last_marker_seen = false;
    stateful_submitted_count = 0;
}

void Context::drain_stateful_decoder()
{
    // This method is also called from the base destructor, where virtual
    // dispatch already resolves uses_stateful_streaming() to false.
    if (!capture_initialized)
        return;
    if (stateful_last_marker_seen && stateful_batches.empty() && stateful_pending.empty())
        return;

    // Chrome has no VA call that explicitly carries end-of-stream to this
    // backend. Submit any staged access units, then use the stateful STOP
    // command to release pictures still held in Iris' reorder queue.
    if (!stateful_pending.empty() && flush_stateful_batch() != VA_STATUS_SUCCESS)
        return;

    try {
        device.decoder_stop();
    } catch (const std::system_error& error) {
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful drain stop failed: %s\n", error.what());
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool saw_last = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto capture_index = device.dequeue_ready(device.capture_buf_type, 50);
        if (!capture_index)
            continue;
        const bool capture_was_last = device.last_dequeued_last();

        bool capture_requeued = false;
        auto completed = surface_for_timestamp(device.last_dequeued_timestamp(), device.last_dequeued_flags());
        if (completed && driver_data->surfaces.contains(*completed)) {
            auto& surface = driver_data->surfaces.at(*completed);
            const bool staged = copy_surfaces_enabled()
                && stage_surface_frame(surface, device.buffer(device.capture_buf_type, *capture_index));
            if (staged && publish_surface_frame(surface)) {
                device.buffer(device.capture_buf_type, *capture_index).queue();
                surface.destination_buffer_queued = true;
                capture_requeued = true;
            } else {
                surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, *capture_index));
                surface.destination_buffer_index = *capture_index;
                surface.destination_buffer_queued = false;
            }
            if (!surface.pending_frame_ready)
                surface.status = VASurfaceDisplaying;
            surface.source_size_used = 0;
        }
        if (!capture_requeued)
            device.buffer(device.capture_buf_type, *capture_index).queue();

        while (auto output_index = device.dequeue_ready(device.output_buf_type, 0))
            mark_source_buffer_dequeued(*output_index);

        if (capture_was_last) {
            saw_last = true;
            stateful_last_marker_seen = true;
            break;
        }
    }

    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "stateful drain complete last=%d batches=%zu\n", saw_last, stateful_batches.size());
}

void Context::mark_source_buffer_dequeued(unsigned index)
{
    auto batch = stateful_batches.find(index);
    if (batch == stateful_batches.end())
        return;
    for (auto surface_id : batch->second) {
        if (driver_data->surfaces.contains(surface_id))
            driver_data->surfaces.at(surface_id).source_buffer_queued = false;
    }
    // Iris can recycle OUTPUT only after the matching CAPTURE frame has been
    // returned as well. Keep the slot reserved when OUTPUT DQBUF wins the
    // race; surface_for_timestamp() releases it after CAPTURE DQBUF.
    if (stateful_capture_done.erase(index) != 0)
        stateful_batches.erase(batch);
    else
        stateful_output_dequeued.insert(index);
}

void Context::resume_after_drain()
{
    if (!stateful_draining)
        return;
    // STREAMON/STOP returns completed CAPTURE buffers to userspace. Requeue
    // only targets that are no longer being rendered before restarting the
    // firmware; otherwise the resumed batch can run with an empty queue.
    for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
        if (!driver_data->surfaces.contains(surface_id))
            continue;
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
        if (device.buffer_count(device.capture_buf_type) > 0) {
            if (index >= device.buffer_count(device.capture_buf_type))
                return false;
            const auto& driver_format = device.capture_format.fmt.pix_mp;
            const auto& v4l2_format = lookup_format(driver_format.pixelformat).v4l2;
            surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, index));
            surface.destination_buffer_index = index;
            // Before STREAMON start_capture() queues the whole pool. After
            // STREAMON this index was already queued with the pool, so never
            // QBUF it a second time.
            surface.destination_buffer_queued = device.capture_streaming;
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
    if (uses_stateful_streaming() && type == device.output_buf_type) {
        auto batch = stateful_batches.find(index);
        if (batch != stateful_batches.end() && !batch->second.empty())
            return batch->second.front();
        return std::nullopt;
    }
    for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
        if (buffer_index != index)
            continue;
        if (type == device.output_buf_type || type == device.capture_buf_type)
            return surface_id;
    }
    return std::nullopt;
}

namespace {
unsigned capture_frame_type(uint32_t flags)
{
    if (flags & V4L2_BUF_FLAG_KEYFRAME)
        return 2;
    if (flags & V4L2_BUF_FLAG_PFRAME)
        return 0;
    if (flags & V4L2_BUF_FLAG_BFRAME)
        return 1;
    return 255;
}

void remove_batch_timestamp(std::map<std::pair<long long, long>, unsigned>& timestamps, unsigned batch_index)
{
    for (auto it = timestamps.begin(); it != timestamps.end();) {
        if (it->second == batch_index)
            it = timestamps.erase(it);
        else
            ++it;
    }
}
}

std::optional<VASurfaceID> Context::surface_for_timestamp(const timeval& timestamp, uint32_t capture_flags)
{
    if (!uses_stateful_streaming())
        return std::nullopt;
    auto it = stateful_batch_timestamps.find(
        std::make_pair(static_cast<long long>(timestamp.tv_sec), static_cast<long>(timestamp.tv_usec)));
    if (it == stateful_batch_timestamps.end())
    {
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful timestamp miss ts=%lld.%06ld\n",
                static_cast<long long>(timestamp.tv_sec), static_cast<long>(timestamp.tv_usec));
        return std::nullopt;
    }
    const auto batch_index = it->second;
    auto batch = stateful_batches.find(batch_index);
    if (batch == stateful_batches.end() || batch->second.empty()) {
        stateful_batch_timestamps.erase(it);
        return std::nullopt;
    }
    const auto wanted_type = capture_frame_type(capture_flags);
    auto selected = batch->second.begin();
    if (wanted_type != 255) {
        selected = std::ranges::find_if(batch->second, [&](VASurfaceID id) {
            return driver_data->surfaces.contains(id)
                && driver_data->surfaces.at(id).stateful_frame_type == wanted_type;
        });
        // The timestamp identifies the OUTPUT batch. Iris can report a
        // generic P/B flag that does not match the VA slice type (notably at
        // reference-frame boundaries), so never consume a surface from a
        // different timestamp as a fallback. Retire the oldest surface in
        // this batch when the type hint is inconsistent.
        if (selected == batch->second.end())
            selected = batch->second.begin();
    }
    const auto surface_id = *selected;
    batch->second.erase(selected);
    if (batch->second.empty()) {
        stateful_batch_timestamps.erase(it);
        if (stateful_output_dequeued.erase(batch->first) != 0)
            stateful_batches.erase(batch);
        else
            stateful_capture_done.insert(batch->first);
    }
    return surface_id;
}

std::optional<VASurfaceID> Context::surface_for_capture_flags(uint32_t capture_flags)
{
    if (!uses_stateful_streaming())
        return std::nullopt;
    const auto wanted_type = capture_frame_type(capture_flags);
    for (auto order = stateful_batch_order.begin(); order != stateful_batch_order.end();) {
        const auto batch_index = *order;
        auto batch = stateful_batches.find(batch_index);
        if (batch == stateful_batches.end() || batch->second.empty()) {
            order = stateful_batch_order.erase(order);
            continue;
        }
        auto selected = batch->second.begin();
        if (wanted_type != 255) {
            selected = std::ranges::find_if(batch->second, [&](VASurfaceID id) {
                return driver_data->surfaces.contains(id)
                    && driver_data->surfaces.at(id).stateful_frame_type == wanted_type;
            });
            if (selected == batch->second.end()) {
                ++order;
                continue;
            }
        }
        const auto surface_id = *selected;
        batch->second.erase(selected);
        if (batch->second.empty()) {
            remove_batch_timestamp(stateful_batch_timestamps, batch_index);
            if (stateful_output_dequeued.erase(batch_index) != 0)
                stateful_batches.erase(batch);
            else
                stateful_capture_done.insert(batch_index);
            order = stateful_batch_order.erase(order);
        }
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
    if (driver_data->contexts.at(context_id)->uses_stateful_streaming())
        driver_data->contexts.at(context_id)->drain_stateful_decoder();
    driver_data->contexts.erase(context_id);

    return VA_STATUS_SUCCESS;
}
