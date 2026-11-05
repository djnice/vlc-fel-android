// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hwmvctest [-r] [-t idr_threshold] [-l layout] [-D index -o dump.raw] in.264 > out.crc
 *
 * Box side of the HW MVC check (static aarch64, run as root): feeds an Annex-B MVC elementary stream
 * (see tools/fnrtool.c) access unit by access unit to the legacy Amlogic MVC decoder
 * (/dev/amstream_vbuf, VFORMAT_H264MVC), optionally through the frame_num rewriter (-r), and prints
 * one CRC line per decoded picture pair from the hwmvct test receiver (/dev/hwmvct, tools/hwmvct),
 * in the format of tools/e264crc ("n Y U V Ymvc Umvc Vmvc", cropped 8 bit 4:2:0), so the HW output
 * can be diffed against edge264. -l: plane layout (0 linear, 1 32x32 blocks, default 1).
 * -n: feed only (no receiver: the decoder output goes where the vfm map says, e.g. amvideo).
 * -D/-o: write the raw record of picture <index> (struct hwmvct_rec + planes) for layout analysis.
 */
#include "mvcfnr.h"
#include "hwmvct_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define AMSTREAM_IOC_MAGIC 'S'
#define AMSTREAM_IOC_SYSINFO _IOW((AMSTREAM_IOC_MAGIC), 0x0a, int)
#define AMSTREAM_IOC_PORT_INIT _IO((AMSTREAM_IOC_MAGIC), 0x11)
#define AMSTREAM_IOC_SET _IOW((AMSTREAM_IOC_MAGIC), 0xc2, struct am_ioctl_parm)
#define AMSTREAM_SET_VFORMAT 0x105
#define AMSTREAM_SET_TSTAMP 0x10E
#define VFORMAT_H264MVC 9
#define VIDEO_DEC_FORMAT_H264 4
#define EXTERNAL_PTS 0x01

struct am_ioctl_parm {
	union {
		uint32_t data_32;
		uint64_t data_64;
		int data_vformat;
		char data[8];
	};
	uint32_t cmd;
	char reserved[4];
};

struct dec_sysinfo {
	unsigned int format, width, height, rate, extra, status, ratio;
	void *param;
	unsigned long long ratio64;
};

static uint32_t crc_table[256];
static int layout = 1, dump_index = -1, width = 1920, height = 1080;
static const char *dump_path;
static volatile int feeding_done;
static long pictures;

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static inline size_t offset_of(int x, int y, int cw)
{
	if (layout == 0)
		return (size_t)y * cw + x;
	/* 32x32 blocks, row-major, linear inside a block, bytes of each 64 bit word reversed (S5, verified) */
	return (((size_t)(y >> 5) * (cw >> 5) + (x >> 5)) * 1024 + ((y & 31) << 5) + (x & 31)) ^ 7;
}

static uint32_t crc_plane(const uint8_t *p, const struct hwmvct_plane *pl, int w, int h)
{
	uint32_t c = 0xffffffffu;
	if (!pl->size || (int)pl->width < w || (int)pl->height < h)
		return 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			c = crc_table[(c ^ p[offset_of(x, y, pl->width)]) & 0xff] ^ (c >> 8);
	return ~c;
}

static void *reader(void *arg)
{
	int fd = *(int *)arg;
	uint8_t *buf = malloc(HWMVCT_MAX_REC);
	int idle = 0;

	while (buf) {
		ssize_t n = read(fd, buf, HWMVCT_MAX_REC);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read /dev/hwmvct");
			break;
		}
		if (n == 0) {
			/* 3 s without a picture: the end once everything is fed */
			if (feeding_done && ++idle >= 2)
				break;
			continue;
		}
		idle = 0;
		const struct hwmvct_rec *r = (const void *)buf;
		const uint8_t *p[6];
		size_t off = sizeof(*r);
		for (int i = 0; i < 6; i++) {
			p[i] = buf + off;
			off += r->plane[i].size;
		}
		if ((int)r->index == dump_index && dump_path) {
			FILE *f = fopen(dump_path, "wb");
			if (f) {
				fwrite(buf, 1, n, f);
				fclose(f);
			}
		}
		if (r->error || r->dirty)
			fprintf(stderr, "picture %u: error %u dirty %u type 0x%x\n", r->index, r->error, r->dirty,
				r->type);
		if (r->index == 0)
			fprintf(stderr, "first picture: %ux%u type 0x%x planes %u: %ux%u bm %u, %ux%u, %ux%u | %ux%u\n",
				r->width, r->height, r->type, r->plane_num, r->plane[0].width, r->plane[0].height,
				r->plane[0].block_mode, r->plane[1].width, r->plane[1].height, r->plane[2].width,
				r->plane[2].height, r->plane[3].width, r->plane[3].height);
		int cw = (width + 1) / 2, ch = (height + 1) / 2;
		printf("%ld %08x %08x %08x", pictures++, crc_plane(p[0], &r->plane[0], width, height),
		       crc_plane(p[1], &r->plane[1], cw, ch), crc_plane(p[2], &r->plane[2], cw, ch));
		if (r->plane[3].size)
			printf(" %08x %08x %08x", crc_plane(p[3], &r->plane[3], width, height),
			       crc_plane(p[4], &r->plane[4], cw, ch), crc_plane(p[5], &r->plane[5], cw, ch));
		else
			printf(" - - -");
		printf(" pts %u\n", r->pts);
		fflush(stdout);
	}
	free(buf);
	return NULL;
}

