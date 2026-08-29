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

#include "surface.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

extern "C" {
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <libdrm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include "driver.h"
#include "format.h"
#include "media.h"
#include "utils.h"
#include "v4l2.h"

namespace {

bool allocate_surface_dma_buf(Surface& surface, size_t size)
{
    if (surface.export_buffer_fd >= 0)
        return true;
    const int heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap_fd < 0)
        return false;
    dma_heap_allocation_data allocation = {
        .len = size,
        .fd = 0,
        .fd_flags = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };
    const int result = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation);
    close(heap_fd);
    if (result < 0)
        return false;
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, allocation.fd, 0);
    if (mapping == MAP_FAILED) {
        close(allocation.fd);
        return false;
    }
    surface.export_buffer_fd = allocation.fd;
    surface.export_buffer_mapping = mapping;
    surface.export_buffer_size = size;
    std::memset(mapping, 0, size);
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va stable surface dma-buf fd=%d size=%zu\n", allocation.fd, size);
    return true;
}

void dma_buf_sync_cpu(int fd, uint64_t flags)
{
    dma_buf_sync sync = { .flags = flags };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

size_t prepare_export_plane_offsets(Surface& surface, size_t physical_plane_count)
{
    surface.export_plane_offsets.clear();
    surface.export_plane_offsets.reserve(surface.logical_destination_layout.size());
    size_t cursor = 0;
    size_t total = 0;
    const bool contiguous_source = physical_plane_count <= 1;
    for (const auto& plane : surface.logical_destination_layout) {
        const size_t offset = contiguous_source ? plane.offset : cursor;
        surface.export_plane_offsets.push_back(static_cast<unsigned>(offset));
        total = std::max(total, offset + static_cast<size_t>(plane.size));
        if (!contiguous_source)
            cursor += plane.size;
    }
    return total;
}

decltype(formats)::const_iterator matching_format(const V4L2M2MDevice& device, uint32_t format)
{
    return std::ranges::find_if(formats, [&](auto&& f) {
        return f.va.rt_format == format && device.format_supported(device.capture_buf_type, f.v4l2.format);
    });
}

} // namespace

