// SPDX-License-Identifier: MIT
#include "hevc_headers.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void fill_picture(VAPictureParameterBufferHEVC *picture)
{
    memset(picture, 0, sizeof(*picture));
    picture->pic_width_in_luma_samples = 1280;
    picture->pic_height_in_luma_samples = 736;
    picture->pic_fields.bits.chroma_format_idc = 1;
    picture->pic_fields.bits.strong_intra_smoothing_enabled_flag = 1;
    picture->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    picture->log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture->sps_max_dec_pic_buffering_minus1 = 5;
    picture->log2_diff_max_min_luma_coding_block_size = 3;
    picture->log2_diff_max_min_transform_block_size = 3;
    picture->max_transform_hierarchy_depth_inter = 2;
    picture->max_transform_hierarchy_depth_intra = 2;
}

static void fill_sequence(const VAPictureParameterBufferHEVC *picture,
                          struct venus_hevc_sequence *sequence)
{
    sequence->picture = picture;
    sequence->coded_width = picture->pic_width_in_luma_samples;
    sequence->coded_height = picture->pic_height_in_luma_samples;
    sequence->visible_width = 1280;
    sequence->visible_height = 720;
}

/* Reads the NAL unit types out of an Annex B byte stream. */
static size_t unit_types(const uint8_t *data, size_t size,
                         unsigned int *types, size_t maximum)
{
    size_t count = 0;
    size_t index = 0;

    while (count < maximum && index + 4 <= size) {
        size_t header;

        if (data[index] || data[index + 1])
            break;
        if (data[index + 2] == 1)
            header = index + 3;
        else if (data[index + 2] == 0 && data[index + 3] == 1)
            header = index + 4;
        else
            break;
        if (header + 1 >= size)
            break;

        types[count++] = data[header] >> 1;
        index = header + 2;
        while (index + 4 <= size &&
               (data[index] || data[index + 1] ||
                (data[index + 2] != 1 &&
                 !(data[index + 2] == 0 && data[index + 3] == 1))))
            index++;
    }

    return count;
}

static void test_parameter_sets(void)
{
    VAPictureParameterBufferHEVC picture;
    struct venus_hevc_sequence sequence;
    uint8_t output[VENUS_HEVC_HEADERS_MAX * 2];
    struct venus_annexb_writer writer = {
        .data = output,
        .capacity = sizeof(output),
    };
    unsigned int types[4];

    fill_picture(&picture);
    fill_sequence(&picture, &sequence);

    assert(venus_hevc_write_parameter_sets(&writer, &sequence, 0) == 0);
    assert(writer.length > 0);
    assert(writer.length < VENUS_HEVC_HEADERS_MAX);

    assert(unit_types(writer.data, writer.length, types, 4) == 3);
    assert(types[0] == 32); /* VPS */
    assert(types[1] == 33); /* SPS */
    assert(types[2] == 34); /* PPS */
}

/* Reference picture sets that live in the SPS, and long term reference
 * pictures, are not part of the VA-API picture parameters: those streams are
 * refused rather than decoded against parameter sets that would not match
 * their slice headers.
 */
static void test_unsupported_sequences(void)
{
    VAPictureParameterBufferHEVC picture;
    struct venus_hevc_sequence sequence;
    uint8_t output[VENUS_HEVC_HEADERS_MAX * 2];
    struct venus_annexb_writer writer = {
        .data = output,
        .capacity = sizeof(output),
    };

    fill_picture(&picture);
    fill_sequence(&picture, &sequence);

    picture.num_short_term_ref_pic_sets = 1;
    assert(venus_hevc_write_parameter_sets(&writer, &sequence, 0) == -ENOTSUP);

    picture.num_short_term_ref_pic_sets = 0;
    picture.num_long_term_ref_pic_sps = 1;
    assert(venus_hevc_write_parameter_sets(&writer, &sequence, 0) == -ENOTSUP);
}

/* A VA-API client reports whether a stream reorders pictures, and the
 * parameter sets have to say so: a decoder told to lag as far behind its input
 * as the DPB allows never returns a picture to a client that has stopped
 * submitting.  Stamping the DPB size in regardless of the flag - which this
 * used to do - makes both of these renders identical.
 */
static void test_reorder_pics(void)
{
    VAPictureParameterBufferHEVC picture;
    struct venus_hevc_sequence sequence;
    uint8_t reordered[VENUS_HEVC_HEADERS_MAX * 2];
    uint8_t no_reorder[VENUS_HEVC_HEADERS_MAX * 2];
    struct venus_annexb_writer writer;
    size_t reordered_size;
    size_t no_reorder_size;

    fill_picture(&picture);
    fill_sequence(&picture, &sequence);

    picture.pic_fields.bits.NoPicReorderingFlag = 0;
    writer = (struct venus_annexb_writer) {
        .data = reordered,
        .capacity = sizeof(reordered),
    };
    assert(venus_hevc_write_parameter_sets(&writer, &sequence, 0) == 0);
    reordered_size = writer.length;

    picture.pic_fields.bits.NoPicReorderingFlag = 1;
    writer = (struct venus_annexb_writer) {
        .data = no_reorder,
        .capacity = sizeof(no_reorder),
    };
    assert(venus_hevc_write_parameter_sets(&writer, &sequence, 0) == 0);
    no_reorder_size = writer.length;

    assert(reordered_size != no_reorder_size ||
           memcmp(reordered, no_reorder, reordered_size) != 0);
    assert(no_reorder_size < VENUS_HEVC_HEADERS_MAX);
}

/* The picture parameter set id sits behind the two NAL unit header bytes,
 * the first slice flag and, on a random access picture, the
 * no_output_of_prior_pics flag.
 */
static void test_pps_id(void)
{
    /* first slice, random access, ue(0) */
    static const uint8_t idr[] = {0x26, 0x01, 0xc0, 0x00};
    /* first slice, ue(3) */
    static const uint8_t trail[] = {0x02, 0x01, 0x90, 0x00};
    /* not a first slice */
    static const uint8_t not_first[] = {0x02, 0x01, 0x40, 0x00};

    assert(venus_hevc_pps_id(idr, sizeof(idr), true) == 0);
    assert(venus_hevc_pps_id(trail, sizeof(trail), false) == 3);
    assert(venus_hevc_pps_id(not_first, sizeof(not_first), false) == 0);
    assert(venus_hevc_pps_id(NULL, 0, false) == 0);
}

int main(void)
{
    test_parameter_sets();
    test_unsupported_sequences();
    test_reorder_pics();
    test_pps_id();
    printf("hevc-headers ok\n");
    return 0;
}
