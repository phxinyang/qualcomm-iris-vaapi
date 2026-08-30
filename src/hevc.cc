/* Qualcomm Iris stateful HEVC VA-API context. */

#include "hevc.h"

#include "trace.h"

#include "bitwriter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

extern "C" {
#include <va/va_dec_hevc.h>
}

#include "buffer.h"
#include "driver.h"
#include "surface.h"
#include "v4l2.h"

namespace {

void profile_tier_level(BitWriter& writer, const VAPictureParameterBufferHEVC& picture, unsigned max_sub_layers_minus1)
{
    writer.bits(0, 2); // general_profile_space
    writer.bit(0); // general_tier_flag: Main tier
    writer.bits(1, 5); // general_profile_idc: Main
    // Main profile streams produced by the tablet advertise Main and Main 10
    // compatibility (the two high bits). Keeping this value also makes the
    // generated VPS/SPS match Iris' native client stream.
    writer.bits(0x60000000u, 32); // compatibility flags
    writer.bits(0, 48); // constraint flags
    writer.bits(186, 8); // level 6.2, above the tablet's 1080x1920 stream
    for (unsigned i = 0; i < max_sub_layers_minus1; ++i) {
        writer.bit(0); // sub_layer_profile_present_flag
        writer.bit(0); // sub_layer_level_present_flag
    }
    for (unsigned i = max_sub_layers_minus1; i < 8; ++i)
        writer.bits(0, 2); // reserved_zero_2bits
    (void)picture;
}

std::vector<uint8_t> make_hevc_vps(const VAPictureParameterBufferHEVC& picture)
{
    BitWriter writer;
    writer.bits(0, 4); // vps_video_parameter_set_id
    writer.bit(1); // vps_base_layer_internal_flag
    writer.bit(1); // vps_base_layer_available_flag
    writer.bits(0, 6); // vps_max_layers_minus1
    writer.bits(1, 3); // vps_max_sub_layers_minus1
    writer.bit(0); // vps_temporal_id_nesting_flag
    writer.bits(0xffff, 16); // vps_reserved_0xffff_16bits
    profile_tier_level(writer, picture, 1);
    writer.bit(1); // vps_sub_layer_ordering_info_present_flag
    for (unsigned i = 0; i < 2; ++i) {
        writer.ue(std::min<unsigned>(31, picture.sps_max_dec_pic_buffering_minus1));
        writer.ue(picture.pic_fields.bits.NoPicReorderingFlag ? 0 : (i == 0 ? 3 : 4));
        writer.ue(0); // vps_max_latency_increase_plus1
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

std::vector<uint8_t> make_hevc_sps(const VAPictureParameterBufferHEVC& picture)
{
    BitWriter writer;
    writer.bits(0, 4); // sps_video_parameter_set_id
    writer.bits(1, 3); // sps_max_sub_layers_minus1
    writer.bit(1); // sps_temporal_id_nesting_flag
    profile_tier_level(writer, picture, 1);
    writer.ue(0); // sps_seq_parameter_set_id
    writer.ue(picture.pic_fields.bits.chroma_format_idc);
    if (picture.pic_fields.bits.chroma_format_idc == 3)
        writer.bit(picture.pic_fields.bits.separate_colour_plane_flag);
    writer.ue(picture.pic_width_in_luma_samples);
    writer.ue(picture.pic_height_in_luma_samples);
    writer.bit(1); // conformance_window_flag (zero offsets are valid)
    writer.ue(0); // conf_win_left_offset
    writer.ue(0); // conf_win_right_offset
    writer.ue(0); // conf_win_top_offset
    writer.ue(0); // conf_win_bottom_offset
    writer.ue(picture.bit_depth_luma_minus8);
    writer.ue(picture.bit_depth_chroma_minus8);
    writer.ue(picture.log2_max_pic_order_cnt_lsb_minus4);
    writer.bit(1); // sps_sub_layer_ordering_info_present_flag
    for (unsigned i = 0; i < 2; ++i) {
        writer.ue(std::min<unsigned>(31, picture.sps_max_dec_pic_buffering_minus1));
        writer.ue(picture.pic_fields.bits.NoPicReorderingFlag ? 0 : (i == 0 ? 3 : 4));
        writer.ue(0); // sps_max_latency_increase_plus1
    }
    writer.ue(picture.log2_min_luma_coding_block_size_minus3);
    writer.ue(picture.log2_diff_max_min_luma_coding_block_size);
    writer.ue(picture.log2_min_transform_block_size_minus2);
    writer.ue(picture.log2_diff_max_min_transform_block_size);
    writer.ue(picture.max_transform_hierarchy_depth_intra);
    writer.ue(picture.max_transform_hierarchy_depth_inter);
    writer.bit(picture.pic_fields.bits.scaling_list_enabled_flag);
    if (picture.pic_fields.bits.scaling_list_enabled_flag)
        writer.bit(0); // sps_scaling_list_data_present_flag
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
    // VA does not expose the original SPS, but it does expose the current
    // short-term DPB and its RPS_ST_CURR_* flags. Reconstruct one explicit
    // (non-predicted) RPS from that information and repeat it for every SPS
    // index. Long-format VA slices carry the original index, so repeating the
    // current picture's RPS keeps that index valid without rewriting headers.
    struct RpsEntry {
        int32_t poc;
        bool used;
    };
    std::vector<RpsEntry> negative;
    std::vector<RpsEntry> positive;
    for (const auto& reference : picture.ReferenceFrames) {
        if (reference.flags & VA_PICTURE_HEVC_INVALID)
            continue;
        const bool before = reference.flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        const bool after = reference.flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
        if (before)
            negative.push_back({ reference.pic_order_cnt, true });
        else if (after)
            positive.push_back({ reference.pic_order_cnt, true });
    }
    std::sort(negative.begin(), negative.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.poc > rhs.poc;
    });
    std::sort(positive.begin(), positive.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.poc < rhs.poc;
    });
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
            writer.bit(negative[j].used ? 1 : 0);
            previous = negative[j].poc;
        }
        previous = picture.CurrPic.pic_order_cnt;
        for (size_t j = 0; j < positive.size() && j < 16; ++j) {
            const int32_t delta = positive[j].poc - previous;
            writer.ue(delta > 0 ? static_cast<uint32_t>(delta - 1) : 0);
            writer.bit(positive[j].used ? 1 : 0);
            previous = positive[j].poc;
        }
    }
    writer.bit(0); // long_term_ref_pics_present_flag
    writer.bit(picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag);
    writer.bit(picture.pic_fields.bits.strong_intra_smoothing_enabled_flag);
    writer.bit(0); // vui_parameters_present_flag
    writer.bit(0); // sps_extension_present_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 33 << 1, 1 }, writer);
    return result;
}

