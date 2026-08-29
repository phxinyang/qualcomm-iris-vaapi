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
#include <sys/syscall.h>
#include <unistd.h>

#include <va/va.h>
}

#include "config.h"
#include "av1.h"
#include "driver.h"
#include "format.h"
#include "h264.h"
#include "hevc.h"
#include "mpeg2.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"
#include "vp8.h"
#ifdef ENABLE_VP9
#include "vp9.h"
#endif
#include "vp9_stateful.h"

namespace {
thread_local std::unordered_map<const Context*, VASurfaceID> current_surfaces;

size_t stateful_batch_limit_from_env()
{
    size_t limit = 1;
    if (const char* value = std::getenv("V4L2_VA_BATCH_SIZE")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed >= 1)
            limit = std::min<unsigned long>(parsed, 8);
    }
    return limit;
}

}

std::set<VAProfile> Context::supported_profiles(const std::deque<V4L2M2MDevice>& devices)
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
        for (auto&& profile : VP9StatefulContext::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : HEVCContext::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : AV1Context::supported_profiles(device)) {
            result.insert(profile);
        }
    }
    return result;
}

Context* Context::create(DriverData* driver_data, VAProfile profile, int picture_width, int picture_height,
    std::span<VASurfaceID> surface_ids)
{
    for (auto&& device : driver_data->devices) {
        const int probe_fd = device.video_fd;
        auto create_session = [&]() -> V4L2M2MDevice& {
            driver_data->devices.emplace_back(device.clone_for_context());
            auto& session = driver_data->devices.back();
            if (std::getenv("V4L2_VA_TRACE"))
                std::fprintf(stderr, "va context session probe_fd=%d session_fd=%d\n", probe_fd, session.video_fd);
            return session;
        };
        if (MPEG2Context::supported_profiles(device).contains(profile)) {
            return new MPEG2Context(driver_data, create_session(), picture_width, picture_height, surface_ids);
        }
        if (H264Context::supported_profiles(device).contains(profile)) {
            return new H264Context(driver_data, create_session(), profile, picture_width, picture_height, surface_ids);
        }
        if (VP8Context::supported_profiles(device).contains(profile)) {
            return new VP8Context(driver_data, create_session(), picture_width, picture_height, surface_ids);
        }
#ifdef ENABLE_VP9
        if (VP9Context::supported_profiles(device).contains(profile)) {
            return new VP9Context(driver_data, create_session(), picture_width, picture_height, surface_ids);
        }
#endif
        if (VP9StatefulContext::supported_profiles(device).contains(profile)) {
            return new VP9StatefulContext(driver_data, create_session(), picture_width, picture_height, surface_ids);
        }
        if (HEVCContext::supported_profiles(device).contains(profile)) {
            return new HEVCContext(driver_data, create_session(), profile, picture_width, picture_height, surface_ids);
        }
        if (AV1Context::supported_profiles(device).contains(profile)) {
            return new AV1Context(driver_data, create_session(), picture_width, picture_height, surface_ids);
        }
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

bool stateful_capture_scheduled()
{
    const char* value = std::getenv("V4L2_VA_CAPTURE_SCHEDULED");
    return value && std::strcmp(value, "1") == 0;
}

Context::~Context()
{
    // Join before taking any decoder lock. destroyContext() already holds the
    // driver mutex here, and the watchdog only ever try_locks, so it can make
    // progress to its stop check.
    stop_stateful_watchdog();
    if (queues_initialized) {
        std::lock_guard<std::recursive_mutex> guard(synchronization_mutex_);
        // Virtual dispatch is already in the base destructor here, so
        // uses_stateful_streaming() would incorrectly report false. The
        // stateful codecs use their compressed OUTPUT fourcc as the stable
        // discriminator (stateless H.264 uses H264_SLICE instead).
        if (pixelformat == V4L2_PIX_FMT_H264 || pixelformat == V4L2_PIX_FMT_VP9
            || pixelformat == V4L2_PIX_FMT_HEVC || pixelformat == V4L2_PIX_FMT_AV1)
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
    const unsigned capture_buffers = uses_stateful_streaming()
        ? 32u
        : static_cast<unsigned>(surface_ids.size());
    const unsigned output_buffers = uses_stateful_streaming()
        ? 32u
        : static_cast<unsigned>(surface_ids.size());
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
    if (stateful_capture_scheduled()) {
        // Stateful V4L2 has no VA render-target argument. Queue only the
        // capture buffers belonging to surfaces already submitted in OUTPUT,
        // so Iris writes each decoded frame into the DMA-BUF Chrome exported
        // for that VA surface instead of choosing an unrelated pool slot.
        std::set<unsigned> scheduled;
        for (const auto& [batch_index, batch_surfaces] : stateful_batches) {
            for (const auto surface_id : batch_surfaces) {
                auto binding = surface_buffer_indices.find(surface_id);
                if (binding != surface_buffer_indices.end())
                    scheduled.insert(binding->second);
            }
        }
        for (const auto index : scheduled) {
            if (index >= device.buffer_count(device.capture_buf_type))
                continue;
            auto& buffer = device.buffer(device.capture_buf_type, index);
            auto owner = surface_for_buffer(device.capture_buf_type, index);
            if (!owner || !driver_data->surfaces.contains(*owner))
                continue;
            auto& surface = driver_data->surfaces.at(*owner);
            if (!surface.destination_buffer_queued) {
                buffer.queue();
                surface.destination_buffer_queued = true;
            }
        }
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful scheduled capture buffers=%zu\n", scheduled.size());
    } else {
        for (unsigned i = 0; i < device.buffer_count(device.capture_buf_type); i++)
            device.buffer(device.capture_buf_type, i).queue();
        for (auto& [surface_id, buffer_index] : surface_buffer_indices) {
            auto& surface = driver_data->surfaces.at(surface_id);
            // The complete CAPTURE pool is queued before STREAMON. A completed
            // buffer is marked available again only after its timestamp is
            // associated with a VA surface in syncSurface().
            surface.destination_buffer_queued = true;
        }
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
    if (stateful_pending_size > std::numeric_limits<size_t>::max() - surface.source_size_used)
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    surface.stateful_frame_type = stateful_frame_type(surface_id);
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

        // Iris stateful treats each OUTPUT buffer as a continuous Annex-B
        // stream and may emit several CAPTURE frames for it. Keep a bounded
        // group of AUs in one buffer and resolve the returned frames in FIFO
        // order; submitting one AU per buffer loses the decoder's stream state.
        // Keep the production path one access unit per OUTPUT buffer. Iris
        // reports the OUTPUT timestamp on every CAPTURE frame, so aggregating
        // several AUs makes display-order association ambiguous and can show
        // a decoded surface with the wrong reference-frame history. Larger
        // batches remain available as an explicit diagnostic override.
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
                reset_stateful_timeout_recovery();
                reset_stateful_decoder();
            }
            // Keep the complete AU, including its SPS/PPS, in the aggregate
            // stream. Iris accepts repeated parameter sets and needs the PPS
            // values associated with each picture's reference list.
            const size_t source_size = surface.source_size_used;
            if (aggregate_size + source_size > destination.size())
                return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
            std::memcpy(destination.data() + aggregate_size, surface.stateful_bitstream.data(), source_size);
            aggregate_size += source_size;
            batch_surfaces.push_back(surface_id);
        }
        auto batch_timestamp = driver_data->surfaces.at(batch_surfaces.front()).timestamp;
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful flush batch=%u surfaces=%zu bytes=%zu ts=%lld.%06ld pending=%zu\n", batch_index,
                batch_surfaces.size(), aggregate_size, static_cast<long long>(batch_timestamp.tv_sec),
                static_cast<long>(batch_timestamp.tv_usec), stateful_pending.size());
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
        stateful_last_marker_seen = false;
        stateful_batch_timestamps.emplace(
            std::make_pair(static_cast<long long>(batch_timestamp.tv_sec), static_cast<long>(batch_timestamp.tv_usec)), batch_index);
        for (size_t i = 0; i < batch_count; i++) {
            stateful_pending_size -= driver_data->surfaces.at(stateful_pending[i]).source_size_used;
            stateful_submitted_count++;
        }
        stateful_pending.erase(stateful_pending.begin(), stateful_pending.begin() + batch_count);
        if (!device.output_streaming)
            device.stream_output(true);
        if (!capture_initialized) {
            // OUTPUT STREAMON raises Iris' initial SOURCE_CHANGE event. Unlike
            // the native finite-file helper, Chrome keeps the stream open, so
            // do not issue DECODER_CMD_STOP here: STOP drains the firmware and
            // marks the first batch LAST, after which further QBUF calls fail.
            if (!start_capture())
                return VA_STATUS_ERROR_OPERATION_FAILED;
            stateful_queue_restart_pending = false;
        }
        // Do not probe for backpressure from the producer path. A stateful
        // decoder normally keeps a small reorder window queued even while
        // playback is healthy; waiting here and issuing STOP would stall the
        // stream repeatedly. Drains are driven by vaSyncSurface() timeouts,
        // the end-of-stream idle watchdog, or explicit context teardown.
        note_stateful_submission();
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
        auto completed = surface_for_timestamp(device.last_dequeued_timestamp(), device.last_dequeued_flags());
        if (device.last_dequeued_error() && !completed)
            // Iris reports firmware-side decode failures as a CAPTURE buffer
            // with ERROR and timestamp 0. There is no timestamp to resolve,
            // so retire the oldest submitted batch before releasing the slot.
            discard_stateful_error_frame();
        if (!completed && !uses_stateful_streaming())
            completed = surface_for_capture_flags(device.last_dequeued_flags());
        bool capture_requeued = false;
        if (completed && driver_data->surfaces.contains(*completed)) {
            auto& surface = driver_data->surfaces.at(*completed);
            copy_surface_frame(surface, device.buffer(device.capture_buf_type, *capture_index));
            // With the stable DMA-BUF path Chrome consumes a private copy, so
            // the V4L2 CAPTURE slot must be returned immediately. Holding it
            // on the VA surface eventually exhausts the capture queue when
            // several decoders or looping videos are active.
            if (copy_surfaces_enabled() && surface.export_buffer_fd >= 0 && surface.export_buffer_mapping) {
                device.buffer(device.capture_buf_type, *capture_index).queue();
                capture_requeued = true;
            } else if (stateful_capture_scheduled()) {
                const auto expected = surface_buffer_indices.at(*completed);
                if (std::getenv("V4L2_VA_TRACE") && expected != *capture_index)
                    std::fprintf(stderr, "stateful capture mismatch surface=%u expected=%u got=%u\n", *completed,
                        expected, *capture_index);
                // If scheduling selected another slot, return that slot and
                // keep the exported surface binding untouched.
                if (expected != *capture_index)
                    device.buffer(device.capture_buf_type, *capture_index).queue();
                else
                    surface.destination_buffer_queued = false;
            } else {
                surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, *capture_index));
                surface.destination_buffer_index = *capture_index;
                surface.destination_buffer_queued = capture_requeued;
            }
            // A surface is serialized by beginPicture(): a Rendering target
            // is synchronized before it can be reused. Therefore every
            // timestamp completion retires the input scratch and releases the
            // target for the next VA picture.
            surface.status = VASurfaceDisplaying;
            surface.source_size_used = 0;
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
            // An ERROR CAPTURE completion is terminal for every AU in this
            // batch. Release even Rendering targets; otherwise their next
            // beginPicture() waits for a completion that can never arrive.
            surface->second.status = VASurfaceDisplaying;
            surface->second.source_size_used = 0;
        }

        // The OUTPUT DQBUF may have arrived before the error CAPTURE buffer.
        // In that case both halves are complete and the slot can be removed;
        // otherwise mark CAPTURE done and let mark_source_buffer_dequeued()
        // release it when the driver recycles OUTPUT.
        if (stateful_output_dequeued.erase(index) != 0) {
            stateful_batches.erase(batch);
            remove_stateful_batch_order(index);
        } else
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
    // An OUTPUT DQBUF can legally arrive before its delayed CAPTURE frame.
    // The batch remains live until both halves complete, so checking only
    // stateful_output_dequeued would hide exactly the trailing B-frame window
    // that timeout recovery needs to drain.
    return !stateful_batches.empty();
}

