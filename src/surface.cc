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
#include <cstdlib>
#include <stdexcept>

extern "C" {
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

decltype(formats)::const_iterator matching_format(const V4L2M2MDevice& device, uint32_t format)
{
    return std::ranges::find_if(formats, [&](auto&& f) {
        return f.va.rt_format == format && device.format_supported(device.capture_buf_type, f.v4l2.format);
    });
}

} // namespace

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
            if (&candidate->device == &device && candidate->surface_for_buffer(device.capture_buf_type,
                                                    surface.destination_buffer_index)) {
                decode_context = candidate.get();
                break;
            }
        }
        if (!decode_context || !decode_context->capture_started())
            return VA_STATUS_ERROR_OPERATION_FAILED;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (surface.status == VASurfaceRendering) {
            const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (remaining <= 0)
                break;
            auto destination_index = device.dequeue_ready(device.capture_buf_type, remaining);
            if (!destination_index)
                break;
            if (auto completed = decode_context->surface_for_buffer(device.capture_buf_type, *destination_index)) {
                auto& completed_surface = driver_data->surfaces.at(*completed);
                completed_surface.status = VASurfaceDisplaying;
                completed_surface.destination_buffer_queued = false;
                completed_surface.source_size_used = 0;
                if (std::getenv("V4L2_VA_TRACE"))
                    error_log(context, "trace sync dq capture=%u surface=%u target=%u\n", *destination_index,
                        *completed, surface_id);
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
        }
        if (surface.status == VASurfaceRendering) {
            error_log(context, "Timed out waiting for surface %u\n", surface_id);
            return VA_STATUS_ERROR_OPERATION_FAILED;
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
                    const auto completed = decode_context->surface_for_buffer(device.capture_buf_type, *capture_index);
                    device.buffer(device.capture_buf_type, *capture_index).queue();
                    if (completed) {
                        auto& completed_surface = driver_data->surfaces.at(*completed);
                        completed_surface.status = VASurfaceDisplaying;
                        completed_surface.destination_buffer_queued = false;
                        completed_surface.source_size_used = 0;
                        if (std::getenv("V4L2_VA_TRACE"))
                            error_log(context, "trace query dq capture=%u surface=%u\n", *capture_index, *completed);
                    }
                    if (device.last_dequeued_last())
                        decode_context->resume_after_drain();
                }
                while (auto output_index = device.dequeue_ready(device.output_buf_type, 0)) {
                    if (auto completed = decode_context->surface_for_buffer(device.output_buf_type, *output_index)) {
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
    try {
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
    const auto& mapping = surface.destination_buffer->get().mapping();
    for (unsigned i = 0; i < export_fds.size(); i += 1) {
        surface_descriptor->objects[i].drm_format_modifier = format_spec.drm.modifier;
        surface_descriptor->objects[i].fd = export_fds[i];
        surface_descriptor->objects[i].size = mapping[i].size();
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
            layer.object_index[0] = surface.logical_destination_layout[i].physical_plane_index;
            layer.pitch[0] = surface.logical_destination_layout[i].pitch;
            layer.offset[0] = surface.logical_destination_layout[i].offset;
        }
    } else {
        surface_descriptor->num_layers = 1;
        surface_descriptor->layers[0].drm_format = format_spec.drm.format;
        surface_descriptor->layers[0].num_planes = surface.logical_destination_layout.size();
        for (unsigned i = 0; i < surface_descriptor->layers[0].num_planes; i++) {
            surface_descriptor->layers[0].object_index[i] = surface.logical_destination_layout[i].physical_plane_index;
            surface_descriptor->layers[0].pitch[i] = surface.logical_destination_layout[i].pitch;
            surface_descriptor->layers[0].offset[i] = surface.logical_destination_layout[i].offset;
        }
    }

    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va export_surface done id=%u objects=%u planes=%u\n", surface_id,
            surface_descriptor->num_objects, surface_descriptor->layers[0].num_planes);
    if (std::getenv("V4L2_VA_TRACE"))
        std::fprintf(stderr, "va export_surface desc %ux%u fourcc=0x%x mod=0x%llx size=%llu pitch0=%u pitch1=%u off0=%u off1=%u\n",
            surface_descriptor->width, surface_descriptor->height, surface_descriptor->layers[0].drm_format,
            static_cast<unsigned long long>(surface_descriptor->objects[0].drm_format_modifier),
            static_cast<unsigned long long>(surface_descriptor->objects[0].size),
            surface_descriptor->layers[0].pitch[0], surface_descriptor->layers[0].pitch[1],
            surface_descriptor->layers[0].offset[0], surface_descriptor->layers[0].offset[1]);
    return VA_STATUS_SUCCESS;
}
