/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MVC frame_num rewriter for the Amlogic h264 MVC firmware (amvdec_h264mvc).
 *
 * The firmware corrupts the dependent view when frame_num wraps around MaxFrameNum (Blu-ray MVC uses
 * log2_max_frame_num 10: a wrap about every 1024 reference pictures, blocky right view, no error).
 * Every SPS / subset SPS is rewritten to 16 bit frame_num and every slice header (NAL 1, 5, 20) gets a
 * continuous per view counter; the decoded pictures stay bit-identical. Shortly before the 16 bit
 * counter would wrap (the firmware corrupts that too and stalls on MMCO 5), the next I picture and its
 * anchor are rewritten to an IDR and the leading pictures of that open GOP are dropped.
 *
 * C port of CoreELEC xbmc acef2818e8 (CMVCFrameNumRewriter, Portisch, GPL-2.0-or-later) with one fix:
 * an inserted idr_pic_id goes after field_pic_flag / bottom_field_flag (interlaced MVC).
 */
#ifndef MVCFNR_H
#define MVCFNR_H

#include <stddef.h>
#include <stdint.h>

#define MVCFNR_LOG2_MAX_FRAME_NUM 16
#define MVCFNR_IDR_RESET_THRESHOLD ((1u << MVCFNR_LOG2_MAX_FRAME_NUM) - 4096)

struct mvcfnr_buf {
	uint8_t *p;
	size_t len, cap;
};

struct mvcfnr_sps {
	uint8_t valid, log2_max_frame_num, poc_type, log2_max_poc_lsb;
	uint8_t delta_pic_order_always_zero, frame_mbs_only, separate_colour_plane, chroma_format_idc;
};

struct mvcfnr_pps {
	uint8_t valid, sps_id, cabac, bottom_field_pic_order, slice_groups;
	uint8_t num_ref_idx_l0, num_ref_idx_l1, weighted_pred, weighted_bipred_idc;
	uint8_t deblocking_filter_control, redundant_pic_cnt;
};

struct mvcfnr_view {
	int valid;
	uint32_t last_frame_num, high, base;
};

struct mvcfnr {
	struct mvcfnr_sps sps[32], subset_sps[32];
	struct mvcfnr_pps pps[256];
	struct mvcfnr_view view[2];
	int convert_to_idr, drop_leading, drop_picture;
	uint32_t idr_poc_lsb;
	uint32_t idr_threshold; /* MVCFNR_IDR_RESET_THRESHOLD (smaller only for tests) */
	/* statistics */
	unsigned errors, idrs_inserted, aus_dropped, nals_rewritten;
	/* work buffers */
	struct mvcfnr_buf out, rbsp, w, nal;
};

void mvcfnr_init(struct mvcfnr *f);
void mvcfnr_free(struct mvcfnr *f);
/* frame_num unwrap state only (after a flush / seek); parameter sets are kept */
void mvcfnr_reset(struct mvcfnr *f);

enum { MVCFNR_UNCHANGED = 0, MVCFNR_REWRITTEN = 1, MVCFNR_DROP = 2 };
/*
 * One Annex-B buffer (an access unit, or the codec header). MVCFNR_REWRITTEN: use *out / *out_len
 * (valid until the next call); MVCFNR_DROP: leading picture after an inserted IDR, do not decode it.
 */
int mvcfnr_process(struct mvcfnr *f, const uint8_t *data, size_t size, const uint8_t **out,
		   size_t *out_len);

#endif
