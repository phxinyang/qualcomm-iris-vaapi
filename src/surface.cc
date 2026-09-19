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

#include "trace.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <libdrm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include "driver.h"
#include "format.h"
#include "media.h"
#include "utils.h"
#include "v4l2.h"

SurfaceStableExport::SurfaceStableExport(SurfaceStableExport&& other) noexcept
    : export_buffer_fd(std::exchange(other.export_buffer_fd, -1))
    , export_buffer_mapping(std::exchange(other.export_buffer_mapping, nullptr))
    , export_buffer_size(std::exchange(other.export_buffer_size, 0))
    , export_plane_offsets(std::move(other.export_plane_offsets))
    , stable_export_layout(std::move(other.stable_export_layout))
{
}

SurfaceStableExport& SurfaceStableExport::operator=(SurfaceStableExport&& other) noexcept
{
    if (this == &other)
        return *this;
    reset_export_buffer();
    export_buffer_fd = std::exchange(other.export_buffer_fd, -1);
    export_buffer_mapping = std::exchange(other.export_buffer_mapping, nullptr);
    export_buffer_size = std::exchange(other.export_buffer_size, 0);
    export_plane_offsets = std::move(other.export_plane_offsets);
    stable_export_layout = std::move(other.stable_export_layout);
    return *this;
}

SurfaceStableExport::~SurfaceStableExport()
{
    reset_export_buffer();
}

void SurfaceStableExport::adopt_export_buffer(int fd, void* mapping, size_t size) noexcept
{
    // The layout is selected before allocation and remains the public contract
    // for every replacement backing store, so replace only the owned handles.
    if (export_buffer_mapping)
        munmap(export_buffer_mapping, export_buffer_size);
    if (export_buffer_fd >= 0)
        close(export_buffer_fd);
    export_buffer_fd = fd;
    export_buffer_mapping = mapping;
    export_buffer_size = size;
}

void SurfaceStableExport::reset_export_buffer() noexcept
{
    if (export_buffer_mapping)
        munmap(export_buffer_mapping, export_buffer_size);
    if (export_buffer_fd >= 0)
        close(export_buffer_fd);
    export_buffer_fd = -1;
    export_buffer_mapping = nullptr;
    export_buffer_size = 0;
    export_plane_offsets.clear();
    stable_export_layout.clear();
}

namespace {

long current_tid()
{
    return static_cast<long>(syscall(SYS_gettid));
}

int dma_buf_sync_cpu(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    // Some kernels do not implement explicit sync for system-heap buffers;
    // the mapping remains usable in that case.
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int allocate_dma_buf_fd(size_t size)
{
    int heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap_fd < 0)
        return -1;
    dma_heap_allocation_data allocation = {
        .len = size,
        .fd = 0,
        .fd_flags = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };
    const int result = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation);
    close(heap_fd);
    return result < 0 ? -1 : allocation.fd;
}

bool allocate_surface_dma_buf(Surface& surface, size_t size)
{
    if (surface.export_buffer_fd >= 0 && surface.export_buffer_mapping)
        return surface.export_buffer_size >= size;
    if (surface.export_buffer_fd >= 0 || surface.export_buffer_mapping) {
        surface.reset_export_buffer();
        return false;
    }
    const int fd = allocate_dma_buf_fd(size);
    if (fd < 0)
        return false;
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return false;
    }
    surface.adopt_export_buffer(fd, mapping, size);
    // System-heap DMA-BUFs are CPU-cached. A fresh mapping may carry stale
    // cache lines and memset() dirties the cache without reaching RAM. Flush
    // the zeros so the first display and the first firmware write start from
    // a coherent black frame; later firmware completions invalidate instead
    // of flushing to avoid writing stale cache over decoded pixels.
    dma_buf_sync_cpu(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    std::memset(mapping, 0, size);
    dma_buf_sync_cpu(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    if (trace_enabled())
        std::fprintf(stderr, "va stable surface dma-buf fd=%d size=%zu\n", surface.export_buffer_fd, size);
    return true;
}

bool same_layout(const BufferLayout& left, const BufferLayout& right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].physical_plane_index != right[i].physical_plane_index
            || left[i].size != right[i].size || left[i].pitch != right[i].pitch
            || left[i].offset != right[i].offset)
            return false;
    }
    return true;
}

size_t buffer_layout_size(const BufferLayout& layout)
{
    size_t total = 0;
    for (const auto& plane : layout)
        total = std::max(total, static_cast<size_t>(plane.offset) + plane.size);
    return total;
}

size_t install_stable_export_layout(Surface& surface, BufferLayout layout, size_t physical_plane_count)
{
    // A released allocation no longer has a public pitch/offset contract, so
    // the next export may install the layout for its new backing store.
    if (surface.export_buffer_fd < 0 && !surface.export_buffer_mapping
        && surface.export_plane_offsets.empty())
        surface.stable_export_layout.clear();

    size_t cursor = 0;
    size_t total = 0;
    const bool contiguous_source = physical_plane_count <= 1;
    for (auto& plane : layout) {
        if (!contiguous_source) {
            plane.physical_plane_index = 0;
            plane.offset = static_cast<unsigned>(cursor);
        }
        total = std::max(total, static_cast<size_t>(plane.offset) + plane.size);
        if (!contiguous_source)
            cursor += plane.size;
    }

    if (!surface.stable_export_layout.empty()) {
        if (!same_layout(surface.stable_export_layout, layout))
            return 0;
    } else {
        surface.stable_export_layout = std::move(layout);
    }

    surface.export_plane_offsets.clear();
    surface.export_plane_offsets.reserve(surface.stable_export_layout.size());
    for (const auto& plane : surface.stable_export_layout)
        surface.export_plane_offsets.push_back(plane.offset);
    return total;
}

