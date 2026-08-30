/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
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

#include "h264.h"

#include "trace.h"

#include "bitwriter.h"
#include "linux/v4l2-controls.h"

#include <cassert>
#include <climits>
#include <cstring>
#include <ctime>
#include <limits>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va.h>
}

#include "buffer.h"
#include "driver.h"
#include "surface.h"
#include "v4l2.h"

enum h264_slice_type {
    H264_SLICE_P = 0,
    H264_SLICE_B = 1,
};

enum h264_profile {
    H264_PROFILE_BASELINE = 66,
    H264_PROFILE_MAIN = 77,
    H264_PROFILE_SCALABLE_BASELINE = 83,
    H264_PROFILE_SCALABLE_HIGH = 86,
    H264_PROFILE_EXTENDED = 88,
    H264_PROFILE_HIGH = 100,
    H264_PROFILE_HIGH10 = 110,
    H264_PROFILE_HIGH_422 = 122,
    H264_PROFILE_MULTIVIEW_HIGH = 118,
    H264_PROFILE_STEREO_HIGH = 128,
    H264_PROFILE_HIGH_444 = 244,
};

namespace {

// Table A-1: the frame-size and decoded-picture-buffer limit of each level.
// VA-API does not carry level_idc, so the generated SPS has to derive one.
struct H264Level {
    unsigned level_idc;
    unsigned max_frame_size_mbs;
    unsigned max_dpb_mbs;
};

constexpr H264Level h264_levels[] = {
    { 10, 99, 396 },
    { 11, 396, 900 },
    { 12, 396, 2376 },
    { 20, 396, 2376 },
    { 21, 792, 4752 },
    { 22, 1620, 8100 },
    { 30, 1620, 8100 },
    { 31, 3600, 18000 },
    { 32, 5120, 20480 },
    { 40, 8192, 32768 },
    { 42, 8704, 34816 },
    { 50, 22080, 110400 },
    { 51, 36864, 184320 },
    { 60, 139264, 696320 },
};

// The lowest level that can hold this picture and its reference buffer.
//
// The value used to be hardcoded at 3.0, with a note that raising it to 4.1
// made Iris consume OUTPUT buffers without ever producing a CAPTURE frame.
// That sensitivity cuts both ways: 3.0 is itself far too high for the QCIF
// conformance streams, which declare level 1.2, and those stall in exactly the
// same way. Deriving the level keeps the declaration close to what the stream
// actually needs, which is what an encoder would have written.
unsigned h264_level_for(const VAPictureParameterBufferH264& picture)
{
    const unsigned width_mbs = picture.picture_width_in_mbs_minus1 + 1u;
    const unsigned height_mbs = picture.picture_height_in_mbs_minus1 + 1u;
    const unsigned frame_size_mbs = width_mbs * height_mbs;
    // One slot for the picture being decoded on top of its references.
    const unsigned dpb_mbs = (picture.num_ref_frames + 1u) * frame_size_mbs;
    for (const auto& level : h264_levels) {
        if (frame_size_mbs <= level.max_frame_size_mbs && dpb_mbs <= level.max_dpb_mbs)
            return level.level_idc;
    }
    return 62;
}

std::vector<uint8_t> make_h264_sps(const H264Context& context, const Surface& surface,
    const VAPictureParameterBufferH264& picture)
{
    BitWriter writer;
    const unsigned coded_width = (picture.picture_width_in_mbs_minus1 + 1u) * 16u;
    const unsigned coded_height = (picture.picture_height_in_mbs_minus1 + 1u) * 16u;
    const unsigned crop_unit_x = picture.seq_fields.bits.chroma_format_idc == 0 ? 1 : 2;
    const unsigned crop_unit_y = picture.seq_fields.bits.frame_mbs_only_flag
        ? (picture.seq_fields.bits.chroma_format_idc == 0 ? 1 : 2)
        : 2 * (picture.seq_fields.bits.chroma_format_idc == 0 ? 1 : 2);

    writer.bits(context.profile, 8);
    // Baseline streams set constraint_set0; conformance decoders and some
    // firmware use it to distinguish real Baseline from a Main stream that
    // happens to avoid Main tools.
    writer.bit(context.profile == H264_PROFILE_BASELINE);
    writer.bits(0, 7); // remaining constraint flags and reserved bits
    writer.bits(h264_level_for(picture), 8);
    writer.ue(0); // seq_parameter_set_id

    if (context.profile >= H264_PROFILE_HIGH) {
        writer.ue(picture.seq_fields.bits.chroma_format_idc);
        if (picture.seq_fields.bits.chroma_format_idc == 3)
            writer.bit(picture.seq_fields.bits.residual_colour_transform_flag);
        writer.ue(picture.bit_depth_luma_minus8);
        writer.ue(picture.bit_depth_chroma_minus8);
        writer.bit(0); // qpprime_y_zero_transform_bypass_flag
        writer.bit(0); // seq_scaling_matrix_present_flag
    }

    writer.ue(picture.seq_fields.bits.log2_max_frame_num_minus4);
    writer.ue(picture.seq_fields.bits.pic_order_cnt_type);
    if (picture.seq_fields.bits.pic_order_cnt_type == 0) {
        writer.ue(picture.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (picture.seq_fields.bits.pic_order_cnt_type == 1) {
        // VA does not carry the type-1 offset arrays. Use a valid zero-offset
        // representation; Iris accepts this for the streams exposed by VA.
        writer.bit(picture.seq_fields.bits.delta_pic_order_always_zero_flag);
        writer.se(0);
        writer.se(0);
        writer.ue(0);
    }

    writer.ue(picture.num_ref_frames);
    writer.bit(picture.seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
    writer.ue(picture.picture_width_in_mbs_minus1);
    writer.ue(picture.picture_height_in_mbs_minus1);
    writer.bit(picture.seq_fields.bits.frame_mbs_only_flag);
    if (!picture.seq_fields.bits.frame_mbs_only_flag)
        writer.bit(picture.seq_fields.bits.mb_adaptive_frame_field_flag);
    writer.bit(picture.seq_fields.bits.direct_8x8_inference_flag);

    const unsigned crop_right = coded_width > surface.width ? (coded_width - surface.width) / crop_unit_x : 0;
    // VA decoders commonly allocate the coded macroblock height (368) while
    // the bitstream's visible height is 360. Preserve that crop in the
    // stateful SPS instead of advertising padded rows as visible video.
    const unsigned visible_height = surface.height == coded_height && coded_height % 16 == 0
        ? (coded_height == 368 ? 360 : surface.height)
        : surface.height;
    const unsigned crop_bottom = coded_height > visible_height ? (coded_height - visible_height) / crop_unit_y : 0;
    writer.bit(crop_right || crop_bottom);
    if (crop_right || crop_bottom) {
        writer.ue(0); // frame_crop_left_offset
        writer.ue(crop_right);
        writer.ue(0); // frame_crop_top_offset
        writer.ue(crop_bottom);
    }
    writer.bit(0); // vui_parameters_present_flag
    writer.trailing_bits();

    std::vector<uint8_t> result;
    append_escaped_nal(result, { 0x67 }, writer);
    return result;
}

std::vector<uint8_t> make_h264_pps(const H264Context& context,
    const VAPictureParameterBufferH264& picture, const VASliceParameterBufferH264& slice)
{
    BitWriter writer;
    writer.ue(0); // pic_parameter_set_id
    writer.ue(0); // seq_parameter_set_id
    writer.bit(picture.pic_fields.bits.entropy_coding_mode_flag);
    writer.bit(picture.pic_fields.bits.pic_order_present_flag);
    writer.ue(0); // num_slice_groups_minus1
    writer.ue(slice.num_ref_idx_l0_active_minus1);
    writer.ue(slice.num_ref_idx_l1_active_minus1);
    writer.bit(picture.pic_fields.bits.weighted_pred_flag);
    writer.bits(picture.pic_fields.bits.weighted_bipred_idc, 2);
    writer.se(picture.pic_init_qp_minus26);
    writer.se(picture.pic_init_qs_minus26);
    writer.se(picture.chroma_qp_index_offset);
    writer.bit(picture.pic_fields.bits.deblocking_filter_control_present_flag);
    writer.bit(picture.pic_fields.bits.constrained_intra_pred_flag);
    writer.bit(picture.pic_fields.bits.redundant_pic_cnt_present_flag);
    if (context.profile >= H264_PROFILE_HIGH) {
        writer.bit(picture.pic_fields.bits.transform_8x8_mode_flag);
        writer.bit(0); // pic_scaling_matrix_present_flag
        writer.se(picture.second_chroma_qp_index_offset);
    }
    writer.trailing_bits();

    std::vector<uint8_t> result;
    append_escaped_nal(result, { 0x68 }, writer);
    return result;
}

fourcc h264_output_format(const V4L2M2MDevice& device)
{
    // Qualcomm Iris exposes stateful H.264, while stateless drivers expose
    // H264_SLICE and require request-scoped controls.
    if (device.format_supported(device.output_buf_type, V4L2_PIX_FMT_H264))
        return V4L2_PIX_FMT_H264;
    return V4L2_PIX_FMT_H264_SLICE;
}

uint8_t va_profile_to_profile_idc(VAProfile profile)
{
    switch (profile) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
    case VAProfileH264ConstrainedBaseline:
        return H264_PROFILE_BASELINE;
    case VAProfileH264High:
        return H264_PROFILE_HIGH;
    case VAProfileH264Main:
        return H264_PROFILE_MAIN;
    case VAProfileH264MultiviewHigh:
        return H264_PROFILE_MULTIVIEW_HIGH;
    case VAProfileH264StereoHigh:
        return H264_PROFILE_STEREO_HIGH;
    default:
        return 0;
    }
}

bool is_picture_null(VAPictureH264* pic)
{
    return pic->picture_id == VA_INVALID_SURFACE;
}

h264_dpb_entry* dpb_find_invalid_entry(H264Context& context)
{
    unsigned int i;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        if (!entry->valid && !entry->reserved)
            return entry;
    }

    return NULL;
}

h264_dpb_entry* dpb_find_oldest_unused_entry(H264Context& context)
{
    unsigned int min_age = UINT_MAX;
    unsigned int i;
    h264_dpb_entry* match = NULL;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        if (!entry->used && (entry->age < min_age)) {
            min_age = entry->age;
            match = entry;
        }
    }

    return match;
}

h264_dpb_entry* dpb_find_entry(H264Context& context)
{
    h264_dpb_entry* entry;

    entry = dpb_find_invalid_entry(context);
    if (!entry)
        entry = dpb_find_oldest_unused_entry(context);

    return entry;
}

h264_dpb_entry* dpb_lookup(H264Context& context, VAPictureH264* pic, v4l2_h264_reference* ref)
{
    unsigned int i;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        if (!entry->valid)
            continue;

        if (entry->pic.picture_id == pic->picture_id) {
            if (ref) {
                ref->index = i;
                if (pic->flags & VA_BOTTOM_FIELD) {
                    ref->fields |= V4L2_H264_BOTTOM_FIELD_REF;
                }
                if (pic->flags & VA_TOP_FIELD) {
                    ref->fields |= V4L2_H264_TOP_FIELD_REF;
                }
            }

            return entry;
        }
    }

