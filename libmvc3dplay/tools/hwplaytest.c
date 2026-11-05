// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hwplaytest [-fp] [-seek s] in.264 - plays an Annex-B MVC stream through libmvc3dplay's hardware
 * path (mvc3d_hw_*) the way a player would, without audio: 23.976 fps from the first picture.
 * Prints "tunnel <id>" first; a window must show that tunnel (tools/sbtest: setprop debug.sbtest.type
 * 241; setprop debug.sbtest.channel <id>; it starts 5 s later). -seek s: a flush and a jump of s
 * seconds forward every 10 s. Built for armeabi-v7a like the library (tools/build-hwplaytest.sh).
 */
#include "mvc3dplay.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_US 41708

static mvc3d_hw *h;
static volatile int feeding = 1;
static uint8_t *d;
static size_t n;

static int64_t mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static size_t find_sc(size_t i)
{
	while (i + 3 <= n && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1))
		i++;
	return i + 3 <= n ? i : n;
}

/* access units as tools/fnrtool.c splits them */
static size_t au_end(size_t au)
{
	size_t p = au;
	int vcl = 0;
	while (p < n) {
		size_t q = find_sc(p + 3);
		if (p + 3 < n) {
			int t = d[p + 3] & 0x1f;
			int starts = t == 9 || t == 7 || t == 14 || ((t == 1 || t == 5) && p + 4 < n && (d[p + 4] & 0x80)) ||
				     ((t == 6 || t == 8 || t == 15) && vcl == 20);
			if (vcl && starts)
				return p;
			if (t == 1 || t == 5 || t == 20)
				vcl = t;
		}
		p = q;
	}
	return n;
}

static int seek_s;
static volatile int64_t base_ns;
static volatile int64_t base_pts;

static void *feed(void *arg)
{
	size_t au = find_sc(0);
	long idx = 0;
	int64_t t0 = mono_ns();
	while (au < n) {
		size_t end = au_end(au);
		/* a seek: from the next IDR / I picture s seconds on (approximately: by access units) */
		if (seek_s && mono_ns() - t0 > 10000000000LL) {
			t0 = mono_ns();
			long target = idx + seek_s * 24;
			size_t a2 = end;
			long i2 = idx + 1;
			while (a2 < n && (i2 < target || !((d[a2 + 3] & 0x1f) == 7 || (d[a2 + 3] & 0x1f) == 9))) {
				a2 = au_end(a2);
				i2++;
			}
			if (a2 >= n)
				break;
			fprintf(stderr, "seek: au %ld -> %ld\n", idx, i2);
			mvc3d_hw_flush(h);
			base_ns = 0;
			au = a2;
			idx = i2;
			continue;
		}
		mvc3d_hw_decode(h, d + au, end - au, (int64_t)idx * FRAME_US);
		au = end;
		idx++;
	}
	feeding = 0;
	return NULL;
}

int main(int argc, char **argv)
{
	int fp = 0, a = 1;
	for (; a < argc && argv[a][0] == '-'; a++) {
		if (!strcmp(argv[a], "-fp"))
			fp = 1;
		else if (!strcmp(argv[a], "-seek") && a + 1 < argc)
			seek_s = atoi(argv[++a]);
	}
	if (a >= argc) {
		fprintf(stderr, "usage: %s [-fp] [-seek s] in.264\n", argv[0]);
		return 2;
	}
	FILE *in = fopen(argv[a], "rb");
	if (!in)
		return 1;
	fseek(in, 0, SEEK_END);
	n = ftell(in);
	fseek(in, 0, SEEK_SET);
	d = malloc(n);
	if (!d || fread(d, 1, n, in) != n)
		return 1;
	fclose(in);

	fprintf(stderr, "available %d\n", mvc3d_hw_available());
	h = mvc3d_hw_open(1920, 1080, fp);
	if (!h) {
		fprintf(stderr, "mvc3d_hw_open failed\n");
		return 1;
	}
	printf("tunnel %d\n", mvc3d_hw_tunnel_id(h));
	fflush(stdout);
	sleep(5);
	pthread_t th;
	pthread_create(&th, NULL, feed, NULL);

	long shown = 0, late = 0;
	int idle = 0;
	while (idle < 30) {
		int64_t pts;
		int i = mvc3d_hw_get_picture(h, 100, &pts);
		if (i < 0) {
			if (!feeding)
				idle++;
			continue;
		}
		idle = 0;
		int64_t now = mono_ns();
		if (!base_ns) {
			/* the clock starts 100 ms after the first picture */
			base_ns = now + 100000000;
			base_pts = pts;
		}
		int64_t at = base_ns + (pts - base_pts) * 1000;
		/* the player hands a picture over shortly before its time */
		if (at - now > 30000000)
			usleep((at - now - 30000000) / 1000);
		if (at < mono_ns()) {
			late++;
			mvc3d_hw_discard(h, i);
			continue;
		}
		mvc3d_hw_present(h, i, at);
		shown++;
	}
	pthread_join(th, NULL);
	fprintf(stderr, "shown %ld late %ld\n", shown, late);
	mvc3d_hw_close(h);
	free(d);
	return 0;
}