std::vector<uint8_t> make_hevc_pps(const VAPictureParameterBufferHEVC& picture)
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
        writer.bit(1); // uniform_spacing_flag
    }
    if (picture.pic_fields.bits.tiles_enabled_flag)
        writer.bit(picture.pic_fields.bits.loop_filter_across_tiles_enabled_flag);
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

bool has_annexb_start_code(const uint8_t* data, size_t size)
{
    return size >= 3 && data[0] == 0 && data[1] == 0
        && (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
}

}

bool HEVCContext::prepend_parameter_sets(Surface& surface) const
{
    if (surface.source_size_used != 0 || !surface.params.hevc.picture)
        return true;

    const auto vps = make_hevc_vps(*surface.params.hevc.picture);
    const auto sps = make_hevc_sps(*surface.params.hevc.picture);
    const auto pps = make_hevc_pps(*surface.params.hevc.picture);
    const size_t required = vps.size() + sps.size() + pps.size();
    if (!ensure_stateful_bitstream_capacity(surface, required))
        return false;
    auto destination = std::span<uint8_t>(surface.stateful_bitstream);
    size_t offset = 0;
    for (const auto* nal : { &vps, &sps, &pps }) {
        std::copy(nal->begin(), nal->end(), destination.begin() + offset);
        offset += nal->size();
    }
    surface.source_size_used = offset;
    if (trace_enabled())
        std::fprintf(stderr, "hevc generated parameter sets vps=%zu sps=%zu pps=%zu total=%zu\n",
            vps.size(), sps.size(), pps.size(), required);
    return true;
}