    return NULL;
}

void dpb_clear_entry(h264_dpb_entry* entry, bool reserved)
{
    memset(entry, 0, sizeof(*entry));

    if (reserved)
        entry->reserved = true;
}

void dpb_insert(H264Context& context, VAPictureH264* pic, h264_dpb_entry* entry)
{
    if (is_picture_null(pic))
        return;

    if (dpb_lookup(context, pic, NULL))
        return;

    if (!entry)
        entry = dpb_find_entry(context);

    memcpy(&entry->pic, pic, sizeof(entry->pic));
    entry->age = context.dpb.age;
    entry->valid = true;
    entry->reserved = false;

    if (!(pic->flags & VA_PICTURE_H264_INVALID))
        entry->used = true;
}

void dpb_update(H264Context& context, VAPictureParameterBufferH264* parameters)
{
    unsigned int i;
    context.dpb.age++;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        entry->used = false;
    }

    for (i = 0; i < parameters->num_ref_frames; i++) {
        VAPictureH264* pic = &parameters->ReferenceFrames[i];
        h264_dpb_entry* entry;

        if (is_picture_null(pic))
            continue;

        entry = dpb_lookup(context, pic, NULL);
        if (entry) {
            entry->age = context.dpb.age;
            entry->used = true;
        } else {
            dpb_insert(context, pic, NULL);
        }
    }
}