size_t prepare_compact_nv12_export_layout(Surface& surface)
{
    const auto derive_layout = lookup_format(V4L2_PIX_FMT_NV12).v4l2.derive_layout;
    if (!derive_layout)
        return 0;
    // The stable copy has visible-height planes, but its row pitch is also a
    // GPU import contract. Freedreno rejects linear R8/GR88 DMA-BUF imports
    // whose pitch is not a multiple of 64 (e.g. 720/1080-wide web videos),
    // returning EGL_BAD_ALLOC even though decoding and vaGetImage succeed.
    // Keep visible dimensions on Surface and copy only width bytes per row.
    constexpr unsigned export_pitch_alignment = 64;
    if (surface.width == 0 || surface.height == 0
        || surface.width > std::numeric_limits<unsigned>::max() - (export_pitch_alignment - 1))
        return 0;
    const unsigned pitch = (surface.width + export_pitch_alignment - 1)
        & ~(export_pitch_alignment - 1);
    if (static_cast<uint64_t>(pitch) * surface.height * 3 / 2 > std::numeric_limits<unsigned>::max())
        return 0;
    return install_stable_export_layout(surface, derive_layout(pitch, surface.height), 1);
}

size_t prepare_capture_export_layout(Surface& surface, size_t physical_plane_count)
{
    return install_stable_export_layout(surface, surface.logical_destination_layout, physical_plane_count);
}

// Whether the caller pinned the sync timeout. An explicit bound must never be
// silently raised, otherwise the documented knob does nothing in exactly the
// situation someone reaches for it.
bool stateful_sync_timeout_overridden()
{
    const char* value = std::getenv("V4L2_VA_SYNC_TIMEOUT_MS");
    if (!value)
        return false;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value && *end == '\0' && parsed > 0;
}

int stateful_sync_timeout_ms()
{
    // A bounded sync timeout allows the decoder to recover trailing B-frame DPB
    // pictures at the end of the stream or after a seek without blocking indefinitely.
    // Use a 2000ms window so normal real-time playback jitter or thread scheduling
    // does not trigger premature DPB drain resets mid-stream.
    constexpr int default_timeout_ms = 2000;
    const char* value = std::getenv("V4L2_VA_SYNC_TIMEOUT_MS");
    if (!value)
        return default_timeout_ms;

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0')
        return default_timeout_ms;
    return static_cast<int>(std::clamp(parsed, 50L, 60000L));
}

decltype(formats)::const_iterator matching_format(const V4L2M2MDevice& device, uint32_t format)
{
    return std::ranges::find_if(formats, [&](auto&& f) {
        return f.va.rt_format == format && device.format_supported(device.capture_buf_type, f.v4l2.format);
    });
}

bool prepare_stateful_zero_copy_export_layout(Context& context, Surface& surface, VASurfaceID surface_id)
{
    const auto format_match = matching_format(context.device, surface.format);
    if (format_match == formats.end())
        return false;

    std::optional<v4l2_format> probed;
    const v4l2_pix_format_mplane* driver_format = nullptr;
    if (context.initialized()) {
        const auto& live_format = context.device.capture_format.fmt.pix_mp;
        if (live_format.pixelformat == format_match->v4l2.format && live_format.width != 0
            && live_format.height != 0 && live_format.num_planes != 0
            && live_format.plane_fmt[0].bytesperline != 0 && live_format.plane_fmt[0].sizeimage != 0)
            driver_format = &live_format;
    }
    if (!driver_format) {
        probed = context.probe_stateful_capture_format(format_match->v4l2.format);
        if (!probed)
            return false;
        driver_format = &probed->fmt.pix_mp;
    }
    // The qualified contract is deliberately limited to one physical NV12
    // plane. More exotic layouts must remain on the stable-copy path until
    // their plane ownership and modifier contracts are independently proven.
    if (driver_format->pixelformat != format_match->v4l2.format || driver_format->num_planes != 1
        || driver_format->plane_fmt[0].bytesperline == 0 || driver_format->plane_fmt[0].sizeimage == 0)
        return false;

    BufferLayout layout;
    if (format_match->v4l2.derive_layout) {
        layout = format_match->v4l2.derive_layout(driver_format->width, driver_format->height);
        adjust_capture_layout(layout, *driver_format);
    } else {
        for (unsigned plane = 0; plane < driver_format->num_planes; plane++)
            layout.push_back({ plane, driver_format->plane_fmt[plane].sizeimage,
                driver_format->plane_fmt[plane].bytesperline, 0 });
    }

    const size_t required = install_stable_export_layout(surface, std::move(layout), driver_format->num_planes);
    if (required == 0)
        return false;
    surface.logical_destination_layout = surface.stable_export_layout;
    surface.capture_source_layout.clear();
    if (trace_enabled())
        std::fprintf(stderr, "stateful zero-copy export layout installed surface=%u size=%zu\n", surface_id,
            required);
    return true;
}

std::optional<V4L2FrameSizeLimits> surface_size_limits(
    const DriverData& driver_data, VAConfigID config)
{
    std::optional<VAProfile> profile;
    if (const auto config_it = driver_data.configs.find(config); config_it != driver_data.configs.end())
        profile = config_it->second.profile;

    std::optional<V4L2FrameSizeLimits> common;
    bool eligible_device_seen = false;
    for (const auto& device : driver_data.devices) {
        if (profile && !Context::supported_profiles(device).contains(*profile))
            continue;

        const auto format = matching_format(device, VA_RT_FORMAT_YUV420);
        if (format == formats.end())
            continue;
        eligible_device_seen = true;

        const auto limits = device.frame_size_limits(format->v4l2.format);
        if (!limits)
            return std::nullopt;
        if (!common) {
            common = limits;
            continue;
        }

        // A VA config may be usable on more than one decoder node. Advertise
        // the intersection so surface creation cannot select a node whose
        // range is narrower than the global query result.
        common->min_width = std::max(common->min_width, limits->min_width);
        common->min_height = std::max(common->min_height, limits->min_height);
        common->max_width = std::min(common->max_width, limits->max_width);
        common->max_height = std::min(common->max_height, limits->max_height);
    }

    if (!eligible_device_seen || !common || common->min_width > common->max_width
        || common->min_height > common->max_height)
        return std::nullopt;
    return common;
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
    const char* value = std::getenv("V4L2_VA_COPY_SURFACES");
    return !value || std::strcmp(value, "0") != 0;
}

