/*
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

#include "v4l2.h"

#include "trace.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <system_error>

extern "C" {
#include <fcntl.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <libudev.h>
}

#include "utils.h"

namespace {

uint32_t query_capabilities(int video_fd)
{
    v4l2_capability capability = {};
    errno_wrapper(ioctl, video_fd, VIDIOC_QUERYCAP, &capability);

    if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0) {
        return capability.device_caps;
    } else {
        return capability.capabilities;
    }
}

v4l2_format get_format(int video_fd, v4l2_buf_type type)
{
    v4l2_format result = { .type = type };
    errno_wrapper(ioctl, video_fd, VIDIOC_G_FMT, &result);
    return result;
}

std::vector<std::span<uint8_t>> map_buffer(int video_fd, v4l2_buf_type type, unsigned index, uint32_t* query_flags)
{
    v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    v4l2_buffer buffer = {
        .index = index,
        .type = type,
        .m = { .planes = planes },
        .length = VIDEO_MAX_PLANES,
    };
    errno_wrapper(ioctl, video_fd, VIDIOC_QUERYBUF, &buffer);
    *query_flags = buffer.flags;

    if (!V4L2_TYPE_IS_MULTIPLANAR(type)) { // reduce singleplanar API to single-plane in multiplanar buffer
        const auto offset = buffer.m.offset;
        buffer.m.planes = planes;
        buffer.m.planes[0].length = buffer.length;
        buffer.m.planes[0].m.mem_offset = offset;
        buffer.length = 1;
    }

    std::vector<std::span<uint8_t>> result(buffer.length);
    for (unsigned i = 0; i < buffer.length; i++) {
        result[i] = { static_cast<uint8_t*>(mmap(NULL, buffer.m.planes[i].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                          video_fd, buffer.m.planes[i].m.mem_offset)),
            buffer.m.planes[i].length };
        if (result[i].data() == MAP_FAILED) {
            throw std::system_error(errno, std::generic_category());
        }
    }
    return result;
}

std::vector<std::string> enumerate_video_devices(udev* ctx, const std::string& media_device)
{
    int fd = errno_wrapper(open, media_device.c_str(), O_RDONLY);

    media_device_info device_info = {};
    errno_wrapper(ioctl, fd, MEDIA_IOC_DEVICE_INFO, &device_info);

    media_v2_topology topology = {};
    errno_wrapper(ioctl, fd, MEDIA_IOC_G_TOPOLOGY, &topology);

    std::vector<media_v2_entity> entities(topology.num_entities);
    std::vector<media_v2_interface> interfaces(topology.num_interfaces);
    topology.ptr_entities = reinterpret_cast<uint64_t>(entities.data());
    topology.ptr_interfaces = reinterpret_cast<uint64_t>(interfaces.data());

    errno_wrapper(ioctl, fd, MEDIA_IOC_G_TOPOLOGY, &topology);
    close(fd);

    if (std::ranges::find_if(entities, [](auto&& entity) { return entity.function == MEDIA_ENT_F_PROC_VIDEO_DECODER; })
        == entities.end()) {
        return {};
    }

    std::vector<std::string> result;
    for (auto&& interface : interfaces) {
        auto devnum = makedev(interface.devnode.major, interface.devnode.minor);
        std::unique_ptr<udev_device, decltype(&udev_device_unref)> device(
            udev_device_new_from_devnum(ctx, 'c', devnum), &udev_device_unref);
        if (device && interface.intf_type == MEDIA_INTF_T_V4L_VIDEO) {
            result.push_back(udev_device_get_property_value(device.get(), "DEVNAME"));
        }
    }

    return result;
}

std::vector<std::string> enumerate_media_devices(udev* ctx)
{
    std::unique_ptr<udev_enumerate, decltype(&udev_enumerate_unref)> enumerate(
        udev_enumerate_new(ctx), &udev_enumerate_unref);

    udev_enumerate_add_match_subsystem(enumerate.get(), "media");
    udev_enumerate_scan_devices(enumerate.get());

    std::vector<std::string> result;
    for (auto entry = udev_enumerate_get_list_entry(enumerate.get()); entry != nullptr;
         entry = udev_list_entry_get_next(entry)) {
        auto name = udev_list_entry_get_name(entry);

        std::unique_ptr<udev_device, decltype(&udev_device_unref)> device(
            udev_device_new_from_syspath(ctx, name), &udev_device_unref);

        if (device) {
            result.push_back(udev_device_get_property_value(device.get(), "DEVNAME"));
        }
    }

    return result;
}

} // namespace

namespace {

// Compressed formats this driver can drive on the OUTPUT queue. Used to tell a
// decoder apart from an encoder or a camera scaler when there is no media
// topology to consult.
bool advertises_decoder_output(int fd, uint32_t capabilities)
{
    static constexpr uint32_t decoder_formats[] = {
        V4L2_PIX_FMT_MPEG2_SLICE,
        V4L2_PIX_FMT_H264,
        V4L2_PIX_FMT_H264_SLICE,
        V4L2_PIX_FMT_VP8_FRAME,
        V4L2_PIX_FMT_VP9,
        V4L2_PIX_FMT_VP9_FRAME,
        V4L2_PIX_FMT_HEVC,
        V4L2_PIX_FMT_AV1,
    };
    const auto type = (capabilities & V4L2_CAP_VIDEO_M2M) ? V4L2_BUF_TYPE_VIDEO_OUTPUT
                                                          : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    for (v4l2_fmtdesc fmtdesc = { .type = static_cast<uint32_t>(type) };
         ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0; fmtdesc.index += 1) {
        for (const auto format : decoder_formats) {
            if (fmtdesc.pixelformat == format)
                return true;
        }
    }
    return false;
}

// Every video node that looks like a decoder, regardless of whether a media
// controller knows about it.
//
// The media-topology walk above can only find stateless decoders: they need
// the Request API, so they always publish a media device with a
// MEDIA_ENT_F_PROC_VIDEO_DECODER entity. A stateful decoder needs neither, and
// on the Qualcomm Iris target it publishes no media node at all while the only
// media device present belongs to the camera subsystem. Probing therefore
// found nothing and the driver advertised zero profiles unless
// LIBVA_V4L2_VIDEO_PATH was set by hand.
std::vector<std::string> enumerate_standalone_video_devices(udev* ctx)
{
    std::unique_ptr<udev_enumerate, decltype(&udev_enumerate_unref)> enumerate(
        udev_enumerate_new(ctx), &udev_enumerate_unref);

    udev_enumerate_add_match_subsystem(enumerate.get(), "video4linux");
    udev_enumerate_scan_devices(enumerate.get());

    std::vector<std::string> result;
    for (auto entry = udev_enumerate_get_list_entry(enumerate.get()); entry != nullptr;
         entry = udev_list_entry_get_next(entry)) {
        std::unique_ptr<udev_device, decltype(&udev_device_unref)> device(
            udev_device_new_from_syspath(ctx, udev_list_entry_get_name(entry)), &udev_device_unref);
        if (!device)
            continue;
        const char* devname = udev_device_get_property_value(device.get(), "DEVNAME");
        if (!devname)
            continue;

        // Plain open: a node may be busy or owned by another user, and one
        // unusable camera node must not prevent the decoder from being found.
        const int fd = open(devname, O_RDONLY);
        if (fd < 0)
            continue;
        bool supported = false;
        try {
            const uint32_t capabilities = query_capabilities(fd);
            supported = (capabilities & V4L2M2MDevice::required_capabilities) != 0
                && advertises_decoder_output(fd, capabilities);
        } catch (const std::exception&) {
            supported = false;
        }
        close(fd);
        if (supported)
            result.push_back(devname);
    }

    return result;
}

} // namespace

std::vector<std::pair<std::string, std::optional<std::string>>> V4L2M2MDevice::enumerate_devices()
{
    std::vector<std::pair<std::string, std::optional<std::string>>> result;

    std::unique_ptr<udev, decltype(&udev_unref)> ctx(udev_new(), &udev_unref);
    for (auto&& media_device : enumerate_media_devices(ctx.get())) {
        std::vector<std::string> video_devices;
        try {
            video_devices = enumerate_video_devices(ctx.get(), media_device);
        } catch (const std::exception& error) {
            // A media node that cannot be opened or does not answer
            // G_TOPOLOGY says nothing about the other devices on the system.
            if (trace_enabled())
                std::fprintf(stderr, "v4l2 skipping media device %s: %s\n", media_device.c_str(),
                    error.what());
            continue;
        }
        for (auto&& video_device : video_devices) {
            const int fd = open(video_device.c_str(), O_RDONLY);
            if (fd < 0)
                continue;
            const bool supported = (query_capabilities(fd) & required_capabilities) != 0;
            close(fd);
            if (supported) {
                result.emplace_back(video_device, media_device);
            }
        }
    }

    for (auto&& video_device : enumerate_standalone_video_devices(ctx.get())) {
        const bool known = std::ranges::any_of(
            result, [&](const auto& entry) { return entry.first == video_device; });
        if (!known)
            result.emplace_back(video_device, std::nullopt);
    }

    if (trace_enabled()) {
        for (const auto& [video_device, media_device] : result)
            std::fprintf(stderr, "v4l2 probe found %s media=%s\n", video_device.c_str(),
                media_device ? media_device->c_str() : "none");
    }

    return result;
}

V4L2M2MDevice::Buffer::Buffer(V4L2M2MDevice& owner, v4l2_buf_type type, unsigned index)
    : owner_(owner)
    , type_(type)
    , index_(index)
    , memory_(V4L2_MEMORY_MMAP)
    , query_flags_(0)
    , mapping_(map_buffer(owner.video_fd, type, index, &query_flags_))
{
}

V4L2M2MDevice::Buffer::Buffer(V4L2M2MDevice& owner, v4l2_buf_type type, unsigned index, int dmabuf_fd,
    size_t dmabuf_size)
    : owner_(owner)
    , type_(type)
    , index_(index)
    , memory_(V4L2_MEMORY_DMABUF)
    , query_flags_(0)
{
    if (dmabuf_fd < 0)
        throw std::invalid_argument("Invalid DMA-BUF fd");

    v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    v4l2_buffer buffer = {
        .index = index,
        .type = type,
        .memory = V4L2_MEMORY_DMABUF,
        .m = { .planes = planes },
        .length = VIDEO_MAX_PLANES,
    };
    errno_wrapper(ioctl, owner.video_fd, VIDIOC_QUERYBUF, &buffer);
    query_flags_ = buffer.flags & ~V4L2_BUF_FLAG_MAPPED;

    unsigned plane_count = buffer.length;
    size_t required_size = 0;
    if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
        if (plane_count != 1)
            throw std::invalid_argument("Only single-plane DMA-BUF capture is supported");
        required_size = buffer.m.planes[0].length;
    } else {
        plane_count = 1;
        required_size = buffer.length;
    }
    if (dmabuf_size < required_size)
        throw std::invalid_argument("DMA-BUF is smaller than the V4L2 plane");

    const int owned_fd = dup(dmabuf_fd);
    if (owned_fd < 0)
        throw std::system_error(errno, std::generic_category(), "dup DMA-BUF");
    dmabuf_fds_.push_back(owned_fd);
    plane_lengths_.push_back(dmabuf_size);
    if (plane_count != dmabuf_fds_.size()) {
        close(owned_fd);
        dmabuf_fds_.clear();
        plane_lengths_.clear();
        throw std::invalid_argument("DMA-BUF plane count mismatch");
    }
}

V4L2M2MDevice::Buffer::Buffer(V4L2M2MDevice::Buffer&& other)
    : owner_(other.owner_)
    , type_(other.type_)
    , index_(other.index_)
    , memory_(other.memory_)
    , query_flags_(other.query_flags_)
    , mapping_(other.mapping_)
    , dmabuf_fds_(other.dmabuf_fds_)
    , plane_lengths_(other.plane_lengths_)
{
    other.mapping_.clear();
    other.dmabuf_fds_.clear();
    other.plane_lengths_.clear();
}

V4L2M2MDevice::Buffer& V4L2M2MDevice::Buffer::operator=(V4L2M2MDevice::Buffer&& other)
{
    this->~Buffer();
    new (this) V4L2M2MDevice::Buffer(std::move(other));
    return *this;
}

V4L2M2MDevice::Buffer::~Buffer()
{
    for (auto&& map : mapping_) {
        munmap(map.data(), map.size());
    }
    for (const int fd : dmabuf_fds_)
        close(fd);
}

void V4L2M2MDevice::Buffer::queue(int request_fd, timeval* timestamp, unsigned size) const
{
    queue_internal(-1, 0, request_fd, timestamp, size);
}

void V4L2M2MDevice::Buffer::queue_with_dmabuf(int dmabuf_fd, size_t dmabuf_size, int request_fd,
    timeval* timestamp, unsigned size) const
{
    if (!uses_dmabuf() || dmabuf_fd < 0 || dmabuf_size == 0 || dmabuf_fds_.size() != 1)
        throw std::system_error(EINVAL, std::generic_category(), "invalid DMA-BUF queue override");
    if (dmabuf_size < plane_lengths_.front())
        throw std::system_error(EINVAL, std::generic_category(), "DMA-BUF queue override is too small");
    queue_internal(dmabuf_fd, dmabuf_size, request_fd, timestamp, size);
}

void V4L2M2MDevice::Buffer::queue_internal(int dmabuf_fd, size_t dmabuf_size, int request_fd,
    timeval* timestamp, unsigned size) const
{
    const bool dmabuf_override = dmabuf_fd >= 0;
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    struct v4l2_buffer buffer = {
        .index = index_,
        .type = type_,
        .memory = memory_,
        .m = { .planes = planes },
        .length = static_cast<uint32_t>(uses_dmabuf() ? dmabuf_fds_.size() : mapping_.size()),
    };
    // Native Iris clients submit both queues with FIELD_NONE. FIELD_ANY is
    // accepted for the initial CAPTURE QBUF but can make a later buffer fail
    // validation in the firmware.
    buffer.field = V4L2_FIELD_NONE;

    if (V4L2_TYPE_IS_MULTIPLANAR(type_)) {
        const unsigned plane_count = uses_dmabuf() ? dmabuf_fds_.size() : mapping_.size();
        for (unsigned i = 0; i < plane_count; i++) {
            // MMAP QBUF carries the mapped plane length. DMA-BUF QBUF carries
            // the imported fd and its allocation length instead.
            if (uses_dmabuf()) {
                buffer.m.planes[i].m.fd = dmabuf_override ? dmabuf_fd : dmabuf_fds_[i];
                buffer.m.planes[i].length = dmabuf_override ? dmabuf_size : plane_lengths_[i];
            } else {
                buffer.m.planes[i].length = mapping_[i].size();
            }
            buffer.m.planes[i].bytesused = size;
        }
    } else {
        if (uses_dmabuf()) {
            buffer.m.fd = dmabuf_override ? dmabuf_fd : dmabuf_fds_[0];
            buffer.length = dmabuf_override ? dmabuf_size : plane_lengths_[0];
        }
        buffer.bytesused = size;
    }

    if (request_fd >= 0) {
        buffer.flags |= V4L2_BUF_FLAG_REQUEST_FD;
        buffer.request_fd = request_fd;
    }

    // Preserve the MMAP/timestamp flags returned by QUERYBUF on both queues.
    // The tablet's native v4l2-ctl client sends these flags for initial
    // CAPTURE QBUF as well as compressed OUTPUT.
    buffer.flags |= query_flags_;
    if (!uses_dmabuf())
        buffer.flags |= V4L2_BUF_FLAG_MAPPED;

    if (timestamp != NULL)
        buffer.timestamp = *timestamp;

    if (trace_enabled())
        std::fprintf(stderr, "v4l2 q type=%u index=%u size=%u planes=%u len=%u field=%u\\n", type_, index_, size,
            buffer.length, buffer.m.planes ? buffer.m.planes[0].length : buffer.length, buffer.field);
    if (trace_enabled() && dmabuf_override)
        std::fprintf(stderr, "v4l2 q dmabuf override index=%u fd=%d len=%zu\\n", index_, dmabuf_fd, dmabuf_size);

    errno_wrapper(ioctl, owner_.video_fd, VIDIOC_QBUF, &buffer);
}

unsigned V4L2M2MDevice::Buffer::dequeue() const
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    struct v4l2_buffer buffer = {
        .type = type_,
        .memory = memory_,
        .m = { .planes = planes },
        .length = static_cast<uint32_t>(uses_dmabuf() ? dmabuf_fds_.size() : VIDEO_MAX_PLANES),
    };

    errno_wrapper(ioctl, owner_.video_fd, VIDIOC_DQBUF, &buffer);
    if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
        throw std::runtime_error("Dequeued buffer marked erroneous by driver.");
    }
    return buffer.index;
}

std::vector<int> V4L2M2MDevice::Buffer::export_(unsigned flags) const
{
    std::vector<int> result;
    if (uses_dmabuf()) {
        for (const int fd : dmabuf_fds_) {
            const int exported = dup(fd);
            if (exported < 0)
                throw std::system_error(errno, std::generic_category(), "dup DMA-BUF");
            result.push_back(exported);
        }
        return result;
    }
    for (unsigned i = 0; i < mapping_.size(); i++) {
        v4l2_exportbuffer exportbuffer = {
            .type = type_,
            .index = index_,
            .plane = i,
            .flags = flags,
        };

        errno_wrapper(ioctl, owner_.video_fd, VIDIOC_EXPBUF, &exportbuffer);
        result.push_back(exportbuffer.fd);
    }
    return result;
}

V4L2M2MDevice::V4L2M2MDevice(const std::string& video_path, const std::optional<std::string>& media_path)
    : video_path_(video_path)
    , media_path_(media_path)
    , video_fd(errno_wrapper(open, video_path.c_str(), O_RDWR | O_NONBLOCK))
    , media_fd((media_path) ? errno_wrapper(open, media_path->c_str(), O_RDWR | O_NONBLOCK) : -1)
    , capabilities(query_capabilities(video_fd))
    , capture_buf_type(
          (capabilities & V4L2_CAP_VIDEO_M2M) ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
    , output_buf_type(
          (capabilities & V4L2_CAP_VIDEO_M2M) ? V4L2_BUF_TYPE_VIDEO_OUTPUT : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
    , capture_format(get_format(video_fd, capture_buf_type))
    , output_format(get_format(video_fd, output_buf_type))
{
    if (!(capabilities & required_capabilities)) {
        throw std::runtime_error("Missing device capabilities");
    }
}

V4L2M2MDevice::V4L2M2MDevice(V4L2M2MDevice&& other)
    : video_path_(std::move(other.video_path_))
    , media_path_(std::move(other.media_path_))
    , video_fd(std::move(other.video_fd))
    , media_fd(std::move(other.media_fd))
    , capabilities(std::move(other.capabilities))
    , capture_buf_type(std::move(other.capture_buf_type))
    , output_buf_type(std::move(other.output_buf_type))
    , capture_format(std::move(other.capture_format))
    , output_format(std::move(other.output_format))
    , supported_output_formats(std::move(other.supported_output_formats))
    , supported_capture_formats(std::move(other.supported_capture_formats))
    , capture_buffers(std::move(other.capture_buffers))
    , output_buffers(std::move(other.output_buffers))
    , output_streaming(other.output_streaming)
    , capture_streaming(other.capture_streaming)
    , last_dequeued_was_last(other.last_dequeued_was_last)
    , last_dequeued_error_(other.last_dequeued_error_)
    , last_dequeued_flags_(other.last_dequeued_flags_)
    , last_dequeued_timestamp_(other.last_dequeued_timestamp_)
{
    other.capture_buffers.clear();
    other.output_buffers.clear();
    other.video_fd = -1;
    other.media_fd = -1;
    other.output_streaming = false;
    other.capture_streaming = false;
    other.last_dequeued_was_last = false;
    other.last_dequeued_error_ = false;
    other.last_dequeued_flags_ = 0;
    other.last_dequeued_timestamp_ = {};
}

V4L2M2MDevice V4L2M2MDevice::clone_for_context() const
{
    return V4L2M2MDevice(video_path_, media_path_);
}

V4L2M2MDevice& V4L2M2MDevice::operator=(V4L2M2MDevice&& other)
{
    this->~V4L2M2MDevice();
    new (this) V4L2M2MDevice(std::move(other));
    return *this;
}

V4L2M2MDevice::~V4L2M2MDevice()
{
    if (video_fd >= 0) {
        close(video_fd);
    }
    if (media_fd >= 0) {
        close(media_fd);
    }
}

void V4L2M2MDevice::set_format(v4l2_buf_type type, unsigned int pixelformat, unsigned int width, unsigned int height)
{
    struct v4l2_format* format = V4L2_TYPE_IS_CAPTURE(type) ? &capture_format : &output_format;

    format->type = type;
    format->fmt.pix_mp.pixelformat = pixelformat;
    format->fmt.pix_mp.width = width;
    format->fmt.pix_mp.height = height;
    if (V4L2_TYPE_IS_OUTPUT(type)) {
        const unsigned sizeimage = format->fmt.pix_mp.plane_fmt[0].sizeimage;
        for (auto& plane : format->fmt.pix_mp.plane_fmt)
            plane = {};
        format->fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage ? sizeimage : SOURCE_SIZE_MAX;
    } else {
        for (auto& plane : format->fmt.pix_mp.plane_fmt)
            plane.bytesperline = 0;
    }

    // Automatic size is insufficient for compressed OUTPUT buffers. Capture
    // keeps the allocation hint returned by the preceding G_FMT, matching
    // v4l2-ctl's stateful Iris setup.
    if (V4L2_TYPE_IS_OUTPUT(type) && format->fmt.pix_mp.plane_fmt[0].sizeimage == 0)
        format->fmt.pix_mp.plane_fmt[0].sizeimage = SOURCE_SIZE_MAX;

    errno_wrapper(ioctl, video_fd, VIDIOC_S_FMT, format);
}

unsigned V4L2M2MDevice::request_buffers(v4l2_buf_type type, unsigned count)
{
    struct v4l2_requestbuffers req_buffers = {
        .count = count,
        .type = type,
        .memory = V4L2_MEMORY_MMAP,
    };

    errno_wrapper(ioctl, video_fd, VIDIOC_REQBUFS, &req_buffers);

    auto& buffers = V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers;

    buffers.clear();
    for (unsigned i = 0; i < req_buffers.count; i += 1) {
        buffers.emplace_back(*this, type, i);
    }

    return buffers.size(); // Actual amount may differ
}

unsigned V4L2M2MDevice::request_buffers_dmabuf(v4l2_buf_type type, unsigned count, std::span<const int> fds,
    std::span<const size_t> lengths)
{
    if (fds.size() < count || lengths.size() < count)
        throw std::invalid_argument("Not enough DMA-BUFs for V4L2 queue");

    struct v4l2_requestbuffers req_buffers = {
        .count = count,
        .type = type,
        .memory = V4L2_MEMORY_DMABUF,
    };
    errno_wrapper(ioctl, video_fd, VIDIOC_REQBUFS, &req_buffers);

    if (req_buffers.count > fds.size() || req_buffers.count > lengths.size()) {
        v4l2_requestbuffers release = { .type = type, .memory = V4L2_MEMORY_DMABUF };
        ioctl(video_fd, VIDIOC_REQBUFS, &release);
        throw std::invalid_argument("V4L2 returned more DMA-BUF slots than provided");
    }

    auto& buffers = V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers;
    buffers.clear();
    try {
        for (unsigned i = 0; i < req_buffers.count; i++)
            buffers.emplace_back(*this, type, i, fds[i], lengths[i]);
    } catch (...) {
        buffers.clear();
        v4l2_requestbuffers release = { .type = type, .memory = V4L2_MEMORY_DMABUF };
        ioctl(video_fd, VIDIOC_REQBUFS, &release);
        throw;
    }

    return buffers.size();
}

bool V4L2M2MDevice::format_supported(v4l2_buf_type type, unsigned pixelformat) const
{
    for (v4l2_fmtdesc fmtdesc = { .type = type }; ioctl(video_fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0; fmtdesc.index += 1) {
        if (fmtdesc.pixelformat == pixelformat) {
            return true;
        }
    }
    return false;
}

std::optional<V4L2FrameSizeLimits> V4L2M2MDevice::frame_size_limits(unsigned pixelformat) const
{
    V4L2FrameSizeLimits result {
        .min_width = std::numeric_limits<unsigned>::max(),
        .min_height = std::numeric_limits<unsigned>::max(),
        .max_width = 0,
        .max_height = 0,
    };

    bool found = false;
    for (v4l2_frmsizeenum framesize = { .index = 0, .pixel_format = pixelformat };; framesize.index++) {
        if (ioctl(video_fd, VIDIOC_ENUM_FRAMESIZES, &framesize) < 0)
            break;

        switch (framesize.type) {
        case V4L2_FRMSIZE_TYPE_DISCRETE:
            result.min_width = std::min(result.min_width, framesize.discrete.width);
            result.min_height = std::min(result.min_height, framesize.discrete.height);
            result.max_width = std::max(result.max_width, framesize.discrete.width);
            result.max_height = std::max(result.max_height, framesize.discrete.height);
            found = true;
            break;
        case V4L2_FRMSIZE_TYPE_STEPWISE:
        case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            result.min_width = std::min(result.min_width, framesize.stepwise.min_width);
            result.min_height = std::min(result.min_height, framesize.stepwise.min_height);
            result.max_width = std::max(result.max_width, framesize.stepwise.max_width);
            result.max_height = std::max(result.max_height, framesize.stepwise.max_height);
            found = true;
            break;
        default:
            break;
        }
    }

    if (!found || result.max_width == 0 || result.max_height == 0
        || result.min_width > result.max_width || result.min_height > result.max_height)
        return std::nullopt;
    return result;
}

std::optional<std::set<int32_t>> V4L2M2MDevice::menu_control_values(uint32_t id) const
{
    v4l2_queryctrl control = { .id = id };
    if (ioctl(video_fd, VIDIOC_QUERYCTRL, &control) < 0)
        return std::nullopt;
    if ((control.flags & V4L2_CTRL_FLAG_DISABLED) != 0
        || (control.type != V4L2_CTRL_TYPE_MENU && control.type != V4L2_CTRL_TYPE_INTEGER_MENU)
        || control.minimum < 0 || control.maximum < control.minimum)
        return std::nullopt;

    std::set<int32_t> result;
    for (int64_t index = control.minimum; index <= control.maximum; ++index) {
        v4l2_querymenu menu = {
            .id = id,
            .index = static_cast<uint32_t>(index),
        };
        if (ioctl(video_fd, VIDIOC_QUERYMENU, &menu) == 0)
            result.insert(static_cast<int32_t>(index));
    }
    return result;
}

unsigned V4L2M2MDevice::buffer_count(v4l2_buf_type type) const
{
    return (V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers).size();
}

const V4L2M2MDevice::Buffer& V4L2M2MDevice::buffer(v4l2_buf_type type, unsigned index)
{
    return (V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers)[index];
}

const V4L2M2MDevice::Buffer& V4L2M2MDevice::buffer(v4l2_buf_type type, unsigned index) const
{
    return (V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers)[index];
}

int32_t V4L2M2MDevice::get_control(uint32_t id) const {
    v4l2_control ctrl = {
        .id = id,
    };
    errno_wrapper(ioctl, video_fd, VIDIOC_G_CTRL, &ctrl);
    return ctrl.value;
}

void V4L2M2MDevice::set_ext_control(int request_fd, unsigned id, void* data, unsigned size)
{
    v4l2_ext_control control = {
        .id = id,
        .size = size,
        .ptr = data,
    };
    set_ext_controls(request_fd, std::span(&control, 1));
}

void V4L2M2MDevice::set_ext_controls(int request_fd, std::span<v4l2_ext_control> controls)
{
    struct v4l2_ext_controls meta = {
        .count = static_cast<uint32_t>(controls.size()),
        .controls = controls.data(),
    };

    if (request_fd >= 0) {
        meta.which = V4L2_CTRL_WHICH_REQUEST_VAL;
        meta.request_fd = request_fd;
    }

    errno_wrapper(ioctl, video_fd, VIDIOC_S_EXT_CTRLS, &meta);
}

void V4L2M2MDevice::set_streaming(bool enable)
{
    // Stateful M2M decoders (including Qualcomm Iris) require the output
    // queue to be started before capture. Stop in the reverse order.
    if (enable) {
        stream_output(true);
        stream_capture(true);
    } else {
        stream_capture(false);
        stream_output(false);
    }
}

void V4L2M2MDevice::stream_output(bool enable)
{
    if (enable == output_streaming)
        return;
    auto type = output_buf_type;
    errno_wrapper(ioctl, video_fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
    output_streaming = enable;
}

void V4L2M2MDevice::stream_capture(bool enable)
{
    if (enable == capture_streaming)
        return;
    auto type = capture_buf_type;
    errno_wrapper(ioctl, video_fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
    capture_streaming = enable;
}

void V4L2M2MDevice::reset_capture_queue()
{
    auto type = capture_buf_type;
    errno_wrapper(ioctl, video_fd, VIDIOC_STREAMOFF, &type);
    capture_streaming = false;
    request_buffers(capture_buf_type, 0);
}

void V4L2M2MDevice::subscribe_source_change()
{
    v4l2_event_subscription subscription = {
        .type = V4L2_EVENT_SOURCE_CHANGE,
    };
    errno_wrapper(ioctl, video_fd, VIDIOC_SUBSCRIBE_EVENT, &subscription);
}

void V4L2M2MDevice::decoder_stop()
{
    v4l2_decoder_cmd command = { .cmd = V4L2_DEC_CMD_STOP };
    if (trace_enabled())
        std::fprintf(stderr, "v4l2 decoder STOP\n");
    errno_wrapper(ioctl, video_fd, VIDIOC_DECODER_CMD, &command);
}

void V4L2M2MDevice::decoder_start()
{
    v4l2_decoder_cmd command = { .cmd = V4L2_DEC_CMD_START };
    if (trace_enabled())
        std::fprintf(stderr, "v4l2 decoder START\n");
    errno_wrapper(ioctl, video_fd, VIDIOC_DECODER_CMD, &command);
}

bool V4L2M2MDevice::wait_for_source_change(int timeout_ms)
{
    pollfd pollfd = {
        .fd = video_fd,
        .events = POLLPRI,
    };
    const int result = poll(&pollfd, 1, timeout_ms);
    if (trace_enabled())
        std::fprintf(stderr, "v4l2 source poll timeout=%d result=%d revents=0x%x\n", timeout_ms, result, pollfd.revents);
    if (result <= 0 || !(pollfd.revents & POLLPRI))
        return false;

    v4l2_event event = {};
    while (ioctl(video_fd, VIDIOC_DQEVENT, &event) == 0) {
        if (trace_enabled())
            std::fprintf(stderr, "v4l2 event type=%u changes=0x%x\n", event.type, event.u.src_change.changes);
        if (event.type == V4L2_EVENT_SOURCE_CHANGE
            && (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) {
            return true;
        }
    }
    return false;
}

std::optional<unsigned> V4L2M2MDevice::dequeue_ready(v4l2_buf_type type, int timeout_ms)
{
    last_dequeued_was_last = false;
    last_dequeued_error_ = false;
    last_dequeued_flags_ = 0;
    last_dequeued_timestamp_ = {};
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(timeout_ms, 0));
    for (;;) {
        // Some Iris kernels do not wake POLLIN for a completed CAPTURE buffer
        // on a descriptor that also has writable OUTPUT space. Keep the poll
        // as a short sleep, then try the authoritative non-blocking DQBUF.
        const auto remaining = timeout_ms > 0
            ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()
            : 0;
        if (timeout_ms > 0 && remaining <= 0)
            return std::nullopt;
        const int wait_ms = timeout_ms > 0 ? static_cast<int>(std::min<long long>(remaining, 5)) : 0;
        pollfd pollfd = {
            .fd = video_fd,
            .events = static_cast<short>(V4L2_TYPE_IS_CAPTURE(type) ? (POLLIN | POLLPRI) : POLLOUT),
        };
        const int result = poll(&pollfd, 1, wait_ms);
        if (trace_enabled() && result > 0)
            std::fprintf(stderr, "v4l2 poll type=%u timeout=%d result=%d revents=0x%x\n", type, wait_ms, result,
                pollfd.revents);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::generic_category());
        }

        v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        const auto& queue_buffers = V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers;
        const v4l2_memory memory = queue_buffers.empty() ? V4L2_MEMORY_MMAP : queue_buffers.front().memory();
        v4l2_buffer buffer = {
            .type = type,
            .memory = memory,
            .m = { .planes = planes },
            .length = VIDEO_MAX_PLANES,
        };
        if (ioctl(video_fd, VIDIOC_DQBUF, &buffer) == 0) {
            last_dequeued_error_ = (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0;
            last_dequeued_flags_ = buffer.flags;
            last_dequeued_timestamp_ = buffer.timestamp;
            last_dequeued_was_last = (buffer.flags & V4L2_BUF_FLAG_LAST) != 0;
            if (last_dequeued_error_ && trace_enabled())
                std::fprintf(stderr, "v4l2 dq ERROR type=%u index=%u flags=0x%x last=%d ts=%lld.%06ld seq=%u\n", type,
                    buffer.index, buffer.flags, last_dequeued_was_last,
                    static_cast<long long>(buffer.timestamp.tv_sec), static_cast<long>(buffer.timestamp.tv_usec),
                    buffer.sequence);
            if (trace_enabled())
                std::fprintf(stderr, "v4l2 dq type=%u index=%u flags=0x%x last=%d ts=%lld.%06ld seq=%u\n", type,
                    buffer.index, buffer.flags, last_dequeued_was_last,
                    static_cast<long long>(buffer.timestamp.tv_sec), static_cast<long>(buffer.timestamp.tv_usec),
                    buffer.sequence);
            return buffer.index;
        }
        if (trace_enabled() && errno != EAGAIN)
            std::fprintf(stderr, "v4l2 dq type=%u errno=%d\n", type, errno);
        if (errno == EAGAIN) {
            if (timeout_ms <= 0)
                return std::nullopt;
            continue;
        }
        throw std::system_error(errno, std::generic_category());
    }
}
