// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * e264crc in.264
 *
 * Decodes an Annex-B (MVC) stream with edge264 and prints one line per output picture in display
 * order: "n Y U V Ymvc Umvc Vmvc", CRC-32 of the cropped planes (8 bit 4:2:0). The HW side
 * (kernel/hwmvctest) prints the same, so both decoders can be diffed picture by picture.
 */
#include "edge264.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static uint32_t crc_plane(const uint8_t *p, int w, int h, int stride)
{
	uint32_t c = 0xffffffffu;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			c = crc_table[(c ^ p[(size_t)y * stride + x]) & 0xff] ^ (c >> 8);
	return ~c;
}

static long frames;

static void print_frame(const Edge264Frame *f)
{
	printf("%ld %08x %08x %08x", frames++, crc_plane(f->samples[0], f->width_Y, f->height_Y, f->stride_Y),
	       crc_plane(f->samples[1], f->width_C, f->height_C, f->stride_C),
	       crc_plane(f->samples[2], f->width_C, f->height_C, f->stride_C));
	if (f->samples_mvc[0])
		printf(" %08x %08x %08x\n", crc_plane(f->samples_mvc[0], f->width_Y, f->height_Y, f->stride_Y),
		       crc_plane(f->samples_mvc[1], f->width_C, f->height_C, f->stride_C),
		       crc_plane(f->samples_mvc[2], f->width_C, f->height_C, f->stride_C));
	else
		printf(" - - -\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s in.264\n", argv[0]);
		return 2;
	}
	crc_init();
	FILE *in = fopen(argv[1], "rb");
	if (!in) {
		perror(argv[1]);
		return 1;
	}
	fseek(in, 0, SEEK_END);
	size_t n = ftell(in);
	fseek(in, 0, SEEK_SET);
	uint8_t *d = malloc(n + 16);
	if (!d || fread(d, 1, n, in) != n)
		return 1;
	fclose(in);
	const uint8_t *end = d + n;

	Edge264Decoder *dec = edge264_alloc(-1, NULL, NULL, 0, NULL, NULL, NULL);
	if (!dec)
		return 1;
	Edge264Frame f;
	long bad = 0;
	const uint8_t *nal = edge264_find_start_code(d, end, 0);
	nal = nal + 3 < end ? nal + 3 : end;
	for (;;) {
		const uint8_t *next = nal < end ? edge264_find_start_code(nal, end, 0) : end;
		const uint8_t *nal_end = next;
		while (nal_end > nal && nal_end < end && nal_end[-1] == 0)
			nal_end--;
		int res;
		while ((res = edge264_decode_NAL(dec, nal, nal < end ? nal_end : end, NULL, NULL)) == ENOBUFS) {
			if (!edge264_get_frame(dec, &f, 0))
				print_frame(&f);
		}
		if (res == EBADMSG)
			bad++;
		if (nal >= end) {
			while (!edge264_get_frame(dec, &f, 0))
				print_frame(&f);
			if (res == ENODATA)
				break;
			continue;
		}
		while (!edge264_get_frame(dec, &f, 0))
			print_frame(&f);
		nal = next + 3 < end ? next + 3 : end;
	}
	fprintf(stderr, "frames %ld bad_nals %ld\n", frames, bad);
	edge264_free(&dec);
	free(d);
	return 0;
}
