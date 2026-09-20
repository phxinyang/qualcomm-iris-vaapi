// SPDX-License-Identifier: MIT
//
// AV1: VA supplies a parsed frame header plus bare tile payloads; the
// stateful node wants complete OBU temporal units. av1_obu.cc rebuilds
// them. One field the header needs is not in VA at all: refresh_frame_flags.
// It is recovered from the *next* picture's reference map (a slot that then
// holds this frame's surface is a slot this frame wrote), so every picture
// is held back until its successor arrives or the stream is flushed.

#include "translator.h"

#include "../util/error.h"
#include "av1_obu.h"

#include <cstring>

namespace iris::codec {

namespace {

class AV1 final : public Translator {
public:
    explicit AV1(VAProfile profile)
    {
        require(profile == VAProfileAV1Profile0, "AV1 profile not supported", VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    }
    uint32_t v4l2_pixelformat() const override { return v4l2_fourcc('A', 'V', '0', '1'); }

    void translate(const Picture& picture, AccessUnit& out) override
    {
        require(picture.parameters, "AV1 picture without parameters", VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& parameters = *static_cast<const VADecPictureParameterBufferAV1*>(picture.parameters);

        // Sequence state and the previous picture's refresh flags come from
        // this picture's view of the world.
        const bool key_frame = parameters.pic_info_fields.bits.frame_type == 0;
        if (!seen_sequence_ || key_frame) {
            sequence_ = av1_sequence_state(parameters);
            seen_sequence_ = true;
            emit_sequence_header_ = true;
        }
        uint8_t previous_refresh = 0;
        if (held_) {
            for (unsigned slot = 0; slot < 8; ++slot)
                if (parameters.ref_frame_map[slot] == held_picture_.current_frame
                    && held_picture_.current_frame != VA_INVALID_SURFACE)
                    previous_refresh |= static_cast<uint8_t>(1u << slot);
        }

        // Release the held picture into `out`...
        out.bytes.clear();
        out.deferred = false;
        out.sequence_start = false;
        if (held_) {
            build(previous_refresh, out);
        } else {
            out.deferred = true;
        }

        // ...and hold this one.
        held_ = true;
        held_surface_ = picture.surface_id;
        held_picture_ = parameters;
        held_tiles_.clear();
        for (const void* raw : picture.slice_parameters) {
            const auto* slice = static_cast<const VASliceParameterBufferAV1*>(raw);
            held_tiles_.push_back({ slice->slice_data_offset, slice->slice_data_size });
        }
        held_data_.clear();
        for (const auto& [data, size] : picture.slice_data)
            held_data_.insert(held_data_.end(), data, data + size);
        held_emits_sequence_header_ = emit_sequence_header_;
        emit_sequence_header_ = false;
        held_sequence_start_ = key_frame;
    }

    bool flush(AccessUnit& out) override
    {
        if (!held_)
            return false;
        out.bytes.clear();
        out.deferred = false;
        // Nothing follows: no slot needs to survive this frame.
        build(0, out);
        return true;
    }

    uint32_t held_surface() const override { return held_ ? held_surface_ : VA_INVALID_ID; }

private:
    void build(uint8_t refresh_frame_flags, AccessUnit& out)
    {
        const char* reject = nullptr;
        const bool built = av1_build_temporal_unit(out.bytes, held_picture_, sequence_, references_,
            held_emits_sequence_header_, refresh_frame_flags, held_data_.data(), held_tiles_, &reject);
        held_ = false;
        require(built, reject ? reject : "AV1 temporal unit cannot be rebuilt", VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
        for (unsigned slot = 0; slot < 8; ++slot)
            if (refresh_frame_flags & (1u << slot))
                references_.order_hint[slot] = held_picture_.order_hint;
        av1_update_reference_gm(references_, held_picture_, refresh_frame_flags);
        out.sequence_start = held_sequence_start_;
        out.released_surface = held_surface_;
    }

    AV1SequenceState sequence_;
    AV1ReferenceState references_;
    bool seen_sequence_ = false;
    bool emit_sequence_header_ = false;
    bool held_ = false;
    uint32_t held_surface_ = VA_INVALID_ID;
    VADecPictureParameterBufferAV1 held_picture_ {};
    std::vector<AV1TileEntry> held_tiles_;
    std::vector<uint8_t> held_data_;
    bool held_emits_sequence_header_ = false;
    bool held_sequence_start_ = false;
};

} // namespace

std::unique_ptr<Translator> make_av1(VAProfile profile) { return std::make_unique<AV1>(profile); }

} // namespace iris::codec
