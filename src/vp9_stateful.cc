/*
 * Stateful VP9 support for Qualcomm Iris V4L2 M2M decoders.
 */

#include "vp9_stateful.h"

#include <cstring>
#include <limits>

extern "C" {
#include <va/va_dec_vp9.h>
}

#include "buffer.h"
#include "driver.h"
#include "surface.h"
#include "v4l2.h"

VAStatus VP9StatefulContext::store_buffer(const Buffer& buffer) const
{
    auto& surface = driver_data->surfaces.at(current_surface());
    switch (buffer.type) {
    case VAPictureParameterBufferType:
        surface.params.vp9.picture = reinterpret_cast<VADecPictureParameterBufferVP9*>(buffer.data.get());
        return VA_STATUS_SUCCESS;

    case VASliceParameterBufferType:
        surface.params.vp9.slice = reinterpret_cast<VASliceParameterBufferVP9*>(buffer.data.get());
        return VA_STATUS_SUCCESS;

    case VASliceDataBufferType: {
        const size_t bytes = static_cast<size_t>(buffer.size) * buffer.count;
        if (surface.source_size_used > std::numeric_limits<size_t>::max() - bytes
            || !ensure_stateful_bitstream_capacity(surface, surface.source_size_used + bytes))
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        auto destination = std::span<uint8_t>(surface.stateful_bitstream);
        std::memcpy(destination.data() + surface.source_size_used, buffer.data.get(), bytes);
        surface.source_size_used += bytes;
        return VA_STATUS_SUCCESS;
    }

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
}

std::set<VAProfile> VP9StatefulContext::supported_profiles(const V4L2M2MDevice& device)
{
    if (!device.format_supported(device.output_buf_type, V4L2_PIX_FMT_VP9))
        return {};

    // This backend currently publishes only an 8-bit 4:2:0 VA surface
    // contract.  Profiles 1/2/3 require chroma layouts and/or bit depths that
    // cannot be represented by NV12, even if the compressed decoder menu
    // lists them.
    const bool has_8bit_420_surface = device.format_supported(device.capture_buf_type, V4L2_PIX_FMT_NV12)
        || device.format_supported(device.capture_buf_type, V4L2_PIX_FMT_NV12M);
    if (!has_8bit_420_surface)
        return {};

    const auto menu = device.menu_control_values(V4L2_CID_MPEG_VIDEO_VP9_PROFILE);
    if (!menu || !menu->contains(V4L2_MPEG_VIDEO_VP9_PROFILE_0))
        return {};
    return { VAProfileVP9Profile0 };
}