VAStatus HEVCContext::store_buffer(const Buffer& buffer) const
{
    auto& surface = driver_data->surfaces.at(current_surface());
    switch (buffer.type) {
    case VAPictureParameterBufferType:
        surface.params.hevc.picture = reinterpret_cast<VAPictureParameterBufferHEVC*>(buffer.data.get());
        if (trace_enabled()) {
            const auto* picture = surface.params.hevc.picture;
            std::fprintf(stderr, "hevc params w=%u h=%u chroma=%u bitdepth=%u/%u poc=%u refs=%u reorder=%u\n",
                picture->pic_width_in_luma_samples, picture->pic_height_in_luma_samples,
                picture->pic_fields.bits.chroma_format_idc,
                picture->bit_depth_luma_minus8 + 8, picture->bit_depth_chroma_minus8 + 8,
                picture->log2_max_pic_order_cnt_lsb_minus4,
                picture->sps_max_dec_pic_buffering_minus1 + 1,
                picture->pic_fields.bits.NoPicReorderingFlag ? 0 : 1);
            std::fprintf(stderr, "hevc refs curr=%u/%d/%d rpsbits=%u short_rps=%u long_rps=%u\n",
                picture->CurrPic.picture_id, picture->CurrPic.pic_order_cnt,
                picture->CurrPic.flags, picture->st_rps_bits,
                picture->num_short_term_ref_pic_sets, picture->num_long_term_ref_pic_sps);
        }
        return VA_STATUS_SUCCESS;

    case VASliceParameterBufferType:
        surface.params.hevc.slice = reinterpret_cast<VASliceParameterBufferHEVC*>(buffer.data.get());
        if (trace_enabled()) {
            const auto* slice = surface.params.hevc.slice;
            std::fprintf(stderr, "hevc slice size=%u off=%u hdr=%u type=%u addr=%u last=%u\n",
                slice->slice_data_size, slice->slice_data_offset,
                slice->slice_data_byte_offset, slice->LongSliceFlags.fields.slice_type,
                slice->slice_segment_address, slice->LongSliceFlags.fields.LastSliceOfPic);
        }
        return VA_STATUS_SUCCESS;

    case VASliceDataBufferType: {
        const size_t bytes = static_cast<size_t>(buffer.size) * buffer.count;
        if (bytes == 0)
            return VA_STATUS_SUCCESS;

        if (trace_enabled()) {
            std::fprintf(stderr, "hevc data bytes=%zu first=", bytes);
            for (size_t i = 0; i < std::min<size_t>(8, bytes); ++i)
                std::fprintf(stderr, "%02x", buffer.data.get()[i]);
            std::fprintf(stderr, "\n");
        }

        if (!prepend_parameter_sets(surface))
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;

        // VA callers do not consistently retain the Annex-B prefix when they
        // hand a slice to the driver. Qualcomm Iris stateful OUTPUT expects
        // every NAL unit to start with one, just like the native V4L2 client.
        // Preserve an existing 3/4-byte prefix to avoid creating an invalid
        // double prefix for callers that do provide it.
        const uint8_t* source = buffer.data.get();
        size_t source_bytes = bytes;
        // Do not discard a leading zero: 00 02 is a valid two-byte HEVC NAL
        // header for nal_unit_type 0, not an alignment prefix.
        const size_t prefix = has_annexb_start_code(source, source_bytes) ? 0 : 3;
        if (surface.source_size_used > std::numeric_limits<size_t>::max() - prefix
            || surface.source_size_used + prefix > std::numeric_limits<size_t>::max() - source_bytes
            || !ensure_stateful_bitstream_capacity(surface, surface.source_size_used + prefix + source_bytes))
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        auto destination = std::span<uint8_t>(surface.stateful_bitstream);
        if (prefix != 0) {
            destination[surface.source_size_used++] = 0;
            destination[surface.source_size_used++] = 0;
            destination[surface.source_size_used++] = 1;
        }
        std::memcpy(destination.data() + surface.source_size_used, source, source_bytes);
        surface.source_size_used += prefix + source_bytes;
        return VA_STATUS_SUCCESS;
    }

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
}

std::set<VAProfile> HEVCContext::supported_profiles(const V4L2M2MDevice& device)
{
    // The VA long-format API does not provide the original VPS/SPS/PPS
    // extradata. The generated parameter-set path below is validated against
    // the Qualcomm Iris matrix and is therefore enabled by default on nodes
    // that expose stateful HEVC. Keep an explicit opt-out for firmware builds
    // with non-standard parameter-set requirements.
    const char* disabled = std::getenv("V4L2_VA_DISABLE_HEVC_STATEFUL");
    if (disabled && std::strcmp(disabled, "1") == 0)
        return {};
    if (!device.format_supported(device.output_buf_type, V4L2_PIX_FMT_HEVC))
        return {};
    return { VAProfileHEVCMain };
}
