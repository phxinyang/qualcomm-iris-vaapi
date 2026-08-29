/* Qualcomm Iris stateful AV1 VA-API context. */

#include "av1.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" {
#include <va/va_dec_av1.h>
}

#include "buffer.h"
#include "driver.h"
#include "surface.h"
#include "v4l2.h"

namespace {

bool has_obu_header(const uint8_t* data, size_t size)
{
    // This is a diagnostic guard only; Iris remains responsible for parsing.
    return size >= 1 && (data[0] & 0x80) == 0 && ((data[0] >> 3) & 0x0f) != 0;
}

} // namespace

VAStatus AV1Context::store_buffer(const Buffer& buffer) const
{
    auto& surface = driver_data->surfaces.at(current_surface());
    switch (buffer.type) {
    case VAPictureParameterBufferType:
        surface.params.av1.picture = reinterpret_cast<VADecPictureParameterBufferAV1*>(buffer.data.get());
        if (std::getenv("V4L2_VA_TRACE")) {
            const auto* picture = surface.params.av1.picture;
            std::fprintf(stderr,
                "av1 params profile=%u bitdepth=%u size=%ux%u frame_type=%u tiles=%ux%u current=%u\n",
                picture->profile, picture->bit_depth_idx, picture->frame_width_minus1 + 1,
                picture->frame_height_minus1 + 1, picture->pic_info_fields.bits.frame_type,
                picture->tile_cols, picture->tile_rows, picture->current_frame);
        }
        return VA_STATUS_SUCCESS;

    case VASliceParameterBufferType:
        surface.params.av1.slice = reinterpret_cast<VASliceParameterBufferAV1*>(buffer.data.get());
        if (std::getenv("V4L2_VA_TRACE")) {
            const auto* slice = surface.params.av1.slice;
            std::fprintf(stderr, "av1 tile size=%u off=%u row=%u col=%u flag=0x%x\n",
                slice->slice_data_size, slice->slice_data_offset, slice->tile_row,
                slice->tile_column, slice->slice_data_flag);
        }
        return VA_STATUS_SUCCESS;

    case VASliceDataBufferType: {
        const size_t bytes = static_cast<size_t>(buffer.size) * buffer.count;
        if (bytes == 0)
            return VA_STATUS_SUCCESS;
        if (surface.source_size_used > std::numeric_limits<size_t>::max() - bytes
            || !ensure_stateful_bitstream_capacity(surface, surface.source_size_used + bytes))
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        const auto* source = buffer.data.get();
        if (surface.source_size_used == 0 && !has_obu_header(source, bytes)) {
            if (std::getenv("V4L2_VA_TRACE"))
                std::fprintf(stderr, "av1 tile payload has no OBU header; stateful Iris needs a temporal unit\n");
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (std::getenv("V4L2_VA_TRACE")) {
            std::fprintf(stderr, "av1 data bytes=%zu first=", bytes);
            for (size_t i = 0; i < std::min<size_t>(8, bytes); ++i)
                std::fprintf(stderr, "%02x", source[i]);
            std::fprintf(stderr, " obu=%d\n", has_obu_header(source, bytes));
        }
        auto destination = std::span<uint8_t>(surface.stateful_bitstream);
        std::memcpy(destination.data() + surface.source_size_used, source, bytes);
        surface.source_size_used += bytes;
        return VA_STATUS_SUCCESS;
    }

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
}

unsigned AV1Context::stateful_frame_type(VASurfaceID surface_id) const
{
    const auto& surface = driver_data->surfaces.at(surface_id);
    if (!surface.params.av1.picture)
        return 255;
    return surface.params.av1.picture->pic_info_fields.bits.frame_type;
}

bool AV1Context::stateful_sequence_start(VASurfaceID surface_id)
{
    const auto& surface = driver_data->surfaces.at(surface_id);
    return surface.params.av1.picture
        && surface.params.av1.picture->pic_info_fields.bits.frame_type == 0;
}

std::set<VAProfile> AV1Context::supported_profiles(const V4L2M2MDevice& device)
{
    // VA-API exposes AV1 tile payloads without the sequence/frame OBU headers,
    // while Iris' stateful node requires a complete temporal unit. Keep this
    // translator opt-in until a caller-facing OBU-preserving contract exists.
    if (!std::getenv("V4L2_VA_ENABLE_AV1_STATEFUL"))
        return {};
    if (!device.format_supported(device.output_buf_type, V4L2_PIX_FMT_AV1))
        return {};
    // VA-API's AV1 profile 0 is the 8-bit 4:2:0 path supported by this
    // backend. Do not advertise profile 1/2 without matching surface formats.
    return { VAProfileAV1Profile0 };
}
