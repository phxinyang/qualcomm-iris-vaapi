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

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
}

#define SOURCE_SIZE_MAX (1024 * 1024)

using fourcc = uint32_t;

class V4L2M2MDevice {
private:
    std::string video_path_;
    std::optional<std::string> media_path_;

public:
    class Buffer {
    public:
        void queue(int request_fd = -1, timeval* timestamp = nullptr, unsigned size = 0) const;
        unsigned dequeue() const;
        std::vector<int> export_(unsigned flags) const;
        std::vector<std::span<uint8_t>> mapping() const { return mapping_; }
        unsigned index() const { return index_; }
        V4L2M2MDevice& owner() const { return owner_; }

        Buffer(V4L2M2MDevice& owner, v4l2_buf_type type, unsigned index);
        Buffer(Buffer&& other);
        Buffer& operator=(Buffer&& other);
        ~Buffer();

    private:
        V4L2M2MDevice& owner_;
        v4l2_buf_type type_;
        unsigned index_;
        // QUERYBUF flags are part of the MMAP queue contract. Qualcomm's
        // stateful decoder requires these flags on compressed OUTPUT QBUF.
        uint32_t query_flags_;
        std::vector<std::span<uint8_t>> mapping_;

        friend class V4L2M2MDevice;
    };

    static std::vector<std::pair<std::string, std::optional<std::string>>> enumerate_devices();

    static const uint32_t required_capabilities = V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE;

    V4L2M2MDevice(const std::string& video_path, const std::optional<std::string>& media_path);
    V4L2M2MDevice(V4L2M2MDevice&& other);
    V4L2M2MDevice& operator=(V4L2M2MDevice&& other);
    ~V4L2M2MDevice();
    // Open an independent V4L2 fd for a new VA context while retaining the
    // same capability probe and device selection.
    V4L2M2MDevice clone_for_context() const;
    void set_format(enum v4l2_buf_type type, unsigned int pixelformat, unsigned int width, unsigned int height);
    unsigned request_buffers(enum v4l2_buf_type type, unsigned count);
    bool format_supported(v4l2_buf_type type, unsigned pixelformat) const;
    unsigned buffer_count(v4l2_buf_type type) const;
    const Buffer& buffer(v4l2_buf_type type, unsigned index);
    int32_t get_control(uint32_t id) const;
    void set_ext_control(int request_fd, unsigned id, void* data, unsigned size);
    void set_ext_controls(int request_fd, std::span<v4l2_ext_control> controls);
    void set_streaming(bool enable);
    void stream_output(bool enable);
    void stream_capture(bool enable);
    void reset_capture_queue();
    void subscribe_source_change();
    void decoder_stop();
    void decoder_start();
    bool wait_for_source_change(int timeout_ms = 2000);
    std::optional<unsigned> dequeue_ready(v4l2_buf_type type, int timeout_ms = 0);
    bool last_dequeued_last() const { return last_dequeued_was_last; }
    bool last_dequeued_error() const { return last_dequeued_error_; }
    uint32_t last_dequeued_flags() const { return last_dequeued_flags_; }
    const timeval& last_dequeued_timestamp() const { return last_dequeued_timestamp_; }

    int video_fd;
    int media_fd;
    const uint32_t capabilities;
    const v4l2_buf_type capture_buf_type;
    const v4l2_buf_type output_buf_type;
    v4l2_format capture_format = {};
    v4l2_format output_format = {};
    std::set<fourcc> supported_output_formats;
    std::set<fourcc> supported_capture_formats;

private:
    std::vector<Buffer> capture_buffers;
    std::vector<Buffer> output_buffers;

public:
    bool output_streaming = false;
    bool capture_streaming = false;
    bool last_dequeued_was_last = false;
    bool last_dequeued_error_ = false;
    uint32_t last_dequeued_flags_ = 0;
    timeval last_dequeued_timestamp_ = {};

};
