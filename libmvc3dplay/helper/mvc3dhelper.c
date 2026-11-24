// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3dhelper - software 3D MVC decoding and frame packing for the mvc3d
 * kernel module (Amlogic S5 Android)
 *
 * A static aarch64 program (the Dune HD / R_volution userspace is 32-bit
 * only, the kernel is arm64): libmvc3dplay in the player starts it and
 * writes every access unit it queues to MediaCodec into its stdin (see
 * mvc3d_proto.h). The helper decodes both views with edge264-mvc, composes
 * frame packed pictures (left view, active space, right view; 8-bit 4:2:2)
 * into the module's buffers ahead of time, and for every frame the video
 * composer releases (a job of the module) names the picture of the same
 * access unit.
 *
 * mvc3dhelper <width> <height> [decoder threads]
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "edge264.h"
#include "mvc3d_uapi.h"
#include "mvc3d_proto.h"

#define NBUFS		12
#define WAIT_MS		100	/* max wait for a picture still being decoded */
#define STALE_AUS	12	/* a picture this many access units behind a job is never asked for (H.264 reorders a few) */

static int out_w, out_h, gap, view_w, view_h;
static int fd = -1;
static struct mvc3d_alloc al;
static uint8_t *map[NBUFS];
static Edge264Decoder *dec;
static volatile int quit;
static int log_next_au;	/* input access units still to log (after a flush) */
static int log_jobs;	/* jobs still to log (after a flush) */

static void logf_(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void abs_deadline(struct timespec *ts, int64_t us)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec += us / 1000000;
	ts->tv_nsec += (us % 1000000) * 1000;
	if (ts->tv_nsec >= 1000000000) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000;
	}
}

/* ------------------------------------------------------------------ */
/* input: access units from the player                                */
/* ------------------------------------------------------------------ */

struct au {
	struct au *next;
	uint32_t type;
	int64_t n, pts;
	size_t size;
	uint8_t data[];
};

static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qc;
static struct au *q_head, *q_tail;
static size_t q_bytes;

static int read_full(int f, void *buf, size_t size)
{
	uint8_t *p = buf;

	while (size) {
		ssize_t r = read(f, p, size);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		size -= r;
	}
	return 0;
}

static void q_push(struct au *a)
{
	pthread_mutex_lock(&qm);
	a->next = NULL;
	if (q_tail)
		q_tail->next = a;
	else
		q_head = a;
	q_tail = a;
	q_bytes += a->size;
	pthread_cond_signal(&qc);
	pthread_mutex_unlock(&qm);
}

static struct au *q_pop(void)
{
	struct au *a;

	pthread_mutex_lock(&qm);
	while (!q_head && !quit)
		pthread_cond_wait(&qc, &qm);
	a = q_head;
	if (a) {
		q_head = a->next;
		if (!q_head)
			q_tail = NULL;
		q_bytes -= a->size;
	}
	pthread_mutex_unlock(&qm);
	return a;
}

static void flush_early(void);

