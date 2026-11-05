// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MVC frame_num rewriter, see include/mvcfnr.h.
 * C port of CoreELEC xbmc acef2818e8 MVCFrameNumRewriter.cpp (Copyright (C) 2026 Team CoreELEC).
 */
#include "mvcfnr.h"

#include <stdlib.h>
#include <string.h>

static int buf_reserve(struct mvcfnr_buf *b, size_t n)
{
	if (b->cap >= n)
		return 0;
	size_t cap = b->cap ? b->cap : 4096;
	while (cap < n)
		cap *= 2;
	uint8_t *p = realloc(b->p, cap);
	if (!p)
		return -1;
	b->p = p;
	b->cap = cap;
	return 0;
}

static int buf_append(struct mvcfnr_buf *b, const uint8_t *d, size_t n)
{
	if (buf_reserve(b, b->len + n))
		return -1;
	memcpy(b->p + b->len, d, n);
	b->len += n;
	return 0;
}

static int unescape(const uint8_t *d, size_t n, struct mvcfnr_buf *o)
{
	o->len = 0;
	if (buf_reserve(o, n))
		return -1;
	int zeros = 0;
	for (size_t i = 0; i < n; i++) {
		if (zeros >= 2 && d[i] == 0x03) {
			zeros = 0;
			continue;
		}
		o->p[o->len++] = d[i];
		zeros = d[i] == 0 ? zeros + 1 : 0;
	}
	return 0;
}

/* appends the escaped rbsp to o */
static int escape(const struct mvcfnr_buf *r, struct mvcfnr_buf *o)
{
	if (buf_reserve(o, o->len + r->len + r->len / 2 + 4))
		return -1;
	int zeros = 0;
	for (size_t i = 0; i < r->len; i++) {
		uint8_t b = r->p[i];
		if (zeros >= 2 && b <= 0x03) {
			o->p[o->len++] = 0x03;
			zeros = 0;
		}
		o->p[o->len++] = b;
		zeros = b == 0 ? zeros + 1 : 0;
	}
	return 0;
}

struct br {
	const uint8_t *d;
	size_t size, pos;
	int error;
};