bool Context::try_begin_stateful_timeout_recovery()
{
    // Do not reset a cold decoder: parameter-set negotiation and firmware DPB
    // allocation can legitimately take seconds before the first few frames.
    // A burst producer such as FFmpeg can, however, reach syncSurface() before
    // Iris has returned its first CAPTURE buffer. Once enough AUs are queued,
    // that is still actionable backpressure: waiting for completed frames here
    // would permanently disable the only recovery path and leave all delayed
    // CAPTURE timestamps orphaned. Keep the eight-AU floor as the cold-start
    // guard, while allowing either a proven decode history or an all-pending
    // burst to trigger one STOP/drain recovery.
    if (!stateful_timeout_drain() || stateful_timeout_recovery_used
        || stateful_submitted_count < 8 || !has_queued_stateful_output())
        return false;
    stateful_timeout_recovery_used = true;
    return true;
}

void Context::discard_stateful_surface(VASurfaceID surface_id)
{
    if (!uses_stateful_streaming())
        return;

    // A surface can be retired after endPicture() staged it but before the
    // batch reached the V4L2 OUTPUT queue. Remove that reference as well as
    // any queued-batch entry; otherwise the next flush dereferences a surface
    // that no longer exists in DriverData::surfaces.
    for (auto pending = stateful_pending.begin(); pending != stateful_pending.end();) {
        if (*pending != surface_id) {
            ++pending;
            continue;
        }
        if (driver_data->surfaces.contains(surface_id)) {
            const auto size = driver_data->surfaces.at(surface_id).source_size_used;
            stateful_pending_size = size > stateful_pending_size ? 0 : stateful_pending_size - size;
        }
        pending = stateful_pending.erase(pending);
    }
    stateful_sequence_starts.erase(surface_id);

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
            remove_stateful_batch_order(batch->first);
            batch = stateful_batches.erase(batch);
        } else {
            // Keep an OUTPUT slot reserved until its DQBUF arrives. When it
            // does, mark_source_buffer_dequeued() will release the batch.
            stateful_capture_done.insert(batch->first);
            ++batch;
        }
    }
}

