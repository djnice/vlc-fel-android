// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fnrtool [-t idr_threshold] [-v] in.264 [out.264]
 *
 * Runs the MVC frame_num rewriter (src/mvcfnr.c) over an Annex-B MVC elementary stream
 * (ffmpeg -i x.mk3d -map 0:v:0 -c copy -bsf:v h264_mp4toannexb x.264 keeps NAL 15/20), access unit
 * by access unit, writes the rewritten stream and prints where the original frame_num wrapped.
 */
#include "mvcfnr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t find_sc(const uint8_t *d, size_t i, size_t n)
{
	while (i + 3 <= n && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1))
		i++;
	return i + 3 <= n ? i : n;
}

int main(int argc, char **argv)
{
	unsigned threshold = 0;
	int verbose = 0, a = 1;
	for (; a < argc && argv[a][0] == '-'; a++) {
		if (!strcmp(argv[a], "-t") && a + 1 < argc)
			threshold = strtoul(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "-v"))
			verbose = 1;
	}
	if (a >= argc) {
		fprintf(stderr, "usage: %s [-t idr_threshold] [-v] in.264 [out.264]\n", argv[0]);
		return 2;
	}
	FILE *in = fopen(argv[a], "rb");
	if (!in) {
		perror(argv[a]);
		return 1;
	}
	fseek(in, 0, SEEK_END);
	size_t n = ftell(in);
	fseek(in, 0, SEEK_SET);
	uint8_t *d = malloc(n);
	if (!d || fread(d, 1, n, in) != n)
		return 1;
	fclose(in);
	FILE *out = a + 1 < argc ? fopen(argv[a + 1], "wb") : NULL;

	struct mvcfnr f;
	mvcfnr_init(&f);
	if (threshold)
		f.idr_threshold = threshold;

	/*
	 * access units: a new one starts after a VCL NAL at an AUD, SPS, prefix NAL or base slice with
	 * first_mb 0; SEI / PPS / subset SPS only after a dependent view slice (after a base slice they
	 * belong to the dependent view of the same access unit)
	 */
	size_t au = find_sc(d, 0, n), aus = 0, dropped_bytes = 0;
	uint32_t high[2] = { 0, 0 };
	while (au < n) {
		size_t p = au, end = n;
		int vcl = 0;
		while (p < n) {
			size_t q = find_sc(d, p + 3, n);
			if (p + 3 < n) {
				int t = d[p + 3] & 0x1f;
				int starts = t == 9 || t == 7 || t == 14 ||
					     ((t == 1 || t == 5) && p + 4 < n && (d[p + 4] & 0x80)) ||
					     ((t == 6 || t == 8 || t == 15) && vcl == 20);
				if (vcl && starts) {
					end = p;
					break;
				}
				if (t == 1 || t == 5 || t == 20)
					vcl = t;
			}
			p = q;
		}
		/* the 4 byte start code zero of the next AU stays with it */
		size_t au_end = end;
		while (au_end > au && end < n && d[au_end - 1] == 0)
			au_end--;
		const uint8_t *o = d + au;
		size_t ol = au_end - au;
		int r = mvcfnr_process(&f, d + au, au_end - au, &o, &ol);
		if (r == MVCFNR_DROP) {
			dropped_bytes += au_end - au;
			if (verbose)
				printf("au %zu dropped (leading picture)\n", aus);
		} else if (out) {
			fwrite(o, 1, ol, out);
		}
		for (int v = 0; v < 2; v++) {
			if (f.view[v].high != high[v]) {
				if (f.view[v].high > high[v])
					printf("view %d frame_num wrap at au %zu (%.1f s @23.976)\n", v, aus,
					       aus / 23.976);
				high[v] = f.view[v].high;
			}
		}
		if (out && au_end < end)
			fwrite(d + au_end, 1, end - au_end, out);
		aus++;
		au = end;
	}
	printf("aus %zu nals_rewritten %u idrs_inserted %u aus_dropped %u (%zu bytes) errors %u\n", aus,
	       f.nals_rewritten, f.idrs_inserted, f.aus_dropped, dropped_bytes, f.errors);
	for (int i = 0; i < 32; i++) {
		if (f.sps[i].valid)
			printf("sps %d log2_max_frame_num %d poc_type %d\n", i, f.sps[i].log2_max_frame_num,
			       f.sps[i].poc_type);
		if (f.subset_sps[i].valid)
			printf("subset_sps %d log2_max_frame_num %d poc_type %d\n", i,
			       f.subset_sps[i].log2_max_frame_num, f.subset_sps[i].poc_type);
	}
	if (out)
		fclose(out);
	mvcfnr_free(&f);
	free(d);
	return 0;
}
