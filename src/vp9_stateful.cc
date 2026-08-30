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
    // Fixed-resolution Profile 0 decodes correctly, but repeated VP9 source
    // changes and VA context recreation reproducibly reboot the qualified
    // Iris kernel/firmware. Do not advertise a browser-visible capability
    // that can take down the whole device; keep the implementation dormant
    // for firmware diagnostics until that platform defect is fixed.
    (void)device;
    return {};
}