void Context::remove_stateful_batch_order(unsigned index)
{
    for (auto it = stateful_batch_order.begin(); it != stateful_batch_order.end();) {
        if (*it == index)
            it = stateful_batch_order.erase(it);
        else
            ++it;
    }
}

void Context::reset_stateful_decoder()
{
    if (!uses_stateful_streaming() || !capture_initialized)
        return;
    device.decoder_stop();
    for (unsigned attempt = 0; attempt < 200; attempt++) {
        auto capture_index = device.dequeue_ready(device.capture_buf_type, 20);
        if (capture_index) {
            if (auto completed = surface_for_timestamp(device.last_dequeued_timestamp(), device.last_dequeued_flags());
                completed && driver_data->surfaces.contains(*completed)) {
                auto& surface = driver_data->surfaces.at(*completed);
                copy_surface_frame(surface, device.buffer(device.capture_buf_type, *capture_index));
                if (!stateful_capture_scheduled()) {
                    surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, *capture_index));
                    surface.destination_buffer_index = *capture_index;
                    surface.destination_buffer_queued = false;
                }
                surface.status = VASurfaceDisplaying;
                surface.source_size_used = 0;
            }
            // STOP is a drain operation, not a queue teardown. Iris may still
            // need a free CAPTURE slot to retire OUTPUT buffers that were queued
            // before STOP (including the tail of a B-frame GOP). Requeue each
            // returned CAPTURE buffer immediately instead of waiting until the
            // OUTPUT drain has completed.
            device.buffer(device.capture_buf_type, *capture_index).queue();
            for (const auto& [surface_id, buffer_index] : surface_buffer_indices) {
                if (!driver_data->surfaces.contains(surface_id))
                    continue;
                auto& surface = driver_data->surfaces.at(surface_id);
                if (surface.destination_buffer_index == *capture_index)
                    surface.destination_buffer_queued = true;
            }
            if (device.last_dequeued_last()) {
                stateful_last_marker_seen = true;
                break;
            }
        }
        while (auto output_index = device.dequeue_ready(device.output_buf_type, 0)) {
            mark_source_buffer_dequeued(*output_index);
        }
        if (!capture_index && stateful_batches.empty())
            break;
    }
    // OUTPUT completion can trail the final CAPTURE buffer by a few
    // scheduler ticks. Give the driver a short drain window before START;
    // issuing DECODER_CMD_START with one old OUTPUT still queued returns EBUSY
    // on Iris after repeated seeks/loops.
    const auto output_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
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
    if (stateful_last_marker_seen && std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "stateful drain complete last=1 batches=%zu\n", stateful_batches.size());
    stateful_submitted_count = 0;
    stateful_completed_count = 0;
}

