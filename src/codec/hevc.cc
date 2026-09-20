// SPDX-License-Identifier: MIT
//
// HEVC: VPS/SPS/PPS synthesised from VA picture parameters on every access
// unit. The short-term RPS is rebuilt from the current DPB flags and repeated
// for every index so the slice header's index stays valid unchanged.

#include "translator.h"

#include "../util/error.h"
#include "bitwriter.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace iris::codec {

namespace {

bool is_main10(VAProfile profile) { return profile == VAProfileHEVCMain10; }

void profile_tier_level(BitWriter& writer, VAProfile profile, unsigned max_sub_layers_minus1)
{
    writer.bits(0, 2); // general_profile_space
    writer.bit(0); // general_tier_flag
    writer.bits(is_main10(profile) ? 2 : 1, 5); // general_profile_idc
    // Compatibility: Main (bit 1) and Main 10 (bit 2), MSB-first in the
    // 32-bit flag word.
    writer.bits(is_main10(profile) ? 0x20000000u : 0x60000000u, 32);
    writer.bit(1); // progressive_source
    writer.bit(0); // interlaced_source
    writer.bit(0); // non_packed_constraint
    writer.bit(1); // frame_only_constraint
    writer.bits(0, 44); // reserved
    writer.bits(186, 8); // level 6.2: above every stream the firmware accepts
    for (unsigned i = 0; i < max_sub_layers_minus1; ++i) {
        writer.bit(0);
        writer.bit(0);
    }
    for (unsigned i = max_sub_layers_minus1; i < 8; ++i)
        writer.bits(0, 2);
}

void scaling_lists(BitWriter& writer, const VAIQMatrixBufferHEVC& matrix)
{
    for (unsigned size_id = 0; size_id < 4; ++size_id) {
        const unsigned count = size_id == 3 ? 2 : 6;
        const unsigned side = size_id == 0 ? 4 : 8;
        for (unsigned id = 0; id < count; ++id) {
            const uint8_t* values = size_id == 0 ? matrix.ScalingList4x4[id]
                : size_id == 1                   ? matrix.ScalingList8x8[id]
                : size_id == 2                   ? matrix.ScalingList16x16[id]
                                                 : matrix.ScalingList32x32[id];
            writer.bit(1); // scaling_list_pred_mode_flag: explicit
            int previous = 8;
            if (size_id > 1) {
                previous = size_id == 2 ? matrix.ScalingListDC16x16[id] : matrix.ScalingListDC32x32[id];
                writer.se(previous - 8);
            }
            // VA is raster ordered; the bitstream uses the up-right diagonal scan.
            for (unsigned diagonal = 0; diagonal < 2 * side - 1; ++diagonal) {
                for (int y = static_cast<int>(std::min(diagonal, side - 1)), x = static_cast<int>(diagonal) - y;
                     y >= 0 && x < static_cast<int>(side); --y, ++x) {
                    const int value = values[y * side + x];
                    int delta = (value - previous + 256) % 256;
                    if (delta > 127)
                        delta -= 256;
                    writer.se(delta);
                    previous = value;
                }
            }
        }
    }
}

bool has_start_code(const uint8_t* data, size_t size)
{
    return size >= 3 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
}

class HEVC final : public Translator {
public:
    explicit HEVC(VAProfile profile)
        : profile_(profile)
    {
        require(profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10, "HEVC profile not supported",
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    }
    uint32_t v4l2_pixelformat() const override { return v4l2_fourcc('H', 'E', 'V', 'C'); }

    void translate(const Picture& picture, AccessUnit& out) override
    {
        require(picture.parameters && !picture.slice_data.empty(), "HEVC picture without parameters or slices",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& parameters = *static_cast<const VAPictureParameterBufferHEVC*>(picture.parameters);
        // A stream that uses long-term references cannot be described by the
        // generated SPS; refuse rather than decode with wrong references.
        for (const auto& reference : parameters.ReferenceFrames) {
            if (reference.flags & VA_PICTURE_HEVC_INVALID)
                continue;
            require(!(reference.flags & (VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR)),
                "HEVC long-term references are not supported by the generated parameter sets",
                VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        }
        require(parameters.num_long_term_ref_pic_sps == 0, "HEVC lt_ref_pic_sps unsupported",
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        const auto* matrix = static_cast<const VAIQMatrixBufferHEVC*>(picture.iq_matrix);
        if (matrix)
            scaling_ = *matrix; // VA may free the buffer after this picture
        const VAIQMatrixBufferHEVC* effective
            = parameters.pic_fields.bits.scaling_list_enabled_flag && scaling_ ? &*scaling_ : nullptr;

        out.bytes.clear();
        out.deferred = false;
        for (const auto& nal : { hevc_vps(profile_, parameters), hevc_sps(profile_, parameters, effective),
                 hevc_pps(parameters) })
            out.bytes.insert(out.bytes.end(), nal.begin(), nal.end());
        for (const auto& [data, size] : picture.slice_data) {
            if (size == 0)
                continue;
            // Do not treat 00 02 as a prefix: it is a valid two-byte NAL header.
            if (!has_start_code(data, size))
                out.bytes.insert(out.bytes.end(), { 0, 0, 1 });
            out.bytes.insert(out.bytes.end(), data, data + size);
        }
        // IRAP: NoPicReordering is not a start marker; use the slice NAL type
        // (16..23) of the first slice.
        out.sequence_start = false;
        if (!picture.slice_data.empty()) {
            const auto [data, size] = picture.slice_data.front();
            const size_t header = has_start_code(data, size) ? (data[2] == 1 ? 3 : 4) : 0;
            if (size > header) {
                const unsigned nal_type = (data[header] >> 1) & 0x3f;
                out.sequence_start = nal_type >= 16 && nal_type <= 23;
            }
        }
    }

    bool flush(AccessUnit&) override { return false; }

private:
    VAProfile profile_;
    std::optional<VAIQMatrixBufferHEVC> scaling_;
};

} // namespace

std::vector<uint8_t> hevc_vps(VAProfile profile, const VAPictureParameterBufferHEVC& picture)
{
    BitWriter writer;
    writer.bits(0, 4); // vps_video_parameter_set_id
    writer.bit(1); // vps_base_layer_internal_flag
    writer.bit(1); // vps_base_layer_available_flag
    writer.bits(0, 6); // vps_max_layers_minus1
    writer.bits(1, 3); // vps_max_sub_layers_minus1
    writer.bit(0); // vps_temporal_id_nesting_flag
    writer.bits(0xffff, 16);
    profile_tier_level(writer, profile, 1);
    writer.bit(1); // vps_sub_layer_ordering_info_present_flag
    for (unsigned i = 0; i < 2; ++i) {
        writer.ue(std::min<unsigned>(31, picture.sps_max_dec_pic_buffering_minus1));
        writer.ue(picture.pic_fields.bits.NoPicReorderingFlag ? 0 : (i == 0 ? 3 : 4));
        writer.ue(0);
    }
    writer.bits(0, 6); // vps_max_layer_id
    writer.ue(0); // vps_num_layer_sets_minus1
    writer.bit(0); // vps_timing_info_present_flag
    writer.bit(0); // vps_extension_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 32 << 1, 1 }, writer);
    return result;
}

std::vector<uint8_t> hevc_sps(VAProfile profile, const VAPictureParameterBufferHEVC& picture,
    const VAIQMatrixBufferHEVC* scaling)
{
    BitWriter writer;
    writer.bits(0, 4); // sps_video_parameter_set_id
    writer.bits(1, 3); // sps_max_sub_layers_minus1
    writer.bit(1); // sps_temporal_id_nesting_flag
    profile_tier_level(writer, profile, 1);
    writer.ue(0); // sps_seq_parameter_set_id
    writer.ue(picture.pic_fields.bits.chroma_format_idc);
    if (picture.pic_fields.bits.chroma_format_idc == 3)
        writer.bit(picture.pic_fields.bits.separate_colour_plane_flag);
    writer.ue(picture.pic_width_in_luma_samples);
    writer.ue(picture.pic_height_in_luma_samples);
    writer.bit(0); // conformance_window_flag: VA reports the coded size; the
                   // visible crop comes from the decoder's COMPOSE rectangle
    writer.ue(picture.bit_depth_luma_minus8);
    writer.ue(picture.bit_depth_chroma_minus8);
    writer.ue(picture.log2_max_pic_order_cnt_lsb_minus4);
    writer.bit(1); // sps_sub_layer_ordering_info_present_flag
    for (unsigned i = 0; i < 2; ++i) {
        writer.ue(std::min<unsigned>(31, picture.sps_max_dec_pic_buffering_minus1));
        writer.ue(picture.pic_fields.bits.NoPicReorderingFlag ? 0 : (i == 0 ? 3 : 4));
        writer.ue(0);
    }
    writer.ue(picture.log2_min_luma_coding_block_size_minus3);
    writer.ue(picture.log2_diff_max_min_luma_coding_block_size);
    writer.ue(picture.log2_min_transform_block_size_minus2);
    writer.ue(picture.log2_diff_max_min_transform_block_size);
    // Bitstream order is inter then intra; the VA struct has them swapped.
    writer.ue(picture.max_transform_hierarchy_depth_inter);
    writer.ue(picture.max_transform_hierarchy_depth_intra);
    writer.bit(picture.pic_fields.bits.scaling_list_enabled_flag);
    if (picture.pic_fields.bits.scaling_list_enabled_flag) {
        writer.bit(scaling != nullptr);
        if (scaling)
            scaling_lists(writer, *scaling);
    }
    writer.bit(picture.pic_fields.bits.amp_enabled_flag);
    writer.bit(picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
    writer.bit(picture.pic_fields.bits.pcm_enabled_flag);
    if (picture.pic_fields.bits.pcm_enabled_flag) {
        writer.bits(picture.pcm_sample_bit_depth_luma_minus1, 4);
        writer.bits(picture.pcm_sample_bit_depth_chroma_minus1, 4);
        writer.ue(picture.log2_min_pcm_luma_coding_block_size_minus3);
        writer.ue(picture.log2_diff_max_min_pcm_luma_coding_block_size);
        writer.bit(picture.pic_fields.bits.pcm_loop_filter_disabled_flag);
    }

    // Rebuild one explicit short-term RPS from the DPB flags and repeat it
    // for every set the stream declared, so the slice's index is valid.
    struct Entry {
        int32_t poc;
    };
    std::vector<Entry> negative, positive;
    for (const auto& reference : picture.ReferenceFrames) {
        if (reference.flags & VA_PICTURE_HEVC_INVALID)
            continue;
        if (reference.flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE)
            negative.push_back({ reference.pic_order_cnt });
        else if (reference.flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)
            positive.push_back({ reference.pic_order_cnt });
    }
    std::sort(negative.begin(), negative.end(), [](auto& a, auto& b) { return a.poc > b.poc; });
    std::sort(positive.begin(), positive.end(), [](auto& a, auto& b) { return a.poc < b.poc; });
    const unsigned rps_count = std::min<unsigned>(64, picture.num_short_term_ref_pic_sets);
    writer.ue(rps_count);
    for (unsigned i = 0; i < rps_count; ++i) {
        if (i != 0)
            writer.bit(0); // inter_ref_pic_set_prediction_flag
        writer.ue(std::min<size_t>(16, negative.size()));
        writer.ue(std::min<size_t>(16, positive.size()));
        int32_t previous = picture.CurrPic.pic_order_cnt;
        for (size_t j = 0; j < negative.size() && j < 16; ++j) {
            const int32_t delta = previous - negative[j].poc;
            writer.ue(delta > 0 ? static_cast<uint32_t>(delta - 1) : 0);
            writer.bit(1);
            previous = negative[j].poc;
        }
        previous = picture.CurrPic.pic_order_cnt;
        for (size_t j = 0; j < positive.size() && j < 16; ++j) {
            const int32_t delta = positive[j].poc - previous;
            writer.ue(delta > 0 ? static_cast<uint32_t>(delta - 1) : 0);
            writer.bit(1);
            previous = positive[j].poc;
        }
    }
    writer.bit(0); // long_term_ref_pics_present_flag (rejected in translate)
    writer.bit(picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag);
    writer.bit(picture.pic_fields.bits.strong_intra_smoothing_enabled_flag);
    writer.bit(0); // vui_parameters_present_flag
    writer.bit(0); // sps_extension_present_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 33 << 1, 1 }, writer);
    return result;
}

std::vector<uint8_t> hevc_pps(const VAPictureParameterBufferHEVC& picture)
{
    BitWriter writer;
    writer.ue(0); // pps_pic_parameter_set_id
    writer.ue(0); // pps_seq_parameter_set_id
    writer.bit(picture.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag);
    writer.bit(picture.slice_parsing_fields.bits.output_flag_present_flag);
    writer.bits(picture.num_extra_slice_header_bits, 3);
    writer.bit(picture.pic_fields.bits.sign_data_hiding_enabled_flag);
    writer.bit(picture.slice_parsing_fields.bits.cabac_init_present_flag);
    writer.ue(std::min<unsigned>(14, picture.num_ref_idx_l0_default_active_minus1));
    writer.ue(std::min<unsigned>(14, picture.num_ref_idx_l1_default_active_minus1));
    writer.se(std::clamp<int>(picture.init_qp_minus26, -26, 25));
    writer.bit(picture.pic_fields.bits.constrained_intra_pred_flag);
    writer.bit(picture.pic_fields.bits.transform_skip_enabled_flag);
    writer.bit(picture.pic_fields.bits.cu_qp_delta_enabled_flag);
    if (picture.pic_fields.bits.cu_qp_delta_enabled_flag)
        writer.ue(std::min<unsigned>(3, picture.diff_cu_qp_delta_depth));
    writer.se(std::clamp<int>(picture.pps_cb_qp_offset, -12, 12));
    writer.se(std::clamp<int>(picture.pps_cr_qp_offset, -12, 12));
    writer.bit(picture.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag);
    writer.bit(picture.pic_fields.bits.weighted_pred_flag);
    writer.bit(picture.pic_fields.bits.weighted_bipred_flag);
    writer.bit(picture.pic_fields.bits.transquant_bypass_enabled_flag);
    writer.bit(picture.pic_fields.bits.tiles_enabled_flag);
    writer.bit(picture.pic_fields.bits.entropy_coding_sync_enabled_flag);
    if (picture.pic_fields.bits.tiles_enabled_flag) {
        writer.ue(picture.num_tile_columns_minus1);
        writer.ue(picture.num_tile_rows_minus1);
        // VA carries the explicit column/row sizes; a stream with uniform
        // spacing has them all equal, but writing them out is correct for
        // both cases and avoids a wrong PPS for non-uniform tiles.
        writer.bit(0); // uniform_spacing_flag
        for (unsigned i = 0; i < picture.num_tile_columns_minus1 && i < 19; ++i)
            writer.ue(picture.column_width_minus1[i]);
        for (unsigned i = 0; i < picture.num_tile_rows_minus1 && i < 21; ++i)
            writer.ue(picture.row_height_minus1[i]);
        writer.bit(picture.pic_fields.bits.loop_filter_across_tiles_enabled_flag);
    }
    writer.bit(picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);
    const bool deblocking_control = picture.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag
        || picture.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag;
    writer.bit(deblocking_control);
    if (deblocking_control) {
        writer.bit(picture.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag);
        writer.bit(picture.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag);
        if (!picture.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
            writer.se(std::clamp<int>(picture.pps_beta_offset_div2, -6, 6));
            writer.se(std::clamp<int>(picture.pps_tc_offset_div2, -6, 6));
        }
    }
    writer.bit(0); // pps_scaling_list_data_present_flag
    writer.bit(picture.slice_parsing_fields.bits.lists_modification_present_flag);
    writer.ue(std::min<unsigned>(3, picture.log2_parallel_merge_level_minus2));
    writer.bit(picture.slice_parsing_fields.bits.slice_segment_header_extension_present_flag);
    writer.bit(0); // pps_extension_present_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 34 << 1, 1 }, writer);
    return result;
}

std::unique_ptr<Translator> make_hevc(VAProfile profile) { return std::make_unique<HEVC>(profile); }

} // namespace iris::codec