static void *reader_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct m3p_hdr h;
		struct au *a;

		if (read_full(0, &h, sizeof(h)) || h.magic != M3P_MAGIC || h.size > (64u << 20))
			break;
		if (h.type == M3P_LEAD) {
			uint32_t us = h.arg < 0 ? 0 : h.arg > 500000 ? 500000 : (uint32_t)h.arg;

			if (ioctl(fd, MVC3D_IOC_SET_LEAD, &us))
				logf_("SET_LEAD %u: %s", us, strerror(errno));
			continue;
		}
		a = malloc(sizeof(*a) + h.size);
		if (!a)
			break;
		a->type = h.type;
		a->n = h.au;
		a->pts = h.pts_us;
		if (h.type == M3P_AU && log_next_au > 0) {
			log_next_au--;
			logf_("input au %lld pts %lld size %u", (long long)h.au, (long long)h.pts_us, h.size);
		}
		a->size = h.size;
		if (h.size && read_full(0, a->data, h.size)) {
			free(a);
			break;
		}
		if (h.type == M3P_FLUSH)
			flush_early();
		q_push(a);
		if (h.type == M3P_QUIT)
			return NULL;
	}
	/* the player is gone */
	struct au *a = calloc(1, sizeof(*a));

	if (a) {
		a->type = M3P_QUIT;
		q_push(a);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* pictures ready for the display (display order)                     */
/* ------------------------------------------------------------------ */

struct pic {
	int64_t tag;	/* access unit number */
	int buf;
};

static pthread_mutex_t rm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rc;
static struct pic ready[NBUFS];
static int nready;
static unsigned flush_gen;	/* rm: pictures composed before a flush are not queued */

static void buf_return(int buf)
{
	struct mvc3d_buf b = { .index = buf };

	if (ioctl(fd, MVC3D_IOC_RETURN, &b))
		logf_("RETURN %d: %s", buf, strerror(errno));
}

/* rm held */
static void ready_remove(int i, int give_back)
{
	if (give_back)
		buf_return(ready[i].buf);
	memmove(&ready[i], &ready[i + 1], (nready - i - 1) * sizeof(ready[0]));
	nready--;
}

/* ------------------------------------------------------------------ */
/* composition                                                        */
/* ------------------------------------------------------------------ */

struct view {
	const uint8_t *y, *u, *v;
	int w, h, sy, sc;
};

/*
 * one view into UYVY lines at dst; 4:2:0 -> 4:2:2 chroma for luma row r
 * (chroma between rows 2k and 2k+1): 3/4 of chroma row k and 1/4 of row
 * k-1 (even r) or k+1 (odd r)
 */
static void put_view(uint8_t *dst, int stride, const struct view *vw)
{
	int hc = vw->h / 2;
	uint8_t line[8192];

	for (int r = 0; r < vw->h; r++) {
		int k = r >> 1, n = (r & 1) ? k + 1 : k - 1;
		const uint8_t *y = vw->y + r * vw->sy;
		const uint8_t *u0, *u1, *v0, *v1;
		uint8_t *d = line;

		n = n < 0 ? 0 : n >= hc ? hc - 1 : n;
		u0 = vw->u + k * vw->sc;
		u1 = vw->u + n * vw->sc;
		v0 = vw->v + k * vw->sc;
		v1 = vw->v + n * vw->sc;
		for (int x = 0; x < vw->w / 2; x++, d += 4) {
			d[0] = (3 * u0[x] + u1[x] + 2) >> 2;
			d[1] = y[2 * x];
			d[2] = (3 * v0[x] + v1[x] + 2) >> 2;
			d[3] = y[2 * x + 1];
		}
		/* one sequential write per line into the write-combined buffer */
		memcpy(dst + (size_t)r * stride, line, vw->w * 2);
	}
}

static void black(uint8_t *dst, int stride, int w, int h)
{
	for (int r = 0; r < h; r++) {
		uint8_t *d = dst + (size_t)r * stride;

		for (int x = 0; x < w / 2; x++, d += 4) {
			d[0] = d[2] = 128;
			d[1] = d[3] = 16;
		}
	}
}

struct compose_job {
	uint8_t *dst;
	int stride;
	const struct view *vw;
};

static void *compose_thread(void *arg)
{
	struct compose_job *j = arg;

	put_view(j->dst, j->stride, j->vw);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* decoding                                                           */
/* ------------------------------------------------------------------ */

static long st_decoded, st_single, st_bad_nal, st_composed_us, st_comp_max_us, st_size;
static long st_shown, st_dropped, st_skipped, st_stale, st_late_found, st_resets;

static int dequeue_buf(unsigned gen)
{
	struct mvc3d_buf b = { .timeout_ms = 100 };

	for (;;) {
		if (!ioctl(fd, MVC3D_IOC_DEQUEUE, &b))
			return b.index;
		if (errno != EAGAIN && errno != EINTR) {
			logf_("DEQUEUE: %s", strerror(errno));
			quit = 1;
			return -1;
		}
		pthread_mutex_lock(&rm);
		int stale = gen != flush_gen;
		pthread_mutex_unlock(&rm);
		/* all buffers wait for the display (e.g. paused): wait too */
		if (quit || stale)
			return -1;
	}
}

static void output_frame(const Edge264Frame *f, unsigned gen)
{
	struct view l = { f->samples[0], f->samples[1], f->samples[2], f->width_Y, f->height_Y,
			  f->stride_Y, f->stride_C };
	struct view r = l;
	int64_t t0;
	int buf;

	st_decoded++;
	if (f->width_Y != view_w || f->height_Y != view_h || f->bit_depth_Y != 8) {
		if (!st_size++)
			logf_("picture %dx%d %d bit, expected %dx%d 8 bit: not shown", f->width_Y,
			      f->height_Y, f->bit_depth_Y, view_w, view_h);
		return;
	}
	if (f->samples_mvc[0]) {
		r.y = f->samples_mvc[0];
		r.u = f->samples_mvc[1];
		r.v = f->samples_mvc[2];
	} else {
		st_single++;	/* no dependent view: the base view twice */
	}
	buf = dequeue_buf(gen);
	if (buf < 0) {
		st_skipped++;
		return;
	}
	t0 = now_us();
	struct compose_job rj = { map[buf] + (size_t)(view_h + gap) * al.stride, (int)al.stride, &r };
	pthread_t ct;
	int helper = !pthread_create(&ct, NULL, compose_thread, &rj);

	put_view(map[buf], al.stride, &l);
	if (helper)
		pthread_join(ct, NULL);
	else
		compose_thread(&rj);
	t0 = now_us() - t0;
	st_composed_us += t0;
	if (t0 > st_comp_max_us)
		st_comp_max_us = t0;

	pthread_mutex_lock(&rm);
	if (gen != flush_gen || nready == NBUFS) {
		pthread_mutex_unlock(&rm);
		buf_return(buf);
		return;
	}
	ready[nready].tag = f->tag;
	ready[nready].buf = buf;
	nready++;
	pthread_cond_broadcast(&rc);
	pthread_mutex_unlock(&rm);
}

static void drain_frames(unsigned gen)
{
	Edge264Frame f;

	while (!quit && !edge264_get_frame(dec, &f, 0))
		output_frame(&f, gen);
}

static void decode_au(struct au *a, unsigned gen)
{
	const uint8_t *end = a->data + a->size;
	const uint8_t *nal = edge264_find_start_code(a->data, end, 0);

	edge264_set_tag(dec, a->n);
	nal = nal + 3 < end ? nal + 3 : end;
	while (nal < end && !quit) {
		const uint8_t *next = edge264_find_start_code(nal, end, 0);
		int res;

		/* trailing zero of a 4 byte start code */
		const uint8_t *nal_end = next;
		while (nal_end > nal && nal_end < end && nal_end[-1] == 0)
			nal_end--;
		int64_t stuck_since = 0;

		while ((res = edge264_decode_NAL(dec, nal, nal_end, NULL, NULL)) == ENOBUFS && !quit) {
			Edge264Frame f;

			if (!edge264_get_frame(dec, &f, 0)) {
				output_frame(&f, gen);
				stuck_since = 0;
				continue;
			}
			/*
			 * no picture to take out: frames still decoding, or (after a
			 * seek into broken references) never: a flush ends the wait,
			 * else the decoder starts over at the next key picture
			 */
			pthread_mutex_lock(&rm);
			int flushed = gen != flush_gen;
			pthread_mutex_unlock(&rm);
			if (flushed)
				return;
			if (!stuck_since) {
				stuck_since = now_us();
			} else if (now_us() - stuck_since > 500000) {
				logf_("no picture out of the decoder for 500 ms (au %lld): reset", (long long)a->n);
				st_resets++;
				edge264_flush(dec);
				return;
			}
			usleep(1000);
		}
		if (res == EBADMSG)
			st_bad_nal++;
		nal = next + 3 < end ? next + 3 : end;
	}
	drain_frames(gen);
}

/*
 * reader thread, at a flush message: the access units before it and the
 * pictures of the old stream are dropped right away (the decoder thread may
 * be waiting for a buffer they hold); the decoder flushes at the message
 */

static void flush_early(void)
{
	logf_("flush (%zu KiB of input dropped)", q_bytes >> 10);
	log_next_au = 3;
	log_jobs = 20;
	pthread_mutex_lock(&qm);
	while (q_head) {
		struct au *a = q_head;

		q_head = a->next;
		free(a);
	}
	q_tail = NULL;
	q_bytes = 0;
	pthread_mutex_unlock(&qm);

	pthread_mutex_lock(&rm);
	flush_gen++;
	while (nready)
		ready_remove(nready - 1, 1);
	pthread_cond_broadcast(&rc);
	pthread_mutex_unlock(&rm);
}

static void flush_all(void)
{
	pthread_mutex_lock(&rm);
	while (nready)
		ready_remove(nready - 1, 1);
	pthread_mutex_unlock(&rm);
	edge264_flush(dec);
}

static void *decoder_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct au *a = q_pop();
		unsigned gen;

		if (!a || a->type == M3P_QUIT) {
			free(a);
			break;
		}
		if (a->type == M3P_FLUSH) {
			flush_all();
		} else if (a->type == M3P_AU) {
			pthread_mutex_lock(&rm);
			gen = flush_gen;
			pthread_mutex_unlock(&rm);
			decode_au(a, gen);
		}
		free(a);
	}
	quit = 1;
	pthread_cond_broadcast(&rc);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* jobs: the picture of every frame the video composer releases       */
/* ------------------------------------------------------------------ */

static int64_t job_au(const struct mvc3d_job *j)
{
	/* the Amlogic codec2 decoder numbers its input buffers in the timestamp's seconds */
	if (j->timestamp % 1000000000ULL)
		return -1;
	return (int64_t)(j->timestamp / 1000000000ULL);
}

static void *job_thread(void *arg)
{
	int logged = 0, stream_logged = 99;

	(void)arg;
	while (!quit) {
		struct mvc3d_job j = { .timeout_ms = 200 };
		struct mvc3d_show s = { 0 };
		struct timespec dl;
		int64_t au;
		int found = -1, waited = 0;

		if (ioctl(fd, MVC3D_IOC_WAIT_JOB, &j)) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			logf_("WAIT_JOB: %s", strerror(errno));
			break;
		}
		au = job_au(&j);
		s.id = j.id;
		s.buf = MVC3D_SHOW_DROP;
		abs_deadline(&dl, WAIT_MS * 1000);
		pthread_mutex_lock(&rm);
		while (au >= 0 && !quit) {
			for (int i = 0; i < nready; i++)
				if (ready[i].tag == au) {
					found = i;
					break;
				}
			if (found >= 0)
				break;
			/* pictures far behind: their frames never came (a mismatch, or dropped) */
			for (int i = nready - 1; i >= 0; i--)
				if (ready[i].tag < au - STALE_AUS) {
					ready_remove(i, 1);
					st_stale++;
				}
			/*
			 * (nearly) all buffers hold pictures this frame is not: the
			 * oldest one goes back, the decoder would wait for a buffer
			 * while the picture asked for is still to decode (after a seek
			 * the player decodes from the key frame before the position,
			 * and those pictures are never shown)
			 */
			if (nready >= NBUFS - 3) {
				int lo = 0;

				for (int i = 1; i < nready; i++)
					if (ready[i].tag < ready[lo].tag)
						lo = i;
				/* not one the next frames may still ask for (B frames: a few back) */
				if (ready[lo].tag < au - 4) {
					ready_remove(lo, 1);
					st_stale++;
					continue;
				}
			}
			if (pthread_cond_timedwait(&rc, &rm, &dl) == ETIMEDOUT)
				break;
			waited = 1;
		}
		if (found >= 0) {
			/* earlier in display order: frames the player or SurfaceFlinger dropped */
			while (found-- > 0) {
				ready_remove(0, 1);
				st_skipped++;
			}
			s.buf = ready[0].buf;
			ready_remove(0, 0);
			st_late_found += waited;
		}
		if (j.flags & MVC3D_JOB_NEW_STREAM)
			stream_logged = 0;
		if (stream_logged < 8 || log_jobs > 0 || (found < 0 && logged < 60)) {
			if (log_jobs > 0)
				log_jobs--;
			char tags[160] = "";
			int o = 0;

			for (int i = 0; i < nready && o < (int)sizeof(tags) - 12; i++)
				o += snprintf(tags + o, sizeof(tags) - o, " %lld", (long long)ready[i].tag);
			logf_("job %u seq %u au %lld flags %#x: %s (ready:%s)", j.id, j.seq, (long long)au,
			      j.flags, s.buf >= 0 ? "shown" : "dropped", tags);
			logged++;
			stream_logged++;
		}
		pthread_mutex_unlock(&rm);

		if (ioctl(fd, MVC3D_IOC_SHOW, &s)) {
			if (s.buf >= 0)
				buf_return(s.buf);
			s.buf = MVC3D_SHOW_DROP;
		}
		if (s.buf >= 0)
			st_shown++;
		else
			st_dropped++;
	}
	quit = 1;
	pthread_cond_broadcast(&qc);
	return NULL;
}