void Context::drain_stateful_decoder()
{
    if (!capture_initialized)
        return;

    // A timeout recovery may already have STOP-drained the decoder and seen
    // its terminal CAPTURE LAST marker. If no new AU was submitted afterward,
    // issuing another STOP during context destruction only creates a second
    // LAST event and confuses strict EOS clients.
    if (stateful_last_marker_seen && stateful_batches.empty() && stateful_pending.empty()) {
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful drain already complete last=1 batches=0\n");
        return;
    }

    // Chrome's EOS flush emits the final reordered pictures through
    // SurfaceReady(), but it has no VA call that tells this backend that no
    // more access units will arrive. STREAMOFF would therefore implicitly
    // stop Iris and discard the tail. Submit any AU still staged, then use
    // the stateful STOP drain before tearing down the queues.
    if (!stateful_pending.empty() && flush_stateful_batch() != VA_STATUS_SUCCESS)
        return;

    try {
        device.decoder_stop();
        // The STOP command drains the stateful decoder asynchronously. Mark
        // the context before waiting for the terminal CAPTURE marker so the
        // LAST path can restart Iris through resume_after_drain().
        stateful_draining = true;
    } catch (const std::system_error& error) {
        stateful_draining = false;
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "stateful drain stop failed: %s\n", error.what());
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool saw_last = false;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            auto capture_index = device.dequeue_ready(device.capture_buf_type, 50);
            if (!capture_index)
                continue;

            auto completed = surface_for_timestamp(device.last_dequeued_timestamp(), device.last_dequeued_flags());
            const bool capture_last = device.last_dequeued_last();
            if (completed && driver_data->surfaces.contains(*completed)) {
                auto& surface = driver_data->surfaces.at(*completed);
                copy_surface_frame(surface, device.buffer(device.capture_buf_type, *capture_index));
                surface.status = VASurfaceDisplaying;
                surface.source_size_used = 0;
            }
            // No consumer can hold a reference after Context destruction. Requeue
            // the slot anyway so the firmware can publish the LAST marker and the
            // following OUTPUT DQBUF completions.
            device.buffer(device.capture_buf_type, *capture_index).queue();

            while (auto output_index = device.dequeue_ready(device.output_buf_type, 0))
                mark_source_buffer_dequeued(*output_index);

            if (capture_last) {
                saw_last = true;
                stateful_last_marker_seen = true;
                break;
            }
        } catch (...) {
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
    if (stateful_capture_done.erase(index) != 0) {
        stateful_batches.erase(batch);
        remove_stateful_batch_order(index);
    } else
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

namespace {

int stateful_eos_idle_ms()
{
    if (const char* value = std::getenv("V4L2_VA_EOS_IDLE_MS")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed >= 10 && parsed <= 5000)
            return static_cast<int>(parsed);
    }
    return 120;
}

bool stateful_eos_drain_enabled()
{
    const char* value = std::getenv("V4L2_VA_EOS_DRAIN");
    return value && std::strcmp(value, "1") == 0;
}

}

