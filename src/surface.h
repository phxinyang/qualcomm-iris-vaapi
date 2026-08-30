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

#include <functional>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

extern "C" {
#include <linux/videodev2.h>

#include <va/va_backend.h>
#include <va/va_dec_av1.h>
}

#include "context.h"
#include "format.h"
#include "v4l2.h"

struct DriverData;

// Own the stable DMA-BUF exported for a VA surface. Keep the legacy member
// names here so Context can inspect/reset a binding without sharing the
// close/munmap implementation. Moving transfers ownership; copying would
// duplicate raw handles and is therefore forbidden.
struct SurfaceStableExport {
    SurfaceStableExport() = default;
    SurfaceStableExport(const SurfaceStableExport&) = delete;
    SurfaceStableExport& operator=(const SurfaceStableExport&) = delete;
    SurfaceStableExport(SurfaceStableExport&& other) noexcept;
    SurfaceStableExport& operator=(SurfaceStableExport&& other) noexcept;
    ~SurfaceStableExport();

    void adopt_export_buffer(int fd, void* mapping, size_t size) noexcept;
    void reset_export_buffer() noexcept;

    int export_buffer_fd = -1;
    void* export_buffer_mapping = nullptr;
    size_t export_buffer_size = 0;
    // Offsets of logical VA planes in the optional contiguous export buffer.
    // For a single physical NV12 plane these retain the V4L2 offsets; for
    // NV12M they are packed consecutively into the stable buffer.
    std::vector<unsigned> export_plane_offsets;
    // Once a stable fd has been exported its pitch/offset contract cannot
    // follow later V4L2 queue reconfiguration. Keep that immutable layout
    // beside the allocation it describes.
    BufferLayout stable_export_layout;
};

struct Surface : SurfaceStableExport {
    VASurfaceStatus status;
    unsigned width;
    unsigned height;
    // VA surfaces are global, while each V4L2 stateful session has its own
    // capture queue. A client may create contexts before allocating its frame
    // pools, so retain the best-effort context association made at creation
    // time to prevent exportSurfaceHandle() from binding a surface to the
    // first decoder merely because dimensions match.
    VAContextID owner_context = VA_INVALID_ID;

    std::optional<std::reference_wrapper<const V4L2M2MDevice::Buffer>> source_buffer;
    unsigned int source_size_used;
    unsigned source_buffer_index;
    bool source_buffer_queued;
    // Stateful Iris consumes a continuous bytestream. Keep each VA picture
    // in private scratch storage until Context batches it into one OUTPUT
    // buffer.
    std::vector<uint8_t> stateful_bitstream;
    // VA submits pictures in decode order while Iris returns CAPTURE in
    // display order. Retain the VA coding type for capture association.
    unsigned stateful_frame_type = 255; // 0=P, 1=B, 2=I/IDR, 255=unknown

    std::optional<std::reference_wrapper<const V4L2M2MDevice::Buffer>> destination_buffer;
    unsigned destination_buffer_index;
    bool destination_buffer_queued;
    // Optional stable DMA-BUF backing for an exported VA surface. The
    // stateful V4L2 decoder may return any CAPTURE index for a timestamp, so
    // copy_surface_frame() updates this fixed buffer before it is displayed.
    BufferLayout logical_destination_layout;
    // Preserve the V4L2 CAPTURE layout after logical_destination_layout is
    // switched to the compact stable layout consumed by vaGetImage.
    BufferLayout capture_source_layout;
    uint32_t format;

    timeval timestamp;

    union {
        struct {
            VAPictureParameterBufferMPEG2* picture;
            VASliceParameterBufferMPEG2* slice;
            VAIQMatrixBufferMPEG2* iqmatrix;
        } mpeg2;
        struct {
            VAIQMatrixBufferH264* matrix;
            VAPictureParameterBufferH264* picture;
            VASliceParameterBufferH264* slice;
        } h264;
        struct {
            VAPictureParameterBufferVP8* picture;
            VASliceParameterBufferVP8* slice;
            VAProbabilityDataBufferVP8* probabilities;
            VAIQMatrixBufferVP8* iqmatrix;
        } vp8;
        struct {
            VADecPictureParameterBufferVP9* picture;
            VASliceParameterBufferVP9* slice;
        } vp9;
        struct {
            VAPictureParameterBufferHEVC* picture;
            VASliceParameterBufferHEVC* slice;
        } hevc;
        struct {
            VADecPictureParameterBufferAV1* picture;
            VASliceParameterBufferAV1* slice;
        } av1;
    } params;

    int request_fd;
};

// Stateful access units start in a small private buffer and grow only when a
// codec produces an AU larger than the default staging size.
bool ensure_stateful_bitstream_capacity(Surface& surface, size_t required);

void createSurfacesDeferred(
    DriverData* driver_data, Context& context, std::span<VASurfaceID> surface_ids, unsigned buffer_count);
VAStatus createSurfaces2(VADriverContextP context, unsigned int format, unsigned int width, unsigned int height,
    VASurfaceID* surfaces_ids, unsigned int surfaces_count, VASurfaceAttrib* attributes, unsigned int attributes_count);
VAStatus createSurfaces(
    VADriverContextP context, int width, int height, int format, int surfaces_count, VASurfaceID* surfaces_ids);
VAStatus destroySurfaces(VADriverContextP context, VASurfaceID* surfaces_ids, int surfaces_count);
VAStatus syncSurface(VADriverContextP context, VASurfaceID surface_id);
VAStatus querySurfaceAttributes(
    VADriverContextP context, VAConfigID config, VASurfaceAttrib* attributes, unsigned int* attributes_count);
VAStatus querySurfaceStatus(VADriverContextP context, VASurfaceID surface_id, VASurfaceStatus* status);

void copy_surface_frame(Surface& surface, const V4L2M2MDevice::Buffer& capture);
bool import_surface_dma_buf(Surface& surface, const V4L2M2MDevice::Buffer& capture, size_t size);
bool copy_surfaces_enabled();
VAStatus putSurface(VADriverContextP context, VASurfaceID surface_id, void* draw, short src_x, short src_y,
    unsigned short src_width, unsigned short src_height, short dst_x, short dst_y, unsigned short dst_width,
    unsigned short dst_height, VARectangle* cliprects, unsigned int cliprects_count, unsigned int flags);
VAStatus lockSurface(VADriverContextP context, VASurfaceID surface_id, unsigned int* fourcc, unsigned int* luma_stride,
    unsigned int* chroma_u_stride, unsigned int* chroma_v_stride, unsigned int* luma_offset,
    unsigned int* chroma_u_offset, unsigned int* chroma_v_offset, unsigned int* buffer_name, void** buffer);
VAStatus unlockSurface(VADriverContextP context, VASurfaceID surface_id);
VAStatus exportSurfaceHandle(
    VADriverContextP context, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void* descriptor);