static void *stats_thread(void *arg)
{
	(void)arg;
	while (!quit) {
		for (int i = 0; i < 100 && !quit; i++)
			usleep(100000);
		logf_("stats: decoded %ld (single view %ld, other size %ld), bad NALs %ld, resets %ld, shown %ld, dropped %ld, skipped %ld, stale %ld, waited %ld, compose avg %.1f ms max %.1f ms, input queue %zu KiB",
		      st_decoded, st_single, st_size, st_bad_nal, st_resets, st_shown, st_dropped, st_skipped, st_stale,
		      st_late_found, st_decoded ? st_composed_us / 1000.0 / st_decoded : 0.0,
		      st_comp_max_us / 1000.0, q_bytes >> 10);
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_condattr_t ca;
	pthread_t tr, td, tj, ts;
	int threads = -1, fp = 0;

	if (argc < 3) {
		printf("error usage: %s width height [fp|2d [threads]]\n", argv[0]);
		return 2;
	}
	view_w = atoi(argv[1]);
	view_h = atoi(argv[2]);
	/* fp: HDMI frame packing wanted (the display takes 3D) */
	if (argc > 3)
		fp = !strcmp(argv[3], "fp");
	if (argc > 4)
		threads = atoi(argv[4]);
	/* HDMI 1.4 frame packing: the active space is 45 lines at 1080, 30 at 720 */
	gap = view_h == 720 ? 30 : 45;
	out_w = view_w;
	out_h = 2 * view_h + gap;
	signal(SIGPIPE, SIG_IGN);

	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&rc, &ca);
	pthread_cond_init(&qc, NULL);

	fd = open(MVC3D_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("error %s: %s\n", MVC3D_DEV, strerror(errno));
		return 1;
	}
	al = (struct mvc3d_alloc){ out_w, out_h, MVC3D_FMT_UYVY, NBUFS };
	if (ioctl(fd, MVC3D_IOC_ALLOC, &al)) {
		printf("error MVC3D_IOC_ALLOC %dx%d: %s\n", out_w, out_h, strerror(errno));
		return 1;
	}
	for (int i = 0; i < NBUFS; i++) {
		map[i] = mmap(NULL, al.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			      (off_t)i * al.map_size);
		if (map[i] == MAP_FAILED) {
			printf("error mmap: %s\n", strerror(errno));
			return 1;
		}
		/* the active space stays black */
		black(map[i], al.stride, out_w, out_h);
	}
	if (fp) {
		uint32_t on = 1;

		/*
		 * the frame rate switch of the firmware then sets a frame packed
		 * mode; without (a firmware the module does not know) the player
		 * plays 2D: the frame packed picture would go out squeezed
		 */
		if (ioctl(fd, MVC3D_IOC_SET_FP, &on)) {
			printf("error no HDMI frame packing on this firmware (%s)\n", strerror(errno));
			return 1;
		}
	}
	dec = edge264_alloc(threads, NULL, NULL, 0, NULL, NULL, NULL);
	if (!dec) {
		printf("error edge264_alloc\n");
		return 1;
	}
	logf_("mvc3dhelper: %dx%d frame packed %dx%d, %d buffers of %u KiB", view_w, view_h, out_w,
	      out_h, NBUFS, al.map_size >> 10);
	printf("ready %d %d %u\n", out_w, out_h, al.stride);
	fflush(stdout);

	pthread_create(&tr, NULL, reader_thread, NULL);
	pthread_create(&td, NULL, decoder_thread, NULL);
	pthread_create(&tj, NULL, job_thread, NULL);
	pthread_create(&ts, NULL, stats_thread, NULL);
	pthread_join(td, NULL);
	quit = 1;
	pthread_cond_broadcast(&rc);
	pthread_join(tj, NULL);
	pthread_join(ts, NULL);
	logf_("mvc3dhelper: done, decoded %ld, shown %ld, dropped %ld", st_decoded, st_shown, st_dropped);
	edge264_free(&dec);
	/* closing /dev/mvc3d unroutes the video composer (after this playback) */
	close(fd);
	return 0;
}