// Iris only publishes the pictures still held for reordering after a STOP
// drain. Chrome has no VA call that announces the end of the stream, and it
// stops calling into this backend entirely once the last access unit has been
// submitted, so nothing would collect those frames: their VA surfaces keep the
// previous content and the compositor replays stale pictures at EOS.
bool Context::stateful_input_consumed() const
{
    if (!uses_stateful_streaming() || !capture_initialized)
        return false;
    // An access unit still staged for the next OUTPUT buffer means the
    // producer is mid-batch, not finished.
    if (!stateful_pending.empty())
        return false;
    // Nothing outstanding: every submitted picture already reached its
    // surface, so there is nothing for a drain to recover.
    if (stateful_batches.empty())
        return false;
    // A batch may still have its OUTPUT buffer in flight while Iris is in a
    // normal reorder window. STOP is the operation that flushes both that
    // buffer and the decoder DPB, so do not require OUTPUT DQBUF first.
    return true;
}

void Context::note_stateful_submission()
{
    if (!uses_stateful_streaming())
        return;
    {
        std::lock_guard<std::mutex> lock(stateful_watchdog_mutex_);
        stateful_last_submission_ = std::chrono::steady_clock::now();
        stateful_watchdog_handled_ = false;
        if (!stateful_watchdog_.joinable() && !stateful_watchdog_stop_)
            stateful_watchdog_ = std::thread(&Context::stateful_watchdog_loop, this);
    }
    stateful_watchdog_cv_.notify_all();
}

void Context::stop_stateful_watchdog()
{
    {
        std::lock_guard<std::mutex> lock(stateful_watchdog_mutex_);
        stateful_watchdog_stop_ = true;
    }
    stateful_watchdog_cv_.notify_all();
    if (stateful_watchdog_.joinable())
        stateful_watchdog_.join();
}