static uint32_t br_u(struct br *r, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++) {
		if (r->pos >= r->size * 8) {
			r->error = 1;
			return 0;
		}
		v = (v << 1) | ((r->d[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
		r->pos++;
	}
	return v;
}

static uint32_t br_ue(struct br *r)
{
	int zeros = 0;
	while (br_u(r, 1) == 0) {
		if (r->error || ++zeros > 31) {
			r->error = 1;
			return 0;
		}
	}
	return ((1u << zeros) - 1) + br_u(r, zeros);
}

static int32_t br_se(struct br *r)
{
	uint32_t k = br_ue(r);
	return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

struct bw {
	struct mvcfnr_buf *b;
	size_t bits;
	int error;
};

static void bw_bit(struct bw *w, int bit)
{
	if ((w->bits & 7) == 0) {
		if (buf_reserve(w->b, w->b->len + 1)) {
			w->error = 1;
			return;
		}
		w->b->p[w->b->len++] = 0;
	}
	if (bit)
		w->b->p[w->b->len - 1] |= 0x80 >> (w->bits & 7);
	w->bits++;
}

static void bw_u(struct bw *w, uint32_t v, int n)
{
	for (int i = n - 1; i >= 0; i--)
		bw_bit(w, (v >> i) & 1);
}

static void bw_ue(struct bw *w, uint32_t v)
{
	uint64_t x = (uint64_t)v + 1;
	int len = 0;
	while ((x >> len) > 1)
		len++;
	bw_u(w, 0, len);
	bw_u(w, (uint32_t)x, len + 1);
}

static void bw_copy(struct bw *w, const uint8_t *src, size_t from, size_t to)
{
	/* byte-wise when both sides are byte aligned (slice data of CAVLC slices is shifted bit-wise) */
	while (from < to && ((from & 7) || (w->bits & 7)))
		bw_bit(w, (src[from >> 3] >> (7 - (from & 7))) & 1), from++;
	if (from < to && to - from >= 8) {
		size_t n = (to - from) / 8;
		if (buf_append(w->b, src + from / 8, n)) {
			w->error = 1;
			return;
		}
		from += n * 8;
		w->bits += n * 8;
	}
	for (; from < to; from++)
		bw_bit(w, (src[from >> 3] >> (7 - (from & 7))) & 1);
}

/* position (in bits) of the last set bit = rbsp_stop_one_bit, or 0 if none */
static size_t stop_bit_pos(const uint8_t *d, size_t n)
{
	for (size_t i = n; i > 0; i--) {
		uint8_t b = d[i - 1];
		if (b) {
			int bit = 7;
			while (!((b >> (7 - bit)) & 1))
				bit--;
			return (i - 1) * 8 + bit;
		}
	}
	return 0;
}

static void skip_scaling_list(struct br *r, int size)
{
	int last = 8, next = 8;
	for (int j = 0; j < size; j++) {
		if (next != 0)
			next = (last + br_se(r) + 256) % 256;
		last = next == 0 ? last : next;
	}
}

void mvcfnr_init(struct mvcfnr *f)
{
	memset(f, 0, sizeof(*f));
	f->idr_threshold = MVCFNR_IDR_RESET_THRESHOLD;
}

void mvcfnr_free(struct mvcfnr *f)
{
	free(f->out.p);
	free(f->rbsp.p);
	free(f->w.p);
	free(f->nal.p);
	memset(f, 0, sizeof(*f));
}

void mvcfnr_reset(struct mvcfnr *f)
{
	memset(f->view, 0, sizeof(f->view));
	f->convert_to_idr = 0;
	f->drop_leading = 0;
	f->drop_picture = 0;
}

/* rewrites f->rbsp into f->w; returns 1 when changed */
static int rewrite_sps(struct mvcfnr *f, int subset)
{
	struct br r = { f->rbsp.p, f->rbsp.len, 8, 0 };
	struct mvcfnr_sps sps = { .valid = 1, .chroma_format_idc = 1 };
	uint32_t profile = br_u(&r, 8);
	br_u(&r, 16); /* constraint flags + level */
	uint32_t id = br_ue(&r);
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
	    profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
	    profile == 139 || profile == 134 || profile == 135) {
		sps.chroma_format_idc = br_ue(&r);
		if (sps.chroma_format_idc == 3)
			sps.separate_colour_plane = br_u(&r, 1);
		br_ue(&r); /* bit_depth_luma */
		br_ue(&r); /* bit_depth_chroma */
		br_u(&r, 1); /* qpprime_y_zero_transform_bypass */
		if (br_u(&r, 1)) { /* seq_scaling_matrix_present */
			for (int i = 0; i < (sps.chroma_format_idc != 3 ? 8 : 12); i++)
				if (br_u(&r, 1))
					skip_scaling_list(&r, i < 6 ? 16 : 64);
		}
	}
	size_t fn_pos = r.pos;
	sps.log2_max_frame_num = br_ue(&r) + 4;
	size_t fn_end = r.pos;
	sps.poc_type = br_ue(&r);
	if (sps.poc_type == 0) {
		sps.log2_max_poc_lsb = br_ue(&r) + 4;
	} else if (sps.poc_type == 1) {
		sps.delta_pic_order_always_zero = br_u(&r, 1);
		br_se(&r);
		br_se(&r);
		uint32_t n = br_ue(&r);
		for (uint32_t i = 0; i < n && !r.error; i++)
			br_se(&r);
	}
	br_ue(&r); /* max_num_ref_frames */
	br_u(&r, 1); /* gaps_in_frame_num_value_allowed */
	br_ue(&r); /* width */
	br_ue(&r); /* height */
	sps.frame_mbs_only = br_u(&r, 1);

	if (r.error || id > 31) {
		f->errors++;
		return 0;
	}
	(subset ? f->subset_sps : f->sps)[id] = sps;
	if (sps.log2_max_frame_num >= MVCFNR_LOG2_MAX_FRAME_NUM)
		return 0;

	struct bw w = { &f->w, 0, 0 };
	f->w.len = 0;
	bw_copy(&w, f->rbsp.p, 0, fn_pos);
	bw_ue(&w, MVCFNR_LOG2_MAX_FRAME_NUM - 4);
	bw_copy(&w, f->rbsp.p, fn_end, stop_bit_pos(f->rbsp.p, f->rbsp.len) + 1);
	while (w.bits & 7)
		bw_bit(&w, 0);
	return !w.error;
}

static void parse_pps(struct mvcfnr *f)
{
	struct br r = { f->rbsp.p, f->rbsp.len, 8, 0 };
	struct mvcfnr_pps pps = { .valid = 1, .num_ref_idx_l0 = 1, .num_ref_idx_l1 = 1 };
	uint32_t id = br_ue(&r);
	pps.sps_id = br_ue(&r);
	pps.cabac = br_u(&r, 1);
	pps.bottom_field_pic_order = br_u(&r, 1);
	pps.slice_groups = br_ue(&r) > 0;
	if (!pps.slice_groups) {
		pps.num_ref_idx_l0 = br_ue(&r) + 1;
		pps.num_ref_idx_l1 = br_ue(&r) + 1;
		pps.weighted_pred = br_u(&r, 1);
		pps.weighted_bipred_idc = br_u(&r, 2);
		br_se(&r); /* pic_init_qp */
		br_se(&r); /* pic_init_qs */
		br_se(&r); /* chroma_qp_index_offset */
		pps.deblocking_filter_control = br_u(&r, 1);
		br_u(&r, 1); /* constrained_intra_pred */
		pps.redundant_pic_cnt = br_u(&r, 1);
	}
	if (r.error || id > 255 || pps.sps_id > 31) {
		f->errors++;
		return;
	}
	f->pps[id] = pps;
}

/* rewrites f->rbsp into f->w; returns 1 when changed (f->w.len == 0: drop the picture) */
static int rewrite_slice(struct mvcfnr *f, int nal_type)
{
	const uint8_t *d = f->rbsp.p;
	const int mvc = nal_type == 20;
	if (f->rbsp.len < (mvc ? 5u : 2u))
		return 0;

	const int nri = (d[0] >> 5) & 3;
	int idr = nal_type == 5;
	if (mvc)
		idr = !((d[1] >> 6) & 1); /* non_idr_flag */

	struct br r = { d, f->rbsp.len, (mvc ? 4 : 1) * 8, 0 };
	uint32_t first_mb = br_ue(&r);
	int slice_type = br_ue(&r) % 5;
	uint32_t pps_id = br_ue(&r);
	if (r.error || pps_id > 255 || !f->pps[pps_id].valid)
		return 0;
	const struct mvcfnr_pps *pps = &f->pps[pps_id];
	const struct mvcfnr_sps *sps = &(mvc ? f->subset_sps : f->sps)[pps->sps_id];
	if (!sps->valid || sps->log2_max_frame_num >= MVCFNR_LOG2_MAX_FRAME_NUM)
		return 0;
	if (pps->slice_groups) {
		/* FMO is not allowed in High / Stereo High profiles */
		f->errors++;
		return 0;
	}

	if (sps->separate_colour_plane)
		br_u(&r, 2);
	size_t fn_pos = r.pos;
	uint32_t frame_num = br_u(&r, sps->log2_max_frame_num);
	size_t fn_end = r.pos;

	/* unwrap frame_num into a continuous counter, per view */
	struct mvcfnr_view *view = &f->view[mvc ? 1 : 0];
	uint32_t max_frame_num = 1u << sps->log2_max_frame_num;
	if (idr || !view->valid) {
		view->high = 0;
		view->base = 0;
	} else if (frame_num < view->last_frame_num &&
		   view->last_frame_num - frame_num > max_frame_num / 2) {
		view->high += max_frame_num;
	}
	view->valid = 1;
	view->last_frame_num = frame_num;
	uint32_t unwrapped = view->high + frame_num;

	/* parse the rest of the slice header to find its end */
	int field_pic = 0;
	if (!sps->frame_mbs_only) {
		field_pic = br_u(&r, 1);
		if (field_pic)
			br_u(&r, 1);
	}
	size_t field_end = r.pos; /* idr_pic_id goes here */
	if (idr)
		br_ue(&r);
	uint32_t poc_lsb = 0;
	if (sps->poc_type == 0) {
		poc_lsb = br_u(&r, sps->log2_max_poc_lsb);
		if (pps->bottom_field_pic_order && !field_pic)
			br_se(&r);
	} else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
		br_se(&r);
		if (pps->bottom_field_pic_order && !field_pic)
			br_se(&r);
	}
	if (pps->redundant_pic_cnt)
		br_ue(&r);
	const int is_p = slice_type == 0 || slice_type == 3;
	const int is_b = slice_type == 1;
	const int is_i = slice_type == 2 || slice_type == 4;
	if (is_b)
		br_u(&r, 1); /* direct_spatial_mv_pred */
	int num_l0 = pps->num_ref_idx_l0, num_l1 = pps->num_ref_idx_l1;
	if ((is_p || is_b) && br_u(&r, 1)) { /* num_ref_idx_active_override */
		num_l0 = br_ue(&r) + 1;
		if (is_b)
			num_l1 = br_ue(&r) + 1;
	}
	/* ref_pic_list_modification / ref_pic_list_mvc_modification (idc 4, 5 also carry one ue) */
	for (int list = 0; list < (is_b ? 2 : (is_i ? 0 : 1)); list++) {
		if (br_u(&r, 1)) {
			for (int n = 0; n < 1000 && !r.error; n++) {
				if (br_ue(&r) == 3)
					break;
				br_ue(&r);
			}
		}
	}
	if ((pps->weighted_pred && is_p) || (pps->weighted_bipred_idc == 1 && is_b)) {
		int chroma_array_type = sps->separate_colour_plane ? 0 : sps->chroma_format_idc;
		br_ue(&r);
		if (chroma_array_type != 0)
			br_ue(&r);
		for (int list = 0; list < (is_b ? 2 : 1); list++) {
			for (int i = 0; i < (list ? num_l1 : num_l0) && !r.error; i++) {
				if (br_u(&r, 1)) {
					br_se(&r);
					br_se(&r);
				}
				if (chroma_array_type != 0 && br_u(&r, 1)) {
					br_se(&r);
					br_se(&r);
					br_se(&r);
					br_se(&r);
				}
			}
		}
	}
	size_t marking_pos = r.pos;
	if (nri) {
		if (idr) {
			br_u(&r, 2);
		} else if (br_u(&r, 1)) { /* adaptive_ref_pic_marking_mode */
			for (int n = 0; n < 1000 && !r.error; n++) {
				uint32_t op = br_ue(&r);
				if (op == 0)
					break;
				if (op == 1 || op == 3)
					br_ue(&r);
				if (op == 2)
					br_ue(&r);
				if (op == 3 || op == 6)
					br_ue(&r);
				if (op == 4)
					br_ue(&r);
			}
		}
	}
	size_t marking_end = r.pos;
	if (pps->cabac && !is_i)
		br_ue(&r); /* cabac_init_idc */
	br_se(&r); /* slice_qp_delta */
	if (slice_type == 3 || slice_type == 4) {
		if (slice_type == 3)
			br_u(&r, 1);
		br_se(&r);
	}
	if (pps->deblocking_filter_control) {
		if (br_ue(&r) != 1) {
			br_se(&r);
			br_se(&r);
		}
	}
	size_t hdr_end = r.pos;
	if (r.error) {
		f->errors++;
		return 0;
	}

	/*
	 * per access unit (first base view slice): insert an IDR before the 16 bit counter wraps, then
	 * drop the leading pictures (POC before the IDR) that reference older pictures
	 */
	if (!mvc && first_mb == 0) {
		f->convert_to_idr = 0;
		f->drop_picture = 0;
		uint32_t max_poc_lsb = 1u << sps->log2_max_poc_lsb;
		if (f->drop_leading) {
			uint32_t diff = (poc_lsb - f->idr_poc_lsb) & (max_poc_lsb - 1);
			if (sps->poc_type == 0 && diff > max_poc_lsb / 2 && !is_i)
				f->drop_picture = 1;
			else
				f->drop_leading = 0;
		}
		if (!f->drop_picture && !idr && is_i && nri && sps->poc_type == 0 &&
		    unwrapped - view->base >= f->idr_threshold) {
			f->convert_to_idr = 1;
			f->drop_leading = 1;
			f->idr_poc_lsb = poc_lsb;
			f->idrs_inserted++;
		}
	}

	f->w.len = 0;
	if (f->drop_picture)
		return 1;

	const int to_idr = f->convert_to_idr && nri && !idr && (!mvc || ((d[3] >> 2) & 1)); /* anchor */
	if (f->convert_to_idr && !to_idr && !idr)
		f->errors++;
	if (to_idr)
		view->base = unwrapped;
	uint32_t new_frame_num = (unwrapped - view->base) & ((1u << MVCFNR_LOG2_MAX_FRAME_NUM) - 1);

	struct bw w = { &f->w, 0, 0 };
	bw_copy(&w, d, 0, fn_pos);
	bw_u(&w, new_frame_num, MVCFNR_LOG2_MAX_FRAME_NUM);
	if (to_idr) {
		bw_copy(&w, d, fn_end, field_end);
		bw_ue(&w, 0); /* idr_pic_id */
		bw_copy(&w, d, field_end, marking_pos);
		bw_u(&w, 0, 2); /* no_output_of_prior_pics_flag, long_term_reference_flag */
		bw_copy(&w, d, marking_end, hdr_end);
	} else {
		bw_copy(&w, d, fn_end, hdr_end);
	}

	if (pps->cabac) {
		/* slice data starts byte aligned after cabac_alignment_one_bits */
		size_t align_end = (hdr_end + 7) & ~(size_t)7;
		for (size_t i = hdr_end; i < align_end; i++) {
			if (!((d[i >> 3] >> (7 - (i & 7))) & 1)) {
				/* header misparsed, leave the slice untouched */
				f->errors++;
				return 0;
			}
		}
		while (w.bits & 7)
			bw_bit(&w, 1);
		if (align_end / 8 < f->rbsp.len && buf_append(&f->w, d + align_end / 8, f->rbsp.len - align_end / 8))
			w.error = 1;
	} else {
		bw_copy(&w, d, hdr_end, stop_bit_pos(d, f->rbsp.len) + 1);
		while (w.bits & 7)
			bw_bit(&w, 0);
	}
	if (w.error || !f->w.len) {
		f->errors++;
		return 0;
	}

	if (to_idr) {
		if (mvc)
			f->w.p[1] &= ~0x40; /* non_idr_flag = 0 */
		else
			f->w.p[0] = (f->w.p[0] & 0xe0) | 5; /* nal_unit_type = IDR */
	}
	return 1;
}

/*
 * one escaped NAL unit without start code: 1 = f->nal holds the replacement (len 0: drop the
 * picture), 0 = keep it
 */
static int process_nal(struct mvcfnr *f, const uint8_t *nal, size_t size)
{
	if (size < 2)
		return 0;
	int type = nal[0] & 0x1f;
	if (type != 7 && type != 15 && type != 8 && type != 1 && type != 5 && type != 20)
		return 0;
	if (unescape(nal, size, &f->rbsp)) {
		f->errors++;
		return 0;
	}
	if (type == 8) {
		parse_pps(f);
		return 0;
	}
	int changed = (type == 7 || type == 15) ? rewrite_sps(f, type == 15) : rewrite_slice(f, type);
	if (!changed)
		return 0;
	f->nal.len = 0;
	if (f->w.len && escape(&f->w, &f->nal)) {
		f->errors++;
		return 0;
	}
	return 1;
}

int mvcfnr_process(struct mvcfnr *f, const uint8_t *data, size_t size, const uint8_t **out,
		   size_t *out_len)
{
	if (!data || size < 4)
		return MVCFNR_UNCHANGED;

	size_t i = 0;
	while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1))
		i++;
	if (i + 3 > size)
		return MVCFNR_UNCHANGED;

	int changed = 0, drop = 0;
	f->out.len = 0;
	if (buf_append(&f->out, data, i + 3)) /* leading zero bytes + first start code */
		return MVCFNR_UNCHANGED;
	size_t begin = i + 3;
	while (begin < size) {
		/* next start code */
		size_t next = begin;
		while (next + 3 <= size && !(data[next] == 0 && data[next + 1] == 0 && data[next + 2] == 1))
			next++;
		size_t end = next + 3 <= size ? next : size;
		size_t payload_end = end;
		/* trailing zero bytes belong to the next 4 byte start code / trailing_zero_8bits */
		while (payload_end > begin && data[payload_end - 1] == 0)
			payload_end--;
		if (payload_end > begin && process_nal(f, data + begin, payload_end - begin)) {
			if (!f->nal.len)
				drop = 1;
			else if (buf_append(&f->out, f->nal.p, f->nal.len))
				return MVCFNR_UNCHANGED;
			changed = 1;
			f->nals_rewritten++;
		} else if (buf_append(&f->out, data + begin, payload_end - begin)) {
			return MVCFNR_UNCHANGED;
		}
		/* zero bytes + next start code verbatim */
		size_t copy_end = next + 3 <= size ? next + 3 : size;
		if (buf_append(&f->out, data + payload_end, copy_end - payload_end))
			return MVCFNR_UNCHANGED;
		begin = copy_end;
	}
	if (drop) {
		f->aus_dropped++;
		return MVCFNR_DROP;
	}
	if (!changed)
		return MVCFNR_UNCHANGED;
	*out = f->out.p;
	*out_len = f->out.len;
	return MVCFNR_REWRITTEN;
}