bool ensure_stateful_bitstream_capacity(Surface& surface, size_t required)
{
    if (required <= surface.stateful_bitstream.size())
        return true;
    size_t capacity = std::max(surface.stateful_bitstream.size(), static_cast<size_t>(SOURCE_SIZE_MAX));
    while (capacity < required) {
        if (capacity > std::numeric_limits<size_t>::max() / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    try {
        surface.stateful_bitstream.resize(capacity);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool copy_surfaces_enabled()
{
    // The rotating V4L2 CAPTURE buffer is not a stable VA export. Keep the
    // snapshot path enabled by default; set the variable to 0 only for
    // diagnostics on clients that explicitly manage CAPTURE ownership.
    const char* value = std::getenv("V4L2_VA_COPY_SURFACES");
    return !value || std::strcmp(value, "0") != 0;
}

bool stage_surface_frame(Surface& surface, const V4L2M2MDevice::Buffer& capture)
{
    if (!copy_surfaces_enabled())
        return false;
    const auto source = capture.mapping();
    if (source.empty())
        return false;
    const size_t total = prepare_export_plane_offsets(surface, source.size());
    if (total == 0)
        return false;
    try {
        if (surface.pending_frame.size() != total)
            surface.pending_frame.resize(total);
    } catch (const std::bad_alloc&) {
        return false;
    }

    bool contiguous_single_plane = source.size() == 1 && !surface.logical_destination_layout.empty();
    size_t expected_offset = 0;
    if (contiguous_single_plane) {
        for (size_t i = 0; i < surface.logical_destination_layout.size(); ++i) {
            const auto& plane = surface.logical_destination_layout[i];
            if (plane.physical_plane_index != 0 || plane.offset != expected_offset
                || i >= surface.export_plane_offsets.size() || surface.export_plane_offsets[i] != expected_offset) {
                contiguous_single_plane = false;
                break;
            }
            expected_offset += plane.size;
        }
        contiguous_single_plane = contiguous_single_plane && expected_offset == total && total <= source[0].size();
    }
    if (contiguous_single_plane) {
        std::memcpy(surface.pending_frame.data(), source[0].data(), total);
        surface.pending_frame_ready = true;
        return true;
    }

    for (size_t i = 0; i < surface.logical_destination_layout.size(); ++i) {
        const auto& plane = surface.logical_destination_layout[i];
        if (plane.physical_plane_index >= source.size())
            return false;
        const auto src = source[plane.physical_plane_index];
        const size_t src_offset = source.size() == 1 ? plane.offset : 0;
        const size_t dst_offset = surface.export_plane_offsets[i];
        if (src_offset >= src.size() || dst_offset >= surface.pending_frame.size())
            return false;
        const size_t available = std::min(src.size() - src_offset, surface.pending_frame.size() - dst_offset);
        std::memcpy(surface.pending_frame.data() + dst_offset, src.data() + src_offset,
            std::min<size_t>(plane.size, available));
    }
    surface.pending_frame_ready = true;
    return true;
}

bool publish_surface_frame(Surface& surface)
{
    if (!surface.pending_frame_ready || surface.pending_frame.empty())
        return true;
    if (surface.export_buffer_fd < 0 || !surface.export_buffer_mapping) {
        if (!allocate_surface_dma_buf(surface, surface.pending_frame.size()))
            return false;
    }
    if (surface.pending_frame.size() > surface.export_buffer_size)
        return false;
    dma_buf_sync_cpu(surface.export_buffer_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    std::memcpy(surface.export_buffer_mapping, surface.pending_frame.data(), surface.pending_frame.size());
    dma_buf_sync_cpu(surface.export_buffer_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    surface.pending_frame_ready = false;
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "publish_surface_frame bytes=%zu\n", surface.pending_frame.size());
    return true;
}

void copy_surface_frame(Surface& surface, const V4L2M2MDevice::Buffer& capture)
{
    if (stage_surface_frame(surface, capture))
        publish_surface_frame(surface);
}

VAStatus createSurfaces2(VADriverContextP context, unsigned int format, unsigned int width, unsigned int height,
    VASurfaceID* surfaces_ids, unsigned int surfaces_count, VASurfaceAttrib* attributes, unsigned int attributes_count)
{
    // TODO inspect attributes
    // TODO ensure dimensions match previous surfaces

    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va create_surfaces format=0x%x size=%ux%u count=%u\n", format, width, height,
            surfaces_count);

    if (std::ranges::none_of(
            driver_data->devices, [&](auto&& d) { return matching_format(d, format) != formats.end(); })) {
        error_log(context, "No matching render target supported by device (rt_format=0x%x).\n", format);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    for (unsigned i = 0; i < surfaces_count; i++) {
        surfaces_ids[i] = smallest_free_key(driver_data->surfaces);
        auto [config, inserted] = driver_data->surfaces.emplace(std::make_pair(surfaces_ids[i],
            Surface {
                .status = VASurfaceReady, .width = width, .height = height, .format = format, .request_fd = -1 }));
        if (!inserted) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va create_surfaces done first=%u\n", surfaces_count ? surfaces_ids[0] : VA_INVALID_SURFACE);

    return VA_STATUS_SUCCESS;
}

void createSurfacesDeferred(
    DriverData* driver_data, const Context& context, std::span<VASurfaceID> surface_ids, unsigned buffer_count)
{
    if (surface_ids.size() < 1) {
        throw std::invalid_argument("No surfaces to be created");
    }
    const auto& surface = driver_data->surfaces.at(surface_ids[0]);

    auto [format, derive_layout] = matching_format(context.device, surface.format)->v4l2;

    unsigned capture_width = surface.width;
    unsigned capture_height = surface.height;
    if (context.uses_stateful_streaming()) {
        const auto& current = context.device.capture_format.fmt.pix_mp;
        if (current.width != 0 && current.height != 0) {
            capture_width = current.width;
            capture_height = current.height;
        }
    }
    context.device.set_format(context.device.capture_buf_type, format, capture_width, capture_height);

    v4l2_pix_format_mplane* driver_format = &context.device.capture_format.fmt.pix_mp;

    context.device.request_buffers(context.device.capture_buf_type, buffer_count);

    const unsigned bound_surfaces = std::min<unsigned>(surface_ids.size(), context.device.buffer_count(context.device.capture_buf_type));
    for (unsigned i = 0; i < bound_surfaces; i++) {
        auto& surface = driver_data->surfaces.at(surface_ids[i]);
        if (derive_layout) { // (logical) single plane
            surface.logical_destination_layout = derive_layout(driver_format->width, driver_format->height);
        } else {
            for (unsigned j = 0; j < driver_format->num_planes; j += 1) {
                surface.logical_destination_layout.push_back({
                    j,
                    driver_format->plane_fmt[j].sizeimage,
                    driver_format->plane_fmt[j].bytesperline,
                    (j > 0) ? (surface.logical_destination_layout[j - 1].offset
                                  + surface.logical_destination_layout[j - 1].size)
                            : 0,
                });
            }
        }

        surface.destination_buffer = std::cref(context.device.buffer(context.device.capture_buf_type, i));
        surface.destination_buffer_index = i;
        surface.destination_buffer_queued = false;
    }
}

VAStatus createSurfaces(
    VADriverContextP context, int width, int height, int format, int surfaces_count, VASurfaceID* surfaces_ids)
{
    return createSurfaces2(context, format, width, height, surfaces_ids, surfaces_count, NULL, 0);
}

VAStatus destroySurfaces(VADriverContextP context, VASurfaceID* surfaces_ids, int surfaces_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va destroy_surfaces count=%d first=%u\n", surfaces_count,
            surfaces_count ? surfaces_ids[0] : VA_INVALID_SURFACE);
    for (int i = 0; i < surfaces_count; i++) {
        if (!driver_data->surfaces.contains(surfaces_ids[i])) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        auto& surface = driver_data->surfaces.at(surfaces_ids[i]);

        if (surface.request_fd > 0)
            close(surface.request_fd);
        if (surface.export_buffer_mapping)
            munmap(surface.export_buffer_mapping, surface.export_buffer_size);
        if (surface.export_buffer_fd >= 0)
            close(surface.export_buffer_fd);

        driver_data->surfaces.erase(surfaces_ids[i]);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus syncSurface(VADriverContextP context, VASurfaceID surface_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va sync_surface id=%u status=%u\n", surface_id, surface.status);

    if (std::getenv("V4L2_VA_TRACE"))
        error_log(context, "trace sync surface=%u status=%u\\n", surface_id, surface.status);

    if (surface.status != VASurfaceRendering) {
        return VA_STATUS_SUCCESS;
    }

    try {
        Context* pending_context = nullptr;
        for (auto& [context_id, candidate] : driver_data->contexts) {
            if (&candidate->device == &surface.source_buffer->get().owner()) {
                pending_context = candidate.get();
                break;
            }
        }
        if (pending_context && pending_context->has_pending_stateful_batch()
            && pending_context->flush_stateful_batch() != VA_STATUS_SUCCESS)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        auto& device = surface.source_buffer->get().owner();
        Context* decode_context = nullptr;
        for (auto& [context_id, candidate] : driver_data->contexts) {
            // Stateful CAPTURE indices are a rotating pool and may already
            // be rebound to another VA surface after a dropped frame. The
            // device uniquely identifies the decode context; requiring the
            // stale index-to-surface association makes sync fail early.
            if (&candidate->device == &device) {
                decode_context = candidate.get();
                break;
            }
        }
        if (!decode_context || !decode_context->capture_started())
            return VA_STATUS_ERROR_OPERATION_FAILED;

        // A stateful Iris frame normally reaches CAPTURE within a few
        // hundred milliseconds. Waiting for the full VA default timeout can
        // deadlock Chromium behind a delayed B-frame, so use a bounded retry
        // window and drop only the frame that cannot be completed.
        auto deadline = std::chrono::steady_clock::now()
            + (decode_context->uses_stateful_streaming() ? std::chrono::seconds(1) : std::chrono::seconds(10));
        while (surface.status == VASurfaceRendering) {
            const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (remaining <= 0)
                break;
            auto destination_index = device.dequeue_ready(device.capture_buf_type, remaining);
            if (!destination_index)
                break;
            auto completed = decode_context->surface_for_timestamp(device.last_dequeued_timestamp(),
                device.last_dequeued_flags());
            if (!completed && !decode_context->uses_stateful_streaming())
                completed = decode_context->surface_for_capture_flags(device.last_dequeued_flags());
            if (!completed && !decode_context->uses_stateful_streaming())
                completed = decode_context->surface_for_buffer(device.capture_buf_type, *destination_index);
            bool capture_requeued = false;
            if (completed && driver_data->surfaces.contains(*completed)) {
                auto& completed_surface = driver_data->surfaces.at(*completed);
                const bool staged = copy_surfaces_enabled()
                    && stage_surface_frame(completed_surface, device.buffer(device.capture_buf_type, *destination_index));
                if (staged && publish_surface_frame(completed_surface)) {
                    // Stable export owns the frame contents; the V4L2 slot is
                    // immediately reusable by Iris.
                    device.buffer(device.capture_buf_type, *destination_index).queue();
                    completed_surface.destination_buffer_queued = true;
                    capture_requeued = true;
                } else {
                    completed_surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type,
                        *destination_index));
                    completed_surface.destination_buffer_index = *destination_index;
                    completed_surface.destination_buffer_queued = false;
                }
                if (!completed_surface.pending_frame_ready)
                    completed_surface.status = VASurfaceDisplaying;
                completed_surface.source_size_used = 0;
                if (std::getenv("V4L2_VA_TRACE"))
                    error_log(context, "trace sync dq capture=%u surface=%u target=%u\n", *destination_index,
                        *completed, surface_id);
            } else if (std::getenv("V4L2_VA_TRACE")) {
                error_log(context, "trace sync dq capture=%u no-surface ts=%lld.%06ld target=%u\n", *destination_index,
                    static_cast<long long>(device.last_dequeued_timestamp().tv_sec),
                    static_cast<long>(device.last_dequeued_timestamp().tv_usec), surface_id);
            }
            if ((!completed || !driver_data->surfaces.contains(*completed) || device.last_dequeued_error())
                && !capture_requeued) {
                // A deliberately dropped B-frame (or the decoder's terminal
                // marker with timestamp 0) has no VA surface to update, but
                // its CAPTURE slot is still reusable.
                device.buffer(device.capture_buf_type, *destination_index).queue();
            }
            if (device.last_dequeued_last()) {
                try {
                    decode_context->resume_after_drain();
                } catch (const std::system_error& e) {
                    error_log(context, "Failed to resume Iris after drain: %s\\n", e.what());
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
            }

            // Iris publishes CAPTURE before recycling compressed OUTPUT.
            while (auto source_index = device.dequeue_ready(device.output_buf_type, 0)) {
                if (decode_context->uses_stateful_streaming())
                    decode_context->mark_source_buffer_dequeued(*source_index);
                else if (auto completed = decode_context->surface_for_buffer(device.output_buf_type, *source_index))
                    driver_data->surfaces.at(*completed).source_buffer_queued = false;
            }
            if (decode_context->uses_stateful_streaming()
                && !decode_context->has_queued_stateful_output())
                deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
        }
        if (surface.status == VASurfaceRendering) {
            error_log(context, "Timed out waiting for surface %u; dropping incomplete frame\n", surface_id);
            // Iris can leave a delayed B-frame without a CAPTURE completion
            // after its OUTPUT buffer has already been recycled. Release the
            // VA target so the producer can continue, but keep its timestamp
            // mapping alive until the late CAPTURE arrives. Removing the map
            // here turns that completion into an orphan and leaves the target
            // displaying an older exported frame at EOS.
            surface.status = VASurfaceDisplaying;
            surface.source_size_used = 0;
            return VA_STATUS_SUCCESS;
        }
    } catch (std::runtime_error& e) {
        error_log(context, "Failed to dequeue buffer: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    surface.status = VASurfaceDisplaying;

    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceAttributes(
    VADriverContextP context, VAConfigID config, VASurfaceAttrib* attributes, unsigned int* attributes_count)
{
    VASurfaceAttrib* attributes_list;
    unsigned int attributes_list_size = Config::max_attributes * sizeof(*attributes);
    int memory_types;
    unsigned int i = 0;

    attributes_list = static_cast<VASurfaceAttrib*>(malloc(attributes_list_size));
    memset(attributes_list, 0, attributes_list_size);

    attributes_list[i].type = VASurfaceAttribPixelFormat;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;
    attributes_list[i].value.value.i = VA_FOURCC_NV12;
    i++;

    attributes_list[i].type = VASurfaceAttribMinWidth;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;
    attributes_list[i].value.value.i = 32;
    i++;

    attributes_list[i].type = VASurfaceAttribMaxWidth;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;
    attributes_list[i].value.value.i = 2048;
    i++;

    attributes_list[i].type = VASurfaceAttribMinHeight;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;
    attributes_list[i].value.value.i = 32;
    i++;

    attributes_list[i].type = VASurfaceAttribMaxHeight;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;
    attributes_list[i].value.value.i = 2048;
    i++;

    attributes_list[i].type = VASurfaceAttribMemoryType;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;

    memory_types = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

    attributes_list[i].value.value.i = memory_types;
    i++;

    attributes_list_size = i * sizeof(*attributes);

    if (attributes != NULL)
        memcpy(attributes, attributes_list, attributes_list_size);

    free(attributes_list);

    *attributes_count = i;

    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceStatus(VADriverContextP context, VASurfaceID surface_id, VASurfaceStatus* status)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);
    if (surface.status == VASurfaceRendering && surface.destination_buffer) {
        auto& device = surface.destination_buffer->get().owner();
        for (auto& [context_id, decode_context] : driver_data->contexts) {
            if (&decode_context->device != &device || !decode_context->capture_started())
                continue;

            try {
                while (auto capture_index = device.dequeue_ready(device.capture_buf_type, 0)) {
                    auto completed = decode_context->surface_for_timestamp(device.last_dequeued_timestamp(),
                        device.last_dequeued_flags());
                    if (!completed && !decode_context->uses_stateful_streaming())
                        completed = decode_context->surface_for_capture_flags(device.last_dequeued_flags());
                    if (!completed && !decode_context->uses_stateful_streaming())
                        completed = decode_context->surface_for_buffer(device.capture_buf_type, *capture_index);
                    bool capture_requeued = false;
                    if (completed && driver_data->surfaces.contains(*completed)) {
                        auto& completed_surface = driver_data->surfaces.at(*completed);
                        const bool staged = copy_surfaces_enabled()
                            && stage_surface_frame(completed_surface,
                                device.buffer(device.capture_buf_type, *capture_index));
                        if (staged && publish_surface_frame(completed_surface)) {
                            device.buffer(device.capture_buf_type, *capture_index).queue();
                            completed_surface.destination_buffer_queued = true;
                            capture_requeued = true;
                        } else {
                            completed_surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type,
                                *capture_index));
                            completed_surface.destination_buffer_index = *capture_index;
                            completed_surface.destination_buffer_queued = false;
                        }
                        if (!completed_surface.pending_frame_ready)
                            completed_surface.status = VASurfaceDisplaying;
                        completed_surface.source_size_used = 0;
                        if (std::getenv("V4L2_VA_TRACE"))
                            error_log(context, "trace query dq capture=%u surface=%u\n", *capture_index, *completed);
                    } else if (!capture_requeued) {
                        device.buffer(device.capture_buf_type, *capture_index).queue();
                    }
                    if (device.last_dequeued_last())
                        decode_context->resume_after_drain();
                }
                while (auto output_index = device.dequeue_ready(device.output_buf_type, 0)) {
                    if (decode_context->uses_stateful_streaming()) {
                        decode_context->mark_source_buffer_dequeued(*output_index);
                    } else if (auto completed = decode_context->surface_for_buffer(device.output_buf_type, *output_index);
                               completed && driver_data->surfaces.contains(*completed)) {
                        auto& completed_surface = driver_data->surfaces.at(*completed);
                        completed_surface.source_buffer_queued = false;
                    }
                }
            } catch (const std::exception& e) {
                error_log(context, "Failed to reap V4L2 buffers: %s\n", e.what());
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            break;
        }
    }

    *status = surface.status;

    return VA_STATUS_SUCCESS;
}

VAStatus putSurface(VADriverContextP context, VASurfaceID surface_id, void* draw, short src_x, short src_y,
    unsigned short src_width, unsigned short src_height, short dst_x, short dst_y, unsigned short dst_width,
    unsigned short dst_height, VARectangle* cliprects, unsigned int cliprects_count, unsigned int flags)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus lockSurface(VADriverContextP context, VASurfaceID surface_id, unsigned int* fourcc, unsigned int* luma_stride,
    unsigned int* chroma_u_stride, unsigned int* chroma_v_stride, unsigned int* luma_offset,
    unsigned int* chroma_u_offset, unsigned int* chroma_v_offset, unsigned int* buffer_name, void** buffer)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus unlockSurface(VADriverContextP context, VASurfaceID surface_id)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus exportSurfaceHandle(
    VADriverContextP context, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void* descriptor)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    auto surface_descriptor = static_cast<VADRMPRIMESurfaceDescriptor*>(descriptor);

    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va export_surface id=%u mem=0x%x flags=0x%x\n", surface_id, mem_type, flags);

    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    if (!surface.destination_buffer.has_value()) {
        // Chromium exports its VA surfaces while setting up the decoder,
        // before the first vaBeginPicture call. Bind the surface to the
        // matching deferred context so the capture buffer can be exported.
        for (auto& [id, context] : driver_data->contexts) {
            if (context->picture_width < static_cast<int>(surface.width)
                || context->picture_height < static_cast<int>(surface.height))
                continue;
            std::array<VASurfaceID, 1> ids { surface_id };
            try {
                if (!context->initialized())
                    context->initialize(ids);
                if (context->bind_surface(surface_id))
                    break;
            } catch (const std::exception&) {
                continue;
            }
        }
    }

    if (!surface.destination_buffer.has_value()) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    std::vector<int> export_fds;
    bool stable_export = false;
    const auto mapping = surface.destination_buffer->get().mapping();
    try {
        if (copy_surfaces_enabled()) {
            const size_t size = prepare_export_plane_offsets(surface, mapping.size());
            if (!mapping.empty() && size != 0 && allocate_surface_dma_buf(surface, size)) {
                const int exported = dup(surface.export_buffer_fd);
                if (exported < 0)
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                export_fds.push_back(exported);
                stable_export = true;
            }
        }
        if (export_fds.empty())
            export_fds = surface.destination_buffer->get().export_(O_RDONLY);
    } catch (std::runtime_error& e) {
        error_log(context, "Failed to export buffer: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    surface_descriptor->fourcc = VA_FOURCC_NV12;
    surface_descriptor->width = surface.width;
    surface_descriptor->height = surface.height;
    surface_descriptor->num_objects = export_fds.size();

    auto format_spec = lookup_format(surface.destination_buffer->get().owner().capture_format.fmt.pix_mp.pixelformat);
    for (unsigned i = 0; i < export_fds.size(); i += 1) {
        surface_descriptor->objects[i].drm_format_modifier = format_spec.drm.modifier;
        surface_descriptor->objects[i].fd = export_fds[i];
        surface_descriptor->objects[i].size = stable_export ? surface.export_buffer_size : mapping[i].size();
    }

    const bool separate_layers = flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS;
    if (separate_layers) {
        // VA_EXPORT_SURFACE_SEPARATE_LAYERS requires one plane per layer.
        // Chromium's NativePixmap importer relies on this shape and duplicates
        // the object FD when both NV12 planes share one dmabuf.
        surface_descriptor->num_layers = surface.logical_destination_layout.size();
        for (unsigned i = 0; i < surface_descriptor->num_layers; i++) {
            auto& layer = surface_descriptor->layers[i];
            layer.drm_format = i == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88;
            layer.num_planes = 1;
            layer.object_index[0] = stable_export ? 0 : surface.logical_destination_layout[i].physical_plane_index;
            layer.pitch[0] = surface.logical_destination_layout[i].pitch;
            layer.offset[0] = stable_export && i < surface.export_plane_offsets.size()
                ? surface.export_plane_offsets[i]
                : surface.logical_destination_layout[i].offset;
        }
    } else {
        surface_descriptor->num_layers = 1;
        surface_descriptor->layers[0].drm_format = format_spec.drm.format;
        surface_descriptor->layers[0].num_planes = surface.logical_destination_layout.size();
        for (unsigned i = 0; i < surface_descriptor->layers[0].num_planes; i++) {
            surface_descriptor->layers[0].object_index[i] = stable_export ? 0
                                                                           : surface.logical_destination_layout[i].physical_plane_index;
            surface_descriptor->layers[0].pitch[i] = surface.logical_destination_layout[i].pitch;
            surface_descriptor->layers[0].offset[i] = stable_export && i < surface.export_plane_offsets.size()
                ? surface.export_plane_offsets[i]
                : surface.logical_destination_layout[i].offset;
        }
    }

    if (std::getenv("V4L2_VA_TRACE")) {
        std::fprintf(stderr, "va export_surface done id=%u size=%ux%u fourcc=0x%x objects=%u layers=%u\n", surface_id,
            surface_descriptor->width, surface_descriptor->height, surface_descriptor->fourcc,
            surface_descriptor->num_objects, surface_descriptor->num_layers);
        for (unsigned i = 0; i < surface_descriptor->num_objects; i++) {
            const auto& object = surface_descriptor->objects[i];
            std::fprintf(stderr, "va export_surface object[%u] fd=%d size=%llu modifier=0x%llx\n", i, object.fd,
                static_cast<unsigned long long>(object.size),
                static_cast<unsigned long long>(object.drm_format_modifier));
        }
        for (unsigned i = 0; i < surface_descriptor->num_layers; i++) {
            const auto& layer = surface_descriptor->layers[i];
            std::fprintf(stderr, "va export_surface layer[%u] format=0x%x planes=%u", i, layer.drm_format,
                layer.num_planes);
            for (unsigned plane = 0; plane < layer.num_planes; plane++) {
                std::fprintf(stderr, " p%u(obj=%u pitch=%u offset=%u)", plane, layer.object_index[plane],
                    layer.pitch[plane], layer.offset[plane]);
            }
            std::fputc('\n', stderr);
        }
    }
    return VA_STATUS_SUCCESS;
}