void Context::stateful_watchdog_loop()
{
    const auto idle_threshold = std::chrono::milliseconds(stateful_eos_idle_ms());
    while (true) {
        {
            std::unique_lock<std::mutex> lock(stateful_watchdog_mutex_);
            stateful_watchdog_cv_.wait_for(
                lock, std::chrono::milliseconds(25), [this] { return stateful_watchdog_stop_; });
            if (stateful_watchdog_stop_)
                return;
            if (stateful_watchdog_handled_)
                continue;
            if (std::chrono::steady_clock::now() - stateful_last_submission_ < idle_threshold)
                continue;
        }

        // Never block the VA-API thread. destroyContext() destroys this
        // context while it holds the driver mutex and then joins this thread,
        // so waiting for either lock here would deadlock teardown.
        std::unique_lock<std::recursive_mutex> driver_guard(driver_data->mutex, std::try_to_lock);
        if (!driver_guard.owns_lock())
            continue;
        std::unique_lock<std::recursive_mutex> guard(synchronization_mutex_, std::try_to_lock);
        if (!guard.owns_lock())
            continue;

        {
            std::lock_guard<std::mutex> lock(stateful_watchdog_mutex_);
            if (stateful_watchdog_stop_ || stateful_watchdog_handled_)
                continue;
            if (std::chrono::steady_clock::now() - stateful_last_submission_ < idle_threshold)
                continue;
        }

        const bool consumed = stateful_input_consumed();
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr,
                "stateful watchdog idle pending=%zu batches=%zu output_dequeued=%zu capture_done=%zu consumed=%d\n",
                stateful_pending.size(), stateful_batches.size(), stateful_output_dequeued.size(),
                stateful_capture_done.size(), consumed ? 1 : 0);
        if (!consumed || !stateful_eos_drain_enabled())
            continue;

        drain_stateful_decoder();
        resume_after_drain();
        std::lock_guard<std::mutex> lock(stateful_watchdog_mutex_);
        stateful_watchdog_handled_ = true;
    }
}

bool Context::bind_surface(VASurfaceID surface_id){
    if (!queues_initialized || !driver_data->surfaces.contains(surface_id))
        return false;
    if (surface_buffer_indices.contains(surface_id)) {
        if (!uses_stateful_streaming() || driver_data->surfaces.at(surface_id).destination_buffer) {
            if (std::getenv("V4L2_VA_TRACE")) {
                const auto& surface = driver_data->surfaces.at(surface_id);
                std::fprintf(stderr, "va bind existing surface=%u index=%u dest=%u queued=%d\n", surface_id,
                    surface_buffer_indices.at(surface_id), surface.destination_buffer_index,
                    surface.destination_buffer_queued);
            }
            return true;
        }
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
                adjust_capture_layout(surface.logical_destination_layout, driver_format);
            } else {
                for (unsigned plane = 0; plane < driver_format.num_planes; plane++) {
                    surface.logical_destination_layout.push_back({ plane, driver_format.plane_fmt[plane].sizeimage,
                        driver_format.plane_fmt[plane].bytesperline, 0 });
                }
            }
        }
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "va bind stateful surface=%u index=%u dest=%u queued=%d\n", surface_id, index,
                surface.destination_buffer_index, surface.destination_buffer_queued);
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
        adjust_capture_layout(surface.logical_destination_layout, driver_format);
    } else {
        for (unsigned plane = 0; plane < driver_format.num_planes; plane++) {
            surface.logical_destination_layout.push_back({ plane, driver_format.plane_fmt[plane].sizeimage,
                driver_format.plane_fmt[plane].bytesperline, 0 });
        }
    }
    surface.source_buffer = std::cref(device.buffer(device.output_buf_type, index));
    surface.source_buffer_index = index;
    surface.source_buffer_queued = false;
    surface_buffer_indices.emplace(surface_id, index);
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va bind stateless surface=%u index=%u dest=%u\n", surface_id, index,
            surface.destination_buffer_index);
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