static size_t find_sc(const uint8_t *d, size_t i, size_t n)
{
	while (i + 3 <= n && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1))
		i++;
	return i + 3 <= n ? i : n;
}

/* end of the access unit starting at au (same rules as fnrtool) */
static size_t au_end(const uint8_t *d, size_t au, size_t n)
{
	size_t p = au;
	int vcl = 0;
	while (p < n) {
		size_t q = find_sc(d, p + 3, n);
		if (p + 3 < n) {
			int t = d[p + 3] & 0x1f;
			int starts = t == 9 || t == 7 || t == 14 ||
				     ((t == 1 || t == 5) && p + 4 < n && (d[p + 4] & 0x80)) ||
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

static int write_all(int fd, const uint8_t *d, size_t n)
{
	while (n) {
		ssize_t w = write(fd, d, n);
		if (w < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				usleep(2000);
				continue;
			}
			return -1;
		}
		d += w;
		n -= w;
	}
	return 0;
}

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	char msg[160];
	int n = snprintf(msg, sizeof(msg), "signal %d addr %p pc %llx lr %llx\n", sig, si->si_addr,
			 (unsigned long long)uc->uc_mcontext.pc, (unsigned long long)uc->uc_mcontext.regs[30]);
	write(2, msg, n);
	_exit(3);
}

int main(int argc, char **argv)
{
	int rewrite = 0, a = 1, feed_only = 0;
	struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO };
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	unsigned threshold = 0;
	for (; a < argc && argv[a][0] == '-'; a++) {
		if (!strcmp(argv[a], "-r"))
			rewrite = 1;
		else if (!strcmp(argv[a], "-n"))
			feed_only = 1;
		else if (!strcmp(argv[a], "-t") && a + 1 < argc)
			threshold = strtoul(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "-l") && a + 1 < argc)
			layout = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-D") && a + 1 < argc)
			dump_index = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-o") && a + 1 < argc)
			dump_path = argv[++a];
	}
	if (a >= argc) {
		fprintf(stderr, "usage: %s [-r] [-t thr] [-l layout] [-D index -o dump.raw] in.264\n", argv[0]);
		return 2;
	}
	crc_init();
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

	int rfd = feed_only ? -1 : open("/dev/hwmvct", O_RDONLY);
	if (rfd < 0 && !feed_only) {
		perror("/dev/hwmvct");
		return 1;
	}
	int fd = open("/dev/amstream_vbuf", O_RDWR);
	if (fd < 0) {
		perror("/dev/amstream_vbuf");
		return 1;
	}
	struct am_ioctl_parm parm = { .data_vformat = VFORMAT_H264MVC, .cmd = AMSTREAM_SET_VFORMAT };
	if (ioctl(fd, AMSTREAM_IOC_SET, &parm) < 0) {
		perror("AMSTREAM_SET_VFORMAT");
		return 1;
	}
	struct dec_sysinfo si = { .format = VIDEO_DEC_FORMAT_H264, .width = width, .height = height,
				  .rate = 4004, .param = (void *)EXTERNAL_PTS };
	if (ioctl(fd, AMSTREAM_IOC_SYSINFO, &si) < 0) {
		perror("AMSTREAM_IOC_SYSINFO");
		return 1;
	}
	if (ioctl(fd, AMSTREAM_IOC_PORT_INIT, 0) < 0) {
		perror("AMSTREAM_IOC_PORT_INIT");
		return 1;
	}
	pthread_t th;
	if (!feed_only)
		pthread_create(&th, NULL, reader, &rfd);

	struct mvcfnr f;
	mvcfnr_init(&f);
	if (threshold)
		f.idr_threshold = threshold;
	size_t au = find_sc(d, 0, n), aus = 0, dropped = 0;
	while (au < n) {
		size_t end = au_end(d, au, n);
		const uint8_t *o = d + au;
		size_t ol = end - au;
		if (rewrite) {
			int r = mvcfnr_process(&f, d + au, end - au, &o, &ol);
			if (r == MVCFNR_DROP) {
				dropped++;
				aus++;
				au = end;
				continue;
			}
		}
		struct am_ioctl_parm ts = { .data_32 = (uint32_t)(aus * 3754), .cmd = AMSTREAM_SET_TSTAMP };
		if (ioctl(fd, AMSTREAM_IOC_SET, &ts) < 0 && aus == 0)
			perror("AMSTREAM_SET_TSTAMP");
		if (write_all(fd, o, ol) < 0) {
			perror("write /dev/amstream_vbuf");
			break;
		}
		aus++;
		au = end;
	}
	/* end of sequence + end of stream, then padding so the firmware lets the last pictures out */
	static const uint8_t eos[] = { 0, 0, 0, 1, 0x0a, 0, 0, 0, 1, 0x0b };
	write_all(fd, eos, sizeof(eos));
	static uint8_t pad[64 << 10];
	write_all(fd, pad, sizeof(pad));
	feeding_done = 1;
	fprintf(stderr, "fed %zu access units (%zu dropped by the rewriter, errors %u, idrs %u)\n", aus,
		dropped, f.errors, f.idrs_inserted);
	if (feed_only)
		sleep(5); /* the display shows the rest */
	else
		pthread_join(th, NULL);
	fprintf(stderr, "pictures %ld\n", pictures);
	close(fd);
	if (rfd >= 0)
		close(rfd);
	mvcfnr_free(&f);
	free(d);
	return 0;
}