void sync_zero_copy_display(Surface& surface) noexcept
{
    // Firmware just finished writing this DMA-BUF. The CPU mapping still
    // holds stale (often zero) cache lines from allocation or the previous
    // frame. A snooping GPU/compositor would then show green/flash even
    // though RAM is correct. Invalidate first to discard stale cache without
    // writing it back over the decoded frame, then end the CPU access so the
    // buffer is released for GPU display. START+END back-to-back leaves the
    // cache clean; a lone END would flush stale zeros over the new frame.
    if (surface.export_buffer_fd < 0)
        return;
    const int start_rc = dma_buf_sync_cpu(surface.export_buffer_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    const int start_errno = errno;
    const int end_rc = dma_buf_sync_cpu(surface.export_buffer_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    const int end_errno = errno;
    if (trace_enabled())
        std::fprintf(stderr, "zero-copy display sync fd=%d start=%d(%d) end=%d(%d)\n",
            surface.export_buffer_fd, start_rc, start_errno, end_rc, end_errno);
}

bool import_surface_dma_buf(Surface& surface, const V4L2M2MDevice::Buffer& capture, size_t size)
{
    if (!capture.uses_dmabuf() || size == 0)
        return false;
    const size_t required = prepare_capture_export_layout(surface, 1);
    if (required == 0 || required > size)
        return false;
    if (surface.export_buffer_fd >= 0 && surface.export_buffer_mapping)
        return surface.export_buffer_size >= size;

    const auto fds = capture.export_(O_RDWR);
    if (fds.size() != 1)
        return false;
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    if (mapping == MAP_FAILED) {
        close(fds[0]);
        return false;
    }
    surface.adopt_export_buffer(fds[0], mapping, size);
    if (trace_enabled())
        std::fprintf(stderr, "va zero-copy surface import fd=%d size=%zu\n", surface.export_buffer_fd, size);
    return true;
}

bool zero_copy_requested()
{
    const char* value = std::getenv("V4L2_VA_ZERO_COPY");
    return value && std::strcmp(value, "1") == 0;
}

bool zero_copy_contract_enabled()
{
    // A DMA-BUF CAPTURE slot is a decoded surface, not a generic scratch
    // buffer. Until the driver can prove multi-slot ownership, callers must
    // explicitly identify the only contract we have qualified: H.264, one AU
    // per OUTPUT, no B-frame reordering, single-plane NV12.
    const char* value = std::getenv("V4L2_VA_ZERO_COPY_CONTRACT");
    return value && std::strcmp(value, "h264-no-b-v1") == 0;
}

void copy_surface_frame(Surface& surface, const V4L2M2MDevice::Buffer& capture)
{
    if (!copy_surfaces_enabled()) {
        if (trace_enabled())
            std::fprintf(stderr, "copy_surface_frame skip fd=%d mapping=%p\n",
                surface.export_buffer_fd, surface.export_buffer_mapping);
        return;
    }
    const auto source = capture.mapping();
    if (source.empty())
        return;

    // A surface exported before context initialization may already carry the
    // padded layout returned by the stateful zero-copy probe. If importing the
    // DMA-BUF later fails, retain that public contract and copy into it; trying
    // to replace it with a compact layout would either fail the layout check or
    // make the client and V4L2 disagree about the chroma offset.
    const size_t total = surface.stable_export_layout.empty()
        ? prepare_compact_nv12_export_layout(surface)
        : buffer_layout_size(surface.stable_export_layout);
    if (total == 0)
        return;
    // Allocate lazily as well as from exportSurfaceHandle(). FFmpeg commonly
    // uses vaDeriveImage/vaGetImage without exporting a PRIME handle first;
    // those callers still need a stable snapshot because Iris rotates CAPTURE
    // buffers immediately after dequeue.
    if ((surface.export_buffer_fd < 0 || !surface.export_buffer_mapping)
        && !allocate_surface_dma_buf(surface, total))
        return;
    if (total > surface.export_buffer_size)
        return;

    // Context installs the padded V4L2 layout when it binds the CAPTURE pool.
    // Cache it before exposing the immutable compact layout to image clients.
    if (!same_layout(surface.logical_destination_layout, surface.stable_export_layout))
        surface.capture_source_layout = surface.logical_destination_layout;
    const auto& capture_layout = surface.capture_source_layout.empty()
        ? surface.logical_destination_layout
        : surface.capture_source_layout;
    if (capture_layout.size() != surface.stable_export_layout.size())
        return;

    for (size_t i = 0; i < capture_layout.size(); ++i) {
        const auto& source_plane = capture_layout[i];
        const auto& destination_plane = surface.stable_export_layout[i];
        if (source_plane.physical_plane_index >= source.size() || destination_plane.pitch == 0)
            return;
        const auto source_mapping = source[source_plane.physical_plane_index];
        const size_t source_offset = source.size() == 1 ? source_plane.offset : 0;
        const size_t source_pitch = source_plane.pitch ? source_plane.pitch : destination_plane.pitch;
        const size_t rows = i == 0 ? surface.height : surface.height / 2;
        const size_t row_bytes = surface.width;
        if (source_pitch < row_bytes || destination_plane.pitch < row_bytes || rows == 0)
            return;
        const size_t source_end = source_offset + (rows - 1) * source_pitch + row_bytes;
        const size_t destination_end = static_cast<size_t>(destination_plane.offset)
            + (rows - 1) * destination_plane.pitch + row_bytes;
        if (source_end > source_mapping.size() || destination_end > surface.export_buffer_size)
            return;
    }

    dma_buf_sync_cpu(surface.export_buffer_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    for (size_t i = 0; i < capture_layout.size(); ++i) {
        const auto& source_plane = capture_layout[i];
        const auto& destination_plane = surface.stable_export_layout[i];
        const auto source_mapping = source[source_plane.physical_plane_index];
        const size_t source_offset = source.size() == 1 ? source_plane.offset : 0;
        const size_t source_pitch = source_plane.pitch ? source_plane.pitch : destination_plane.pitch;
        const size_t rows = i == 0 ? surface.height : surface.height / 2;
        auto* destination = static_cast<uint8_t*>(surface.export_buffer_mapping) + destination_plane.offset;
        for (size_t row = 0; row < rows; ++row)
            std::memcpy(destination + row * destination_plane.pitch,
                source_mapping.data() + source_offset + row * source_pitch, surface.width);
    }
    dma_buf_sync_cpu(surface.export_buffer_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    surface.logical_destination_layout = surface.stable_export_layout;
    surface.has_completed_frame = true;
    if (trace_enabled())
        std::fprintf(stderr, "copy_surface_frame copied compact planes=%zu to mapping=%p\n",
            surface.stable_export_layout.size(), surface.export_buffer_mapping);
}

VAStatus createSurfaces2(VADriverContextP context, unsigned int format, unsigned int width, unsigned int height,
    VASurfaceID* surfaces_ids, unsigned int surfaces_count, VASurfaceAttrib* attributes, unsigned int attributes_count)
{
    // TODO inspect attributes
    // TODO ensure dimensions match previous surfaces

    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (trace_enabled())
        std::fprintf(stderr, "va create_surfaces tid=%ld format=0x%x size=%ux%u count=%u\n", current_tid(), format,
            width, height, surfaces_count);

    if (std::ranges::none_of(
            driver_data->devices, [&](auto&& d) { return matching_format(d, format) != formats.end(); })) {
        error_log(context, "No matching render target supported by device (rt_format=0x%x).\n", format);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    std::lock_guard<std::recursive_mutex> guard(driver_data->mutex);
    std::vector<VAContextID> context_ids;
    context_ids.reserve(driver_data->contexts.size());
    for (const auto& [context_id, context] : driver_data->contexts)
        context_ids.push_back(context_id);
    const auto thread_context = driver_data->context_threads.find(current_tid());
    for (unsigned i = 0; i < surfaces_count; i++) {
        surfaces_ids[i] = smallest_free_key(driver_data->surfaces);
        VAContextID owner_context = VA_INVALID_ID;
        if (thread_context != driver_data->context_threads.end()) {
            owner_context = thread_context->second;
        } else if (!context_ids.empty()) {
            owner_context = context_ids[driver_data->surface_context_cursor % context_ids.size()];
            driver_data->surface_context_cursor++;
        }
        auto [config, inserted] = driver_data->surfaces.emplace(std::make_pair(surfaces_ids[i],
            Surface { .status = VASurfaceReady,
                .width = width,
                .height = height,
                .owner_context = owner_context,
                .format = format,
                .request_fd = -1 }));
        if (!inserted) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    if (trace_enabled())
        std::fprintf(stderr, "va create_surfaces done first=%u owner=%u\n",
            surfaces_count ? surfaces_ids[0] : VA_INVALID_SURFACE,
            surfaces_count && driver_data->surfaces.contains(surfaces_ids[0])
                ? driver_data->surfaces.at(surfaces_ids[0]).owner_context
                : VA_INVALID_ID);

    return VA_STATUS_SUCCESS;
}

void createSurfacesDeferred(
    DriverData* driver_data, Context& context, std::span<VASurfaceID> surface_ids, unsigned buffer_count)
{
    if (surface_ids.size() < 1) {
        throw std::invalid_argument("No surfaces to be created");
    }
    const auto& surface = driver_data->surfaces.at(surface_ids[0]);

    const auto format_match = matching_format(context.device, surface.format);
    if (format_match == formats.end())
        throw std::invalid_argument("No matching capture format for context device");
    auto [format, derive_layout] = format_match->v4l2;

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

    // A stateful CAPTURE queue normally uses MMAP buffers which are copied into
    // a stable per-surface DMA-BUF after dequeue. The opt-in path imports those
    // stable buffers directly into V4L2, eliminating that memcpy. It is limited
    // to the single physical-plane NV12 layout used by Iris; multi-plane
    // formats keep the proven MMAP path until all plane FDs can be validated.
    // Pooled direct-export zero-copy uses the same opt-in gates as the
    // retired single-flight DMA-BUF import experiment, but keeps the MMAP
    // CAPTURE pool deep: completions are adopted by timestamp at dequeue
    // time and exported on demand, so decode pipelines instead of stalling
    // one frame at a time. The per-surface import path below stays only for
    // builds that predate this contract.
    const bool direct = zero_copy_requested() && context.uses_stateful_streaming()
        && context.zero_copy_capture_allowed() && context.zero_copy_codec_supported()
        && zero_copy_contract_enabled()
        && stateful_batch_limit() == 1
        && driver_format->num_planes == 1;
    bool zero_copy = !direct && zero_copy_requested() && context.uses_stateful_streaming()
        && context.zero_copy_capture_allowed() && context.zero_copy_codec_supported()
        && zero_copy_contract_enabled()
        && stateful_batch_limit() == 1
        && driver_format->num_planes == 1;
    if (zero_copy_requested() && context.uses_stateful_streaming() && !context.zero_copy_codec_supported()
        && trace_enabled())
        std::fprintf(stderr, "stateful zero-copy fallback reason=codec_reorder_contract\n");
    if (zero_copy_requested() && context.uses_stateful_streaming() && stateful_batch_limit() != 1
        && trace_enabled())
        std::fprintf(stderr, "stateful zero-copy fallback reason=batch_contract limit=%zu\n", stateful_batch_limit());
    if (zero_copy_requested() && context.uses_stateful_streaming() && !zero_copy_contract_enabled()
        && trace_enabled())
        std::fprintf(stderr, "stateful zero-copy fallback reason=ownership_contract\n");
    std::vector<int> zero_copy_fds;
    std::vector<size_t> zero_copy_lengths;
    std::vector<int> temporary_zero_copy_fds;
    const unsigned zero_copy_count = buffer_count;
    if (zero_copy && zero_copy_count == 0)
        zero_copy = false;

    for (unsigned i = 0; i < surface_ids.size(); i++) {
        auto& surface = driver_data->surfaces.at(surface_ids[i]);
        surface.logical_destination_layout.clear();
        surface.capture_source_layout.clear();
        if (derive_layout) { // (logical) single plane
            surface.logical_destination_layout = derive_layout(driver_format->width, driver_format->height);
            adjust_capture_layout(surface.logical_destination_layout, *driver_format);
        } else {
            for (unsigned j = 0; j < driver_format->num_planes; j += 1) {
                surface.logical_destination_layout.push_back({
                    j,
                    driver_format->plane_fmt[j].sizeimage,
                    driver_format->plane_fmt[j].bytesperline,
                    0,
                });
            }
        }

    }

    if (zero_copy) {
        const size_t size = driver_format->plane_fmt[0].sizeimage;
        for (unsigned i = 0; i < zero_copy_count; i++) {
            if (i < surface_ids.size()) {
                auto& surface = driver_data->surfaces.at(surface_ids[i]);
                const size_t required = prepare_capture_export_layout(surface, 1);
                if (size == 0 || required == 0 || required > size || !allocate_surface_dma_buf(surface, size)) {
                    zero_copy = false;
                    if (trace_enabled())
                        std::fprintf(stderr, "stateful zero-copy fallback reason=surface_dma_buf index=%u size=%zu required=%zu\n",
                            i, size, required);
                    break;
                }
                zero_copy_fds.push_back(surface.export_buffer_fd);
            } else {
                const int fd = allocate_dma_buf_fd(size);
                if (fd < 0) {
                    zero_copy = false;
                    if (trace_enabled())
                        std::fprintf(stderr, "stateful zero-copy fallback reason=extra_dma_buf index=%u size=%zu\n", i,
                            size);
                    break;
                }
                zero_copy_fds.push_back(fd);
                temporary_zero_copy_fds.push_back(fd);
            }
            zero_copy_lengths.push_back(size);
        }
    }

    if (zero_copy && zero_copy_fds.size() == zero_copy_count) {
        try {
            const unsigned requested = context.device.request_buffers_dmabuf(context.device.capture_buf_type,
                zero_copy_count, zero_copy_fds, zero_copy_lengths);
            if (requested != zero_copy_count) {
                zero_copy = false;
                if (trace_enabled())
                    std::fprintf(stderr, "stateful zero-copy fallback reason=buffer_count requested=%u expected=%u\n",
                        requested, zero_copy_count);
                context.device.request_buffers(context.device.capture_buf_type, 0);
            }
        } catch (const std::exception& error) {
            zero_copy = false;
            if (trace_enabled())
                std::fprintf(stderr, "stateful zero-copy fallback reason=%s\n", error.what());
            try {
                context.device.request_buffers(context.device.capture_buf_type, 0);
            } catch (const std::exception&) {
            }
        }
    }
    if (!zero_copy)
        context.device.request_buffers(context.device.capture_buf_type, buffer_count);

    for (const int fd : temporary_zero_copy_fds)
        close(fd);

    const unsigned bound_surfaces = std::min<unsigned>(surface_ids.size(), context.device.buffer_count(context.device.capture_buf_type));
    for (unsigned i = 0; i < bound_surfaces; i++) {
        auto& surface = driver_data->surfaces.at(surface_ids[i]);
        surface.destination_buffer = std::cref(context.device.buffer(context.device.capture_buf_type, i));
        surface.destination_buffer_index = i;
        surface.destination_buffer_queued = false;
    }
    if (trace_enabled() && context.uses_stateful_streaming())
        std::fprintf(stderr, "stateful zero-copy %s capture=%u surfaces=%u\n",
            zero_copy ? "enabled" : (direct ? "direct" : "disabled"),
            context.device.buffer_count(context.device.capture_buf_type), bound_surfaces);
}

VAStatus createSurfaces(
    VADriverContextP context, int width, int height, int format, int surfaces_count, VASurfaceID* surfaces_ids)
{
    return createSurfaces2(context, format, width, height, surfaces_ids, surfaces_count, NULL, 0);
}

VAStatus destroySurfaces(VADriverContextP context, VASurfaceID* surfaces_ids, int surfaces_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    std::lock_guard<std::recursive_mutex> guard(driver_data->mutex);
    if (trace_enabled())
        std::fprintf(stderr, "va destroy_surfaces count=%d first=%u\n", surfaces_count,
            surfaces_count ? surfaces_ids[0] : VA_INVALID_SURFACE);

    for (int i = 0; i < surfaces_count; i++) {
        if (!driver_data->surfaces.contains(surfaces_ids[i])) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        auto& surface = driver_data->surfaces.at(surfaces_ids[i]);

        // Chrome routinely retires stateful VA targets while the decoder is
        // still draining a previous access unit. Waiting in syncSurface() for
        // every such target serializes the compositor behind the bounded
        // stateful timeout and drops the frame after the caller has already
        // declared the target dead. Remove the batch mapping immediately;
        // any late CAPTURE buffer is requeued by the decoder service. Keep the
        // old synchronization behavior for request/stateless contexts, where
        // the target may still own the only decoded buffer.
        bool stateful_discarded = false;
        if (surface.owner_context != VA_INVALID_ID) {
            auto owner = driver_data->contexts.find(surface.owner_context);
            if (owner != driver_data->contexts.end() && owner->second->uses_stateful_streaming()) {
                if (trace_enabled())
                    std::fprintf(stderr, "destroy_surfaces skip stateful sync surface=%u owner=%u\n",
                        surfaces_ids[i], surface.owner_context);
                std::lock_guard<std::recursive_mutex> context_guard(owner->second->synchronization_mutex());
                owner->second->discard_stateful_surface(surfaces_ids[i]);
                stateful_discarded = true;
            }
        }
        if (!stateful_discarded && surface.source_buffer) {
            const auto* source_device = &surface.source_buffer->get().owner();
            for (auto& retired : driver_data->retired_contexts) {
                if (&retired->device == source_device && retired->uses_stateful_streaming()) {
                    if (trace_enabled())
                        std::fprintf(stderr, "destroy_surfaces skip stateful sync surface=%u retired=1\n",
                            surfaces_ids[i]);
                    std::lock_guard<std::recursive_mutex> context_guard(retired->synchronization_mutex());
                    retired->discard_stateful_surface(surfaces_ids[i]);
                    stateful_discarded = true;
                    break;
                }
            }
        }
        if (!stateful_discarded && surface.status == VASurfaceRendering)
            syncSurface(context, surfaces_ids[i]);

        if (surface.request_fd > 0)
            close(surface.request_fd);
        surface.reset_export_buffer();

        driver_data->surfaces.erase(surfaces_ids[i]);
        // A context is retained after vaDestroyContext so a late VA sync can
        // still find its V4L2 buffers. Once its final surface is gone, stop
        // the decoder now; waiting for vaTerminate() leaves an idle stateful
        // session competing with the next browser decoder.
        reap_retired_contexts(driver_data);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus syncSurface(VADriverContextP context, VASurfaceID surface_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    // Keep the surface map stable while a stateful dequeue may block. VA
    // clients are allowed to destroy a surface pool from another thread while
    // decoded frames are still being downloaded; destroySurfaces() takes this
    // same mutex before erasing entries.
    std::lock_guard<std::recursive_mutex> driver_guard(driver_data->mutex);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    if (trace_enabled())
        std::fprintf(stderr, "va sync_surface id=%u status=%u\n", surface_id, surface.status);

    if (trace_enabled())
        error_log(context, "trace sync surface=%u status=%u\\n", surface_id, surface.status);

    if (surface.status != VASurfaceRendering) {
        return VA_STATUS_SUCCESS;
    }

    try {
        if (!surface.source_buffer)
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
        if (!decode_context) {
            for (auto& candidate : driver_data->retired_contexts) {
                if (&candidate->device == &device) {
                    decode_context = candidate.get();
                    break;
                }
            }
        }
        if (!decode_context || !decode_context->capture_started())
            return VA_STATUS_ERROR_OPERATION_FAILED;
        std::lock_guard<std::recursive_mutex> guard(decode_context->synchronization_mutex());
        if (decode_context->has_pending_stateful_batch()
            && decode_context->flush_stateful_batch() != VA_STATUS_SUCCESS)
            return VA_STATUS_ERROR_OPERATION_FAILED;

        // A stateful Iris frame with B-frame reordering can arrive after the
        // OUTPUT DQBUF. Use the short bounded default (or the explicit
        // V4L2_VA_SYNC_TIMEOUT_MS override) so Chrome can continue submitting
        // AUs instead of deadlocking behind a delayed frame.
        int sync_timeout = decode_context->uses_stateful_streaming() ? stateful_sync_timeout_ms() : 10000;
        // Iris may spend several seconds parsing the first parameter set and
        // allocating its reference surfaces. That cold start is worth waiting
        // out once, but only once, and only when the caller has not asked for
        // a specific bound.
        //
        // The allowance used to apply to the first forty-eight pictures of
        // every stream and to override an explicit V4L2_VA_SYNC_TIMEOUT_MS.
        // On content the firmware cannot decode at all, that turned into a
        // thirty-second block per picture with no diagnostic and no way to
        // shorten it, which a client cannot tell apart from a hang.
        const bool cold_start = decode_context->stateful_timeout_drain()
            && decode_context->stateful_completed_frames() == 0
            && !decode_context->stateful_cold_start_exhausted();
        if (cold_start && !stateful_sync_timeout_overridden())
            sync_timeout = std::max(sync_timeout, 30000);
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(sync_timeout);
        while (surface.status == VASurfaceRendering) {
            const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (remaining <= 0)
                break;
            auto destination_index = device.dequeue_ready(device.capture_buf_type, remaining);
            if (!destination_index)
                break;
            if (decode_context->handle_capture_completion(*destination_index)) {
                try {
                    decode_context->resume_after_drain();
                } catch (const std::system_error& e) {
                    error_log(context, "Failed to resume Iris after drain: %s\\n", e.what());
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
            }

            // Iris publishes CAPTURE before recycling compressed OUTPUT.
            decode_context->service_output_queue();
            try {
                decode_context->resume_after_drain();
            } catch (const std::system_error& e) {
                error_log(context, "Failed to resume Iris after trailing OUTPUT: %s\\n", e.what());
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            decode_context->queue_zero_copy_capture();
        }
        if (surface.status == VASurfaceRendering) {
            const bool had_completed_frames = decode_context->stateful_completed_frames() != 0;
            const auto timeout_action = decode_context->on_stateful_sync_timeout(!had_completed_frames);
            // Some stateful codecs (notably HEVC with B-frame reordering)
            // publish their final CAPTURE frames only after DECODER_CMD_STOP.
            // Give the context one bounded recovery attempt before retiring
            // the VA surface, otherwise the last decoded pictures are lost
            // and callers observe stale/flash frames at EOS.
            if (timeout_action == iris::StatefulSession::TimeoutAction::RecoveryRequested) {
                if (!decode_context->reset_stateful_decoder()) {
                    error_log(context, "Stateful decoder drain did not reach LAST for surface %u\n", surface_id);
                    surface.status = VASurfaceDisplaying;
                    surface.source_size_used = 0;
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                if (surface.status != VASurfaceRendering)
                    return VA_STATUS_SUCCESS;
            }
            if (timeout_action == iris::StatefulSession::TimeoutAction::Failed) {
                error_log(context,
                    "Decoder timed out for %u consecutive surfaces after producing frames; failing context\n",
                    decode_context->stateful_consecutive_timeouts());
                surface.status = VASurfaceDisplaying;
                surface.source_size_used = 0;
                return VA_STATUS_ERROR_DECODING_ERROR;
            }
            // A cold-start wait that produced nothing has told us what it can.
            // Record it so the next picture uses the ordinary bounded timeout
            // and an undecodable stream fails in seconds instead of minutes.
            if (decode_context->stateful_completed_frames() == 0) {
                decode_context->mark_stateful_cold_start_exhausted();
                decode_context->note_stateful_barren_sync();
                // Sixteen pictures in and the decoder has never emitted one.
                // Continuing costs the caller one timeout per remaining
                // picture and still yields nothing, so report a decode error
                // and let it fall back rather than grinding through the
                // stream in silence.
                if (decode_context->stateful_barren_syncs() > 16) {
                    error_log(context,
                        "Decoder produced no frame for %u pictures; giving up on this stream\n",
                        decode_context->stateful_barren_syncs());
                    surface.status = VASurfaceDisplaying;
                    surface.source_size_used = 0;
                    return VA_STATUS_ERROR_DECODING_ERROR;
                }
            }
            if (decode_context->stateful_timeout_drain()
                && decode_context->stateful_submitted_frames() < 8) {
                error_log(context,
                    "Decoder startup backpressure surface %u completed_frame=%d; releasing provisional frame\n",
                    surface_id, surface.has_completed_frame ? 1 : 0);
            } else {
                error_log(context, "Timed out waiting for surface %u completed_frame=%d; dropping incomplete frame\n",
                    surface_id, surface.has_completed_frame ? 1 : 0);
            }
            // Preserve the last stable export. Clearing it here produces a
            // visible black flash when the compositor samples a surface after
            // a bounded EOS/DPB timeout; the incomplete frame is represented
            // by the status transition below instead.
            //
            // A timeout alone must stay patient: B-frame reordering means the
            // first pictures legitimately complete after this wait (the
            // h264-b2 fallback oracle proves it), so failing here would turn
            // a slow-but-decodable stream into a hard error. Terminal frames
            // are handled by the release_error guard at the end of this
            // function instead.
            surface.status = VASurfaceDisplaying;
            surface.source_size_used = 0;
            return VA_STATUS_SUCCESS;
        }
    } catch (std::runtime_error& e) {
        error_log(context, "Failed to dequeue buffer: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    // An error-discard release (or an ERROR CAPTURE completion) is terminal:
    // the firmware will never produce this frame, so a fresh surface still
    // holding its zero-filled export must fail instead of presenting green.
    // A surface with a previously completed frame keeps the preserved-export
    // SUCCESS path above. Timeout releases without an error stay patient
    // (B-frame reorder) and are unaffected by this guard.
    if (surface.release_error && !surface.has_completed_frame) {
        error_log(context, "Surface %u released by decoder error with no completed frame; failing\n", surface_id);
        surface.status = VASurfaceDisplaying;
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    surface.status = VASurfaceDisplaying;
    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceAttributes(
    VADriverContextP context, VAConfigID config, VASurfaceAttrib* attributes, unsigned int* attributes_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    std::lock_guard<std::recursive_mutex> guard(driver_data->mutex);
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

    if (const auto limits = surface_size_limits(*driver_data, config); limits) {
        const auto va_dimension = [](unsigned value) {
            return static_cast<int>(std::min<unsigned>(value, std::numeric_limits<int>::max()));
        };
        const auto append_dimension = [&](VASurfaceAttribType type, int value) {
            attributes_list[i].type = type;
            attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
            attributes_list[i].value.type = VAGenericValueTypeInteger;
            attributes_list[i].value.value.i = value;
            i++;
        };
        append_dimension(VASurfaceAttribMinWidth, va_dimension(limits->min_width));
        append_dimension(VASurfaceAttribMaxWidth, va_dimension(limits->max_width));
        append_dimension(VASurfaceAttribMinHeight, va_dimension(limits->min_height));
        append_dimension(VASurfaceAttribMaxHeight, va_dimension(limits->max_height));
    }

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

    std::lock_guard<std::recursive_mutex> driver_guard(driver_data->mutex);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);
    if (trace_enabled())
        std::fprintf(stderr, "va query_status id=%u status=%u dest=%d\n", surface_id, surface.status,
            surface.destination_buffer ? 1 : 0);
    if (surface.status == VASurfaceRendering && surface.destination_buffer) {
        auto& device = surface.destination_buffer->get().owner();
        for (auto& [context_id, decode_context] : driver_data->contexts) {
            if (&decode_context->device != &device || !decode_context->capture_started())
                continue;
            std::lock_guard<std::recursive_mutex> guard(decode_context->synchronization_mutex());

            try {
                while (auto capture_index = device.dequeue_ready(device.capture_buf_type, 0)) {
                    if (decode_context->handle_capture_completion(*capture_index))
                        decode_context->resume_after_drain();
                }
                decode_context->service_output_queue();
                decode_context->resume_after_drain();
                decode_context->queue_zero_copy_capture();
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

    std::lock_guard<std::recursive_mutex> driver_guard(driver_data->mutex);

    if (trace_enabled())
        std::fprintf(stderr, "va export_surface id=%u mem=0x%x flags=0x%x\n", surface_id, mem_type, flags);

    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    const bool capture_bound = surface.destination_buffer.has_value();
    if (!capture_bound && !copy_surfaces_enabled()) {
        // With no V4L2 CAPTURE binding there is nothing direct to export. A
        // successful zero-filled handle would be worse than an explicit
        // failure because the client would display stale black frames.
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (trace_enabled() && capture_bound)
        std::fprintf(stderr, "va export_surface binding id=%u capture_index=%u\n", surface_id,
            surface.destination_buffer_index);

    Context* owner_context = nullptr;
    if (surface.owner_context != VA_INVALID_ID) {
        const auto owner = driver_data->contexts.find(surface.owner_context);
        if (owner != driver_data->contexts.end())
            owner_context = owner->second.get();
    }

    std::vector<int> export_fds;
    bool stable_export = false;
    const auto mapping = capture_bound ? surface.destination_buffer->get().mapping()
                                       : std::vector<std::span<uint8_t>> {};
    try {
        if (owner_context && !surface.destination_buffer
            && zero_copy_requested() && owner_context->uses_stateful_streaming()
            && owner_context->zero_copy_capture_allowed() && owner_context->zero_copy_codec_supported()
            && zero_copy_contract_enabled() && stateful_batch_limit() == 1
            && surface.stable_export_layout.empty()) {
            if (!prepare_stateful_zero_copy_export_layout(*owner_context, surface, surface_id) && trace_enabled())
                std::fprintf(stderr, "stateful zero-copy fallback reason=export_layout_probe surface=%u\n",
                    surface_id);
        }
        // Pooled direct-export zero-copy hands the compositor the held
        // MMAP CAPTURE slot itself (EXPBUF below), so no stable snapshot is
        // allocated or duplicated once a decoded frame is bound. Chromium
        // exports its pool before the first decode, so pre-decode exports
        // keep the existing stable-buffer behavior; once a real frame is
        // held the compositor reads the slot directly, matching the
        // FFmpeg/Chromium/GStreamer export-on-dequeue discipline.
        const bool direct_export = capture_bound && owner_context
            && owner_context->zero_copy_direct_enabled() && surface.has_completed_frame
            && !surface.destination_buffer_queued;
        if (capture_bound && owner_context && owner_context->zero_copy_direct_enabled() && trace_enabled())
            std::fprintf(stderr, "va export_surface direct id=%u capture_index=%u held=%d completed=%d\n",
                surface_id, surface.destination_buffer_index,
                surface.destination_buffer_queued ? 0 : 1, surface.has_completed_frame ? 1 : 0);
        // Chromium exports a new pool before its first vaBeginPicture. Give an
        // unbound surface an independent stable NV12 buffer; exporting must
        // never bind it to (or reconfigure) an already-running decoder. The
        // zero-copy contract may have installed a padded layout above.
        if ((surface.export_buffer_fd < 0 || !surface.export_buffer_mapping) && !direct_export) {
            if (!capture_bound || copy_surfaces_enabled()) {
                const size_t required = surface.stable_export_layout.empty()
                    ? prepare_compact_nv12_export_layout(surface)
                    : buffer_layout_size(surface.stable_export_layout);
                if (required == 0 || !allocate_surface_dma_buf(surface, required))
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
        }
        if (surface.export_buffer_fd >= 0 && surface.export_buffer_mapping && !direct_export) {
            const size_t required = buffer_layout_size(surface.stable_export_layout);
            if (required == 0 || required > surface.export_buffer_size)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            const int exported = dup(surface.export_buffer_fd);
            if (exported < 0)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            export_fds.push_back(exported);
            stable_export = true;
        }
        if (!capture_bound && stable_export)
            surface.logical_destination_layout = surface.stable_export_layout;
        if (export_fds.empty() && capture_bound)
            export_fds = surface.destination_buffer->get().export_(O_RDONLY);
    } catch (std::runtime_error& e) {
        error_log(context, "Failed to export buffer: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (export_fds.empty())
        return VA_STATUS_ERROR_OPERATION_FAILED;

    surface_descriptor->fourcc = VA_FOURCC_NV12;
    surface_descriptor->width = surface.width;
    surface_descriptor->height = surface.height;
    surface_descriptor->num_objects = export_fds.size();

    const auto& format_spec = stable_export
        ? lookup_format(V4L2_PIX_FMT_NV12)
        : lookup_format(surface.destination_buffer->get().owner().capture_format.fmt.pix_mp.pixelformat);
    const auto& descriptor_layout = stable_export ? surface.stable_export_layout
                                                  : surface.logical_destination_layout;
    for (unsigned i = 0; i < export_fds.size(); i += 1) {
        surface_descriptor->objects[i].drm_format_modifier = format_spec.drm.modifier;
        surface_descriptor->objects[i].fd = export_fds[i];
        surface_descriptor->objects[i].size = stable_export
            ? surface.export_buffer_size
            : (i < mapping.size() ? mapping[i].size() : 0);
    }

    const bool separate_layers = flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS;
    if (separate_layers) {
        // VA_EXPORT_SURFACE_SEPARATE_LAYERS requires one plane per layer.
        // Chromium's NativePixmap importer relies on this shape and duplicates
        // the object FD when both NV12 planes share one dmabuf.
        surface_descriptor->num_layers = descriptor_layout.size();
        for (unsigned i = 0; i < surface_descriptor->num_layers; i++) {
            auto& layer = surface_descriptor->layers[i];
            layer.drm_format = i == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88;
            layer.num_planes = 1;
            layer.object_index[0] = stable_export ? 0 : descriptor_layout[i].physical_plane_index;
            layer.pitch[0] = descriptor_layout[i].pitch;
            layer.offset[0] = stable_export && i < surface.export_plane_offsets.size()
                ? surface.export_plane_offsets[i]
                : descriptor_layout[i].offset;
        }
    } else {
        surface_descriptor->num_layers = 1;
        surface_descriptor->layers[0].drm_format = format_spec.drm.format;
        surface_descriptor->layers[0].num_planes = descriptor_layout.size();
        for (unsigned i = 0; i < surface_descriptor->layers[0].num_planes; i++) {
            surface_descriptor->layers[0].object_index[i] = stable_export ? 0
                                                                           : descriptor_layout[i].physical_plane_index;
            surface_descriptor->layers[0].pitch[i] = descriptor_layout[i].pitch;
            surface_descriptor->layers[0].offset[i] = stable_export && i < surface.export_plane_offsets.size()
                ? surface.export_plane_offsets[i]
                : descriptor_layout[i].offset;
        }
    }

    if (trace_enabled()) {
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
