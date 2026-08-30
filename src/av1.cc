/* Qualcomm Iris stateful AV1 VA-API context. */

#include "av1.h"

#include "trace.h"

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

VAStatus AV1Context::store_buffer(const Buffer& buffer) const
{
    auto& surface = driver_data->surfaces.at(current_surface());
    switch (buffer.type) {
    case VAPictureParameterBufferType:
        surface.params.av1.picture = reinterpret_cast<VADecPictureParameterBufferAV1*>(buffer.data.get());
        if (trace_enabled()) {
            const auto* picture = surface.params.av1.picture;
            std::fprintf(stderr,
                "av1 params profile=%u bitdepth=%u size=%ux%u frame_type=%u tiles=%ux%u current=%u\n",
                picture->profile, picture->bit_depth_idx, picture->frame_width_minus1 + 1,
                picture->frame_height_minus1 + 1, picture->pic_info_fields.bits.frame_type,
                picture->tile_cols, picture->tile_rows, picture->current_frame);
            // VA does not carry refresh_frame_flags, which an OBU frame header
            // must contain for every non-key frame. The reference map is the
            // only signal that can recover it: a slot that a later frame maps
            // to this frame's surface is a slot this frame refreshed. Trace
            // the raw evidence so the derivation can be checked offline
            // against a real stream before it is implemented.
            std::fprintf(stderr, "av1 refmap current=%u order_hint=%u primary_ref=%u show=%u map=",
                picture->current_frame, picture->order_hint, picture->primary_ref_frame,
                picture->pic_info_fields.bits.show_frame);
            for (unsigned i = 0; i < 8; ++i)
                std::fprintf(stderr, "%s%u", i ? "," : "", picture->ref_frame_map[i]);
            std::fprintf(stderr, " idx=");
            for (unsigned i = 0; i < 7; ++i)
                std::fprintf(stderr, "%s%u", i ? "," : "", picture->ref_frame_idx[i]);
            std::fprintf(stderr, "\n");
        }
        observe_picture(*surface.params.av1.picture);
        return VA_STATUS_SUCCESS;

    case VASliceParameterBufferType: {
        const auto* slice = reinterpret_cast<VASliceParameterBufferAV1*>(buffer.data.get());
        surface.params.av1.slice = const_cast<VASliceParameterBufferAV1*>(slice);
        if (trace_enabled()) {
            std::fprintf(stderr, "av1 tile size=%u off=%u row=%u col=%u flag=0x%x\n",
                slice->slice_data_size, slice->slice_data_offset, slice->tile_row,
                slice->tile_column, slice->slice_data_flag);
        }
        // One entry per tile. The offsets index the slice data buffer, which
        // arrives separately and may carry every tile at once.
        tiles_.push_back({ slice->slice_data_offset, slice->slice_data_size });
        return VA_STATUS_SUCCESS;
    }

    case VASliceDataBufferType: {
        const size_t bytes = static_cast<size_t>(buffer.size) * buffer.count;
        if (bytes == 0)
            return VA_STATUS_SUCCESS;
        if (surface.source_size_used > std::numeric_limits<size_t>::max() - bytes
            || !ensure_stateful_bitstream_capacity(surface, surface.source_size_used + bytes))
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        const auto* source = buffer.data.get();
        if (trace_enabled()) {
            std::fprintf(stderr, "av1 data bytes=%zu first=", bytes);
            for (size_t i = 0; i < std::min<size_t>(8, bytes); ++i)
                std::fprintf(stderr, "%02x", source[i]);
            std::fprintf(stderr, "\n");
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

void AV1Context::observe_picture(const VADecPictureParameterBufferAV1& picture) const
{
    const bool key_frame = picture.pic_info_fields.bits.frame_type == 0;
    if (!seen_sequence_ || key_frame) {
        sequence_ = av1_sequence_state(picture);
        seen_sequence_ = true;
        emit_sequence_header_ = true;
    }

    // Recover the previous frame's refresh_frame_flags. VA reports the
    // reference map as it stands before this frame is decoded, so any slot now
    // holding the previous frame's surface is a slot that frame wrote.
    if (deferred_surface_ != VA_INVALID_SURFACE) {
        uint8_t refresh = 0;
        for (unsigned slot = 0; slot < 8; ++slot) {
            if (picture.ref_frame_map[slot] == deferred_picture_.current_frame
                && deferred_picture_.current_frame != VA_INVALID_SURFACE)
                refresh |= static_cast<uint8_t>(1u << slot);
        }
        pending_refresh_ = refresh;
        pending_refresh_known_ = true;
    }
}

VAStatus AV1Context::stateful_submit_picture(VASurfaceID surface_id)
{
    // Release the frame held from the previous call now that its successor's
    // reference map has told us which slots it refreshed.
    if (deferred_surface_ != VA_INVALID_SURFACE) {
        const VAStatus status = release_deferred(pending_refresh_known_ ? pending_refresh_ : 0);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }

    if (!driver_data->surfaces.contains(surface_id))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto& surface = driver_data->surfaces.at(surface_id);
    if (!surface.params.av1.picture) {
        if (trace_enabled())
            std::fprintf(stderr, "av1 submit surface=%u without picture parameters\n", surface_id);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    deferred_surface_ = surface_id;
    deferred_picture_ = *surface.params.av1.picture;
    deferred_tiles_ = tiles_;
    deferred_emits_sequence_header_ = emit_sequence_header_;
    emit_sequence_header_ = false;
    pending_refresh_ = 0;
    pending_refresh_known_ = false;
    tiles_.clear();
    return VA_STATUS_SUCCESS;
}

VAStatus AV1Context::stateful_flush_deferred()
{
    if (deferred_surface_ == VA_INVALID_SURFACE)
        return VA_STATUS_SUCCESS;
    // Nothing follows this frame, so no reference slot needs to survive it.
    return release_deferred(pending_refresh_known_ ? pending_refresh_ : 0);
}

VAStatus AV1Context::release_deferred(uint8_t refresh_frame_flags)
{
    const VASurfaceID surface_id = deferred_surface_;
    deferred_surface_ = VA_INVALID_SURFACE;
    pending_refresh_known_ = false;
    if (!driver_data->surfaces.contains(surface_id))
        return VA_STATUS_SUCCESS;
    auto& surface = driver_data->surfaces.at(surface_id);

    std::vector<uint8_t> unit;
    const char* reject = nullptr;
    const bool built = av1_build_temporal_unit(unit, deferred_picture_, sequence_, references_,
        deferred_emits_sequence_header_, refresh_frame_flags, surface.stateful_bitstream.data(),
        deferred_tiles_, &reject);
    if (!built) {
        if (trace_enabled())
            std::fprintf(stderr, "av1 cannot rebuild temporal unit for surface=%u: %s\n",
                surface_id, reject ? reject : "unknown");
        surface.source_size_used = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    if (!ensure_stateful_bitstream_capacity(surface, unit.size()))
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    std::memcpy(surface.stateful_bitstream.data(), unit.data(), unit.size());
    surface.source_size_used = static_cast<unsigned>(unit.size());

    // Reference slots this frame wrote now hold its order hint, which the next
    // frame's skip-mode signalling depends on.
    for (unsigned slot = 0; slot < 8; ++slot) {
        if (refresh_frame_flags & (1u << slot))
            references_.order_hint[slot] = deferred_picture_.order_hint;
    }

    if (trace_enabled())
        std::fprintf(stderr, "av1 rebuilt surface=%u bytes=%zu refresh=0x%02x tiles=%zu seq=%d\n",
            surface_id, unit.size(), refresh_frame_flags, deferred_tiles_.size(),
            deferred_emits_sequence_header_);

    // The rebuilt stream is the only artifact worth checking independently:
    // appending every temporal unit produces a low-overhead OBU file that any
    // conformant AV1 decoder can verify against the original, which is how
    // this translator is validated without involving the hardware at all.
    if (const char* dump_dir = std::getenv("V4L2_VA_DUMP_BATCH")) {
        char path[256];
        std::snprintf(path, sizeof(path), "%s/av1-rebuilt.obu", dump_dir);
        if (FILE* file = std::fopen(path, "ab")) {
            std::fwrite(unit.data(), 1, unit.size(), file);
            std::fclose(file);
        }
    }
    return append_stateful_picture(surface_id);
}