std::optional<VASurfaceID> Context::surface_for_timestamp(timeval timestamp, uint32_t capture_flags)
{
    if (!uses_stateful_streaming())
        return std::nullopt;
    const auto exact_key = std::make_pair(static_cast<long long>(timestamp.tv_sec), static_cast<long>(timestamp.tv_usec));
    // The CAPTURE timestamp is copied from the corresponding OUTPUT buffer by
    // the V4L2 stateful decoder. A late frame whose timestamp is no longer in
    // this live map belongs to a surface that has already timed out. Never
    // bind it to the nearest future batch: doing so displays an old frame in a
    // new VA surface and permanently shifts every subsequent association.
    auto it = stateful_batch_timestamps.find(exact_key);
    if (it == stateful_batch_timestamps.end()) {
        // DECODER_CMD_STOP terminates a stateful stream with a CAPTURE LAST
        // buffer carrying timestamp zero. LAST is a control marker, not a
        // decoded frame, so it must not be reported as a timestamp miss.
        if (capture_flags & (V4L2_BUF_FLAG_LAST | V4L2_BUF_FLAG_ERROR))
            return std::nullopt;
        // A CAPTURE queue can contain stale buffers from the driver's lazy
        // source-change handshake before the first real HEVC AU completes.
        // Do not report those startup timestamps as stream mismatches; after
        // a few submitted frames, an unmatched timestamp is actionable.
        if (stateful_submitted_count < 8)
            return std::nullopt;
        // A reused Iris session can expose one or more CAPTURE buffers left
        // over from the previous stream before the first new AU completes.
        // Their timestamps precede every live batch and are harmless once
        // requeued; do not report them as decode mismatches.
        if (stateful_completed_count == 0 && !stateful_batch_timestamps.empty()
            && exact_key < stateful_batch_timestamps.begin()->first)
            return std::nullopt;
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
    stateful_completed_count++;
    if (batch->second.empty()) {
        stateful_batch_timestamps.erase(it);
        if (stateful_output_dequeued.erase(batch->first) != 0) {
            remove_stateful_batch_order(batch->first);
            stateful_batches.erase(batch);
        } else {
            stateful_capture_done.insert(batch->first);
        }
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
            if (stateful_output_dequeued.erase(batch_index) != 0) {
                stateful_batches.erase(batch);
            } else {
                stateful_capture_done.insert(batch_index);
            }
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
        std::fprintf(stderr, "va create_context tid=%ld config=%u size=%dx%d surfaces=%d\n",
            static_cast<long>(syscall(SYS_gettid)), config_id, picture_width, picture_height, surfaces_count);
    for (auto&& surface : surfaces) {
        if (!driver_data->surfaces.contains(surface)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    std::lock_guard<std::recursive_mutex> guard(driver_data->mutex);
    try {
        *context_id = smallest_free_key(driver_data->contexts);
        auto [context, inserted] = driver_data->contexts.emplace(std::make_pair(
            *context_id, Context::create(driver_data, config.profile, picture_width, picture_height, surfaces)));
        if (!inserted) {
            error_log(va_context, "Failed to create context\n");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        // vaCreateContext is the first API call that carries an explicit
        // surface list. Record that association here so later lazy exports
        // cannot bind a surface to a different decoder with matching geometry.
        for (const auto surface_id : surfaces) {
            auto& surface = driver_data->surfaces.at(surface_id);
            if (surface.owner_context == VA_INVALID_ID)
                surface.owner_context = *context_id;
        }
        driver_data->context_threads[static_cast<long>(syscall(SYS_gettid))] = *context_id;
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

    std::lock_guard<std::recursive_mutex> guard(driver_data->mutex);
    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    {
        auto& context = driver_data->contexts.at(context_id);
        // FFmpeg may still be synchronizing a decoded surface when it tears down
        // the VA context. Serialize destruction with syncSurface()/beginPicture()
        // so the stateful drain cannot race a caller holding the Context pointer.
        std::lock_guard<std::recursive_mutex> context_guard(context->synchronization_mutex());
        if (std::getenv("V4L2_VA_TRACE"))
            std::fprintf(stderr, "va destroy_context id=%u ptr=%p\n", context_id,
                context.get());
        if (context->uses_stateful_streaming())
            context->drain_stateful_decoder();
        for (auto it = driver_data->context_threads.begin(); it != driver_data->context_threads.end();) {
            if (it->second == context_id)
                it = driver_data->context_threads.erase(it);
            else
                ++it;
        }
        auto context_node = driver_data->contexts.extract(context_id);
        driver_data->retired_contexts.push_back(std::move(context_node.mapped()));
    }
    reap_retired_contexts(driver_data);

    return VA_STATUS_SUCCESS;
}