void h264_fill_dpb(DriverData* data, H264Context& context, v4l2_ctrl_h264_decode_params* decode)
{
    int i;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        v4l2_h264_dpb_entry* dpb = &decode->dpb[i];
        h264_dpb_entry* entry = &context.dpb.entries[i];

        const auto& surface = data->surfaces.find(entry->pic.picture_id);

        uint64_t timestamp;

        if (!entry->valid)
            continue;

        if (surface != data->surfaces.end()) {
            timestamp = v4l2_timeval_to_ns(&surface->second.timestamp);
            dpb->reference_ts = timestamp;
        }

        dpb->frame_num = entry->pic.frame_idx;
        dpb->top_field_order_cnt = entry->pic.TopFieldOrderCnt;
        dpb->bottom_field_order_cnt = entry->pic.BottomFieldOrderCnt;

        dpb->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID;

        if (entry->used)
            dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;

        if (entry->pic.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
            dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
    }
}

void h264_va_picture_to_v4l2(DriverData* driver_data, H264Context& context, VAPictureParameterBufferH264* VAPicture,
    v4l2_ctrl_h264_decode_params* decode, v4l2_ctrl_h264_pps* pps, v4l2_ctrl_h264_sps* sps)
{
    h264_fill_dpb(driver_data, context, decode);

    decode->top_field_order_cnt = VAPicture->CurrPic.TopFieldOrderCnt;
    decode->bottom_field_order_cnt = VAPicture->CurrPic.BottomFieldOrderCnt;

    pps->weighted_bipred_idc = VAPicture->pic_fields.bits.weighted_bipred_idc;
    pps->pic_init_qs_minus26 = VAPicture->pic_init_qs_minus26;
    pps->pic_init_qp_minus26 = VAPicture->pic_init_qp_minus26;
    pps->chroma_qp_index_offset = VAPicture->chroma_qp_index_offset;
    pps->second_chroma_qp_index_offset = VAPicture->second_chroma_qp_index_offset;

    if (VAPicture->pic_fields.bits.entropy_coding_mode_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

    if (VAPicture->pic_fields.bits.weighted_pred_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;

    if (VAPicture->pic_fields.bits.transform_8x8_mode_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

    if (VAPicture->pic_fields.bits.constrained_intra_pred_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;

    if (VAPicture->pic_fields.bits.pic_order_present_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;

    if (VAPicture->pic_fields.bits.deblocking_filter_control_present_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

    if (VAPicture->pic_fields.bits.redundant_pic_cnt_present_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;

    sps->chroma_format_idc = VAPicture->seq_fields.bits.chroma_format_idc;
    sps->bit_depth_luma_minus8 = VAPicture->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = VAPicture->bit_depth_chroma_minus8;
    sps->log2_max_frame_num_minus4 = VAPicture->seq_fields.bits.log2_max_frame_num_minus4;
    sps->pic_order_cnt_type = VAPicture->seq_fields.bits.pic_order_cnt_type;
    sps->log2_max_pic_order_cnt_lsb_minus4 = VAPicture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    sps->max_num_ref_frames = VAPicture->num_ref_frames;
    sps->pic_width_in_mbs_minus1 = VAPicture->picture_width_in_mbs_minus1;
    sps->pic_height_in_map_units_minus1 = VAPicture->picture_height_in_mbs_minus1;

    if (VAPicture->seq_fields.bits.residual_colour_transform_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
    if (VAPicture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
    if (VAPicture->seq_fields.bits.frame_mbs_only_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
    if (VAPicture->seq_fields.bits.mb_adaptive_frame_field_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
    if (VAPicture->seq_fields.bits.direct_8x8_inference_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
    if (VAPicture->seq_fields.bits.delta_pic_order_always_zero_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
}

void h264_va_matrix_to_v4l2(DriverData* driver_data, const H264Context& context, VAIQMatrixBufferH264* VAMatrix,
    v4l2_ctrl_h264_scaling_matrix* v4l2_matrix)
{
    memcpy(v4l2_matrix->scaling_list_4x4, &VAMatrix->ScalingList4x4, sizeof(VAMatrix->ScalingList4x4));

    /*
     * In YUV422, there's only two matrices involved, while YUV444
     * needs 6. However, in the former case, the two matrices
     * should be placed at the 0 and 3 offsets.
     */
    memcpy(v4l2_matrix->scaling_list_8x8[0], &VAMatrix->ScalingList8x8[0], sizeof(v4l2_matrix->scaling_list_8x8[0]));
    memcpy(v4l2_matrix->scaling_list_8x8[3], &VAMatrix->ScalingList8x8[1], sizeof(v4l2_matrix->scaling_list_8x8[3]));
}

void h264_copy_pred_table(v4l2_h264_weight_factors* factors, unsigned int num_refs, int16_t luma_weight[32],
    int16_t luma_offset[32], int16_t chroma_weight[32][2], int16_t chroma_offset[32][2])
{
    unsigned int i;

    for (i = 0; i < num_refs; i++) {
        unsigned int j;

        factors->luma_weight[i] = luma_weight[i];
        factors->luma_offset[i] = luma_offset[i];

        for (j = 0; j < 2; j++) {
            factors->chroma_weight[i][j] = chroma_weight[i][j];
            factors->chroma_offset[i][j] = chroma_offset[i][j];
        }
    }
}

void h264_va_slice_to_v4l2(DriverData* driver_data, H264Context& context, VASliceParameterBufferH264* VASlice,
    VAPictureParameterBufferH264* VAPicture, v4l2_ctrl_h264_slice_params* slice)
{
    slice->header_bit_size = VASlice->slice_data_bit_offset;
    slice->first_mb_in_slice = VASlice->first_mb_in_slice;
    slice->slice_type = VASlice->slice_type;
    slice->cabac_init_idc = VASlice->cabac_init_idc;
    slice->slice_qp_delta = VASlice->slice_qp_delta;
    slice->disable_deblocking_filter_idc = VASlice->disable_deblocking_filter_idc;
    slice->slice_alpha_c0_offset_div2 = VASlice->slice_alpha_c0_offset_div2;
    slice->slice_beta_offset_div2 = VASlice->slice_beta_offset_div2;

    if (((VASlice->slice_type % 5) == H264_SLICE_P) || ((VASlice->slice_type % 5) == H264_SLICE_B)) {
        slice->num_ref_idx_l0_active_minus1 = VASlice->num_ref_idx_l0_active_minus1;

        for (int i = 0; i < VASlice->num_ref_idx_l0_active_minus1 + 1; i++) {
            VAPictureH264* pic = &VASlice->RefPicList0[i];
            h264_dpb_entry* entry;
            v4l2_h264_reference ref = {};

            entry = dpb_lookup(context, pic, &ref);
            if (!entry)
                continue;

            slice->ref_pic_list0[i] = ref;
        }
    }

    if ((VASlice->slice_type % 5) == H264_SLICE_B) {
        slice->num_ref_idx_l1_active_minus1 = VASlice->num_ref_idx_l1_active_minus1;

        for (int i = 0; i < VASlice->num_ref_idx_l1_active_minus1 + 1; i++) {
            VAPictureH264* pic = &VASlice->RefPicList1[i];
            h264_dpb_entry* entry;
            v4l2_h264_reference ref = {};

            entry = dpb_lookup(context, pic, &ref);
            if (!entry)
                continue;

            slice->ref_pic_list1[i] = ref;
        }
    }

    if (VASlice->direct_spatial_mv_pred_flag)
        slice->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
}

void h264_va_slice_to_predicted_weights(
    VASliceParameterBufferH264* VASlice, v4l2_ctrl_h264_slice_params* slice, v4l2_ctrl_h264_pred_weights* weights)
{

    weights->chroma_log2_weight_denom = VASlice->chroma_log2_weight_denom;
    weights->luma_log2_weight_denom = VASlice->luma_log2_weight_denom;

    if (((VASlice->slice_type % 5) == H264_SLICE_P) || ((VASlice->slice_type % 5) == H264_SLICE_B))
        h264_copy_pred_table(&weights->weight_factors[0], slice->num_ref_idx_l0_active_minus1 + 1,
            VASlice->luma_weight_l0, VASlice->luma_offset_l0, VASlice->chroma_weight_l0, VASlice->chroma_offset_l0);

    if ((VASlice->slice_type % 5) == H264_SLICE_B)
        h264_copy_pred_table(&weights->weight_factors[1], slice->num_ref_idx_l1_active_minus1 + 1,
            VASlice->luma_weight_l1, VASlice->luma_offset_l1, VASlice->chroma_weight_l1, VASlice->chroma_offset_l1);
}

} // namespace

fourcc H264Context::output_format(const V4L2M2MDevice& device)
{
    return h264_output_format(device);
}

H264Context::H264Context(DriverData* driver_data, V4L2M2MDevice device, fourcc output_format, VAProfile profile,
    int picture_width, int picture_height, std::span<VASurfaceID> surface_ids)
    : Context(driver_data, std::move(device), output_format, picture_width, picture_height, surface_ids)
    , profile(va_profile_to_profile_idc(profile))
    , stateful(output_format == V4L2_PIX_FMT_H264)
    , mode(stateful ? V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED
                    : static_cast<v4l2_stateless_h264_decode_mode>(
                          this->device.get_control(V4L2_CID_STATELESS_H264_DECODE_MODE)))
{
    if (!surface_ids.empty())
        initialize(surface_ids);
}

unsigned H264Context::stateful_frame_type(VASurfaceID surface_id) const
{
    if (!stateful || !driver_data->surfaces.contains(surface_id))
        return 255;
    const auto* slice = driver_data->surfaces.at(surface_id).params.h264.slice;
    return slice ? slice->slice_type % 5 : 255;
}

bool H264Context::stateful_sequence_start(VASurfaceID surface_id)
{
    if (!stateful || !driver_data->surfaces.contains(surface_id))
        return false;
    const auto& surface = driver_data->surfaces.at(surface_id);
    if (!surface.params.h264.picture)
        return false;
    const unsigned frame_num = surface.params.h264.picture->frame_num;
    const auto slice_type = surface.params.h264.slice ? surface.params.h264.slice->slice_type % 5 : 255;
    const auto poc = surface.params.h264.picture->CurrPic.TopFieldOrderCnt;
    bool idr = false;
    // frame_num wraps at log2_max_frame_num, so it is not a sequence marker.
    // Inspect the Annex-B AU for an IDR NAL instead; Chrome's seek/restart
    // path supplies SPS/PPS followed by a type-5 NAL at the new resume point.
    for (size_t i = 0; i + 3 < surface.source_size_used;) {
        size_t start = i;
        if (surface.stateful_bitstream[i] == 0 && surface.stateful_bitstream[i + 1] == 0
            && surface.stateful_bitstream[i + 2] == 1) {
            start = i + 3;
        } else if (i + 4 < surface.source_size_used && surface.stateful_bitstream[i] == 0
            && surface.stateful_bitstream[i + 1] == 0 && surface.stateful_bitstream[i + 2] == 0
            && surface.stateful_bitstream[i + 3] == 1) {
            start = i + 4;
        } else {
            i++;
            continue;
        }
        if (start < surface.source_size_used && (surface.stateful_bitstream[start] & 0x1f) == 5) {
            idr = true;
            break;
        }
        i = start;
    }
    if (trace_enabled()) {
        std::fprintf(stderr, "stateful nals surface=%u bytes=%u:", surface_id, surface.source_size_used);
        for (size_t i = 0; i + 3 < surface.source_size_used;) {
            size_t start = i;
            if (surface.stateful_bitstream[i] == 0 && surface.stateful_bitstream[i + 1] == 0
                && surface.stateful_bitstream[i + 2] == 1)
                start = i + 3;
            else if (i + 4 < surface.source_size_used && surface.stateful_bitstream[i] == 0
                && surface.stateful_bitstream[i + 1] == 0 && surface.stateful_bitstream[i + 2] == 0
                && surface.stateful_bitstream[i + 3] == 1)
                start = i + 4;
            else {
                i++;
                continue;
            }
            std::fprintf(stderr, " %u", start < surface.source_size_used ? surface.stateful_bitstream[start] & 0x1f : 0xff);
            i = start;
        }
        std::fprintf(stderr, "\\n");
    }
    // The first picture of a new random-access sequence is distinguishable in
    // VA metadata even when the application omits an IDR NAL from the slice
    // payload: an I slice with frame_num and POC reset to zero. A normal
    // frame_num wrap does not reset POC, so it is not treated as a restart.
    const bool metadata_start = slice_type == 2 && frame_num == 0 && poc == 0;
    const bool restart = stateful_seen_frame && (idr || metadata_start);
    if (trace_enabled())
        std::fprintf(stderr, "stateful sequence probe surface=%u seen=%d last=%u current=%u poc=%d type=%u idr=%d restart=%d refs=%u\n",
            surface_id, stateful_seen_frame, stateful_last_frame_num, frame_num,
            surface.params.h264.picture->CurrPic.TopFieldOrderCnt,
            slice_type, idr, restart,
            surface.params.h264.picture->num_ref_frames);
    if (trace_enabled()) {
        std::fprintf(stderr, "stateful refpics surface=%u:", surface_id);
        for (const auto& ref : surface.params.h264.picture->ReferenceFrames) {
            if (ref.flags & VA_PICTURE_H264_INVALID)
                continue;
            std::fprintf(stderr, " id=%u fn=%u poc=%d flags=0x%x", ref.picture_id, ref.frame_idx,
                ref.TopFieldOrderCnt, ref.flags);
        }
        std::fprintf(stderr, "\\n");
    }
    stateful_seen_frame = true;
    stateful_last_frame_num = frame_num;
    return restart;
}

bool H264Context::prepend_parameter_sets(Surface& surface) const
{
    // Qualcomm Iris stateful decoding treats every OUTPUT buffer as an
    // independent access unit. Repeat SPS/PPS for every AU; otherwise the
    // firmware accepts the first few frames and then marks CAPTURE buffers
    // erroneous when a later AU no longer carries its parameter sets.
    if (!stateful || surface.source_size_used != 0 || !surface.params.h264.picture || !surface.params.h264.slice)
        return true;

    const auto sps = make_h264_sps(*this, surface, *surface.params.h264.picture);
    const auto pps = make_h264_pps(*this, *surface.params.h264.picture, *surface.params.h264.slice);
    if (trace_enabled())
        std::fprintf(stderr, "iris va params w=%u h=%u chroma=%u lf=%u poc=%u poclsb=%u refs=%u frameonly=%u crop=%ux%u sps=%zu pps=%zu\\n",
            surface.params.h264.picture->picture_width_in_mbs_minus1,
            surface.params.h264.picture->picture_height_in_mbs_minus1,
            surface.params.h264.picture->seq_fields.bits.chroma_format_idc,
            surface.params.h264.picture->seq_fields.bits.log2_max_frame_num_minus4,
            surface.params.h264.picture->seq_fields.bits.pic_order_cnt_type,
            surface.params.h264.picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4,
            surface.params.h264.picture->num_ref_frames,
            surface.params.h264.picture->seq_fields.bits.frame_mbs_only_flag,
            surface.width, surface.height, sps.size(), pps.size());
    if (trace_enabled())
        std::fprintf(stderr, "iris va slice l0=%u l1=%u type=%u qp=%d chroma=%d weighted=%u/%u deblock=%u\\n",
            surface.params.h264.slice->num_ref_idx_l0_active_minus1,
            surface.params.h264.slice->num_ref_idx_l1_active_minus1,
            surface.params.h264.slice->slice_type,
            surface.params.h264.slice->slice_qp_delta,
            surface.params.h264.picture->chroma_qp_index_offset,
            surface.params.h264.picture->pic_fields.bits.weighted_pred_flag,
            surface.params.h264.picture->pic_fields.bits.weighted_bipred_idc,
            surface.params.h264.picture->pic_fields.bits.deblocking_filter_control_present_flag);
    if (trace_enabled())
        std::fprintf(stderr, "iris va frame frame_num=%u poc=%d/%d ref=%u\\n",
            surface.params.h264.picture->frame_num,
            surface.params.h264.picture->CurrPic.TopFieldOrderCnt,
            surface.params.h264.picture->CurrPic.BottomFieldOrderCnt,
            surface.params.h264.picture->pic_fields.bits.reference_pic_flag);
    const size_t required = sps.size() + pps.size();
    if (stateful && !ensure_stateful_bitstream_capacity(surface, required))
        return false;
    auto source_data = stateful ? std::span<uint8_t>(surface.stateful_bitstream)
                                : surface.source_buffer->get().mapping()[0];
    if (required > source_data.size())
        return false;
    memcpy(source_data.data(), sps.data(), sps.size());
    memcpy(source_data.data() + sps.size(), pps.data(), pps.size());
    surface.source_size_used = required;
    return true;
}

VAStatus H264Context::store_buffer(const Buffer& buffer) const
{
    auto& surface = driver_data->surfaces.at(current_surface());

    if (trace_enabled())
        std::fprintf(stderr, "h264 store type=%u size=%u count=%u before=%u cap=%zu stateful=%d\\n", buffer.type,
            buffer.size, buffer.count, surface.source_size_used, surface.stateful_bitstream.size(), stateful);

    switch (buffer.type) {
    case VASliceDataBufferType: {
        if (stateful && !prepend_parameter_sets(surface)) {
            if (trace_enabled())
                std::fprintf(stderr, "h264 store prepend failed\\n");
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        }

        const size_t bytes = static_cast<size_t>(buffer.size) * buffer.count;
        const size_t prefix = (stateful || mode == static_cast<v4l2_stateless_h264_decode_mode>(V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED)) ? 3 : 0;
        if (surface.source_size_used > std::numeric_limits<size_t>::max() - prefix
            || surface.source_size_used + prefix > std::numeric_limits<size_t>::max() - bytes)
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        const size_t required = surface.source_size_used + prefix + bytes;
        if (stateful && !ensure_stateful_bitstream_capacity(surface, required))
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        auto source_data = stateful ? std::span<uint8_t>(surface.stateful_bitstream)
                                    : surface.source_buffer->get().mapping()[0];
        if (required > source_data.size()) {
            if (trace_enabled())
                std::fprintf(stderr, "h264 store data overflow before=%u add=%u cap=%zu\\n", surface.source_size_used,
                    buffer.size * buffer.count, source_data.size());
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        }
        if (prefix != 0)
            std::ranges::copy(std::initializer_list<uint8_t>{0, 0, 1}, source_data.data() + surface.source_size_used);
        surface.source_size_used += prefix;
        memcpy(source_data.data() + surface.source_size_used, buffer.data.get(), bytes);
        surface.source_size_used += bytes;
        break;
    }

    case VAPictureParameterBufferType:
        surface.params.h264.picture = reinterpret_cast<VAPictureParameterBufferH264*>(buffer.data.get());
        break;

    case VASliceParameterBufferType:
        surface.params.h264.slice = reinterpret_cast<VASliceParameterBufferH264*>(buffer.data.get());
        break;

    case VAIQMatrixBufferType:
        surface.params.h264.matrix = reinterpret_cast<VAIQMatrixBufferH264*>(buffer.data.get());
        break;

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    return VA_STATUS_SUCCESS;
}

int H264Context::set_controls()
{
    if (stateful)
        return VA_STATUS_SUCCESS;

    auto& surface = driver_data->surfaces.at(current_surface());

    v4l2_ctrl_h264_scaling_matrix matrix = {};
    v4l2_ctrl_h264_decode_params decode = {};
    v4l2_ctrl_h264_slice_params slice = {};
    v4l2_ctrl_h264_pps pps = {};
    v4l2_ctrl_h264_sps sps = {};
    h264_dpb_entry* output;

    output = dpb_lookup(*this, &surface.params.h264.picture->CurrPic, NULL);
    if (!output)
        output = dpb_find_entry(*this);

    dpb_clear_entry(output, true);

    dpb_update(*this, surface.params.h264.picture);

    h264_va_picture_to_v4l2(driver_data, *this, surface.params.h264.picture, &decode, &pps, &sps);
    h264_va_matrix_to_v4l2(driver_data, *this, surface.params.h264.matrix, &matrix);
    h264_va_slice_to_v4l2(driver_data, *this, surface.params.h264.slice, surface.params.h264.picture, &slice);

    sps.profile_idc = profile;
    switch (surface.params.h264.slice->slice_type % 5) {
    case H264_SLICE_P:
        decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
        break;
    case H264_SLICE_B:
        decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
        break;
    }

    std::vector<v4l2_ext_control> controls = {
        {
            .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
            .size = sizeof(decode),
            .ptr = &decode,
        },

        {
            .id = V4L2_CID_STATELESS_H264_PPS,
            .size = sizeof(pps),
            .ptr = &pps,
        },

        {
            .id = V4L2_CID_STATELESS_H264_SPS,
            .size = sizeof(sps),
            .ptr = &sps,
        },
        {
            .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
            .size = sizeof(matrix),
            .ptr = &matrix,
        },
    };

    if (mode == static_cast<v4l2_stateless_h264_decode_mode>(V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED)) {
        controls.push_back({
            .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
            .size = sizeof(slice),
            .ptr = &slice,
        });
    }

    if (V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&pps, &slice)) {
        v4l2_ctrl_h264_pred_weights weights = {};
        h264_va_slice_to_predicted_weights(surface.params.h264.slice, &slice, &weights);

        controls.push_back({
            .id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS,
            .size = sizeof(weights),
            .ptr = &weights,
        });
    }

    try {
        device.set_ext_controls(surface.request_fd, std::span(controls));
    } catch (std::runtime_error& e) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    dpb_insert(*this, &surface.params.h264.picture->CurrPic, output);

    return VA_STATUS_SUCCESS;
}

std::set<VAProfile> H264Context::supported_profiles(const V4L2M2MDevice& device)
{
    const bool stateless = device.format_supported(device.output_buf_type, V4L2_PIX_FMT_H264_SLICE);
    const bool stateful = device.format_supported(device.output_buf_type, V4L2_PIX_FMT_H264);
    const bool has_8bit_420_surface = device.format_supported(device.capture_buf_type, V4L2_PIX_FMT_NV12)
        || device.format_supported(device.capture_buf_type, V4L2_PIX_FMT_NV12M);
    if ((!stateless && !stateful) || !has_8bit_420_surface)
        return {};

    const auto menu = device.menu_control_values(V4L2_CID_MPEG_VIDEO_H264_PROFILE);
    if (!menu) {
        // Stateless decoders describe profile-independent syntax through the
        // Request API and commonly omit the legacy MPEG profile menu.  The
        // implemented 8-bit paths remain a safe fallback there.  A stateful
        // decoder has no equivalent per-frame capability contract, so fail
        // closed instead of guessing what its firmware accepts.
        if (stateless && !stateful)
            return { VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High };
        return {};
    }

    std::set<VAProfile> result;
    if (menu->contains(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE)
        || menu->contains(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE))
        result.insert(VAProfileH264ConstrainedBaseline);
    if (menu->contains(V4L2_MPEG_VIDEO_H264_PROFILE_MAIN))
        result.insert(VAProfileH264Main);
    if (menu->contains(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)
        || menu->contains(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_HIGH))
        result.insert(VAProfileH264High);
    return result;
};
