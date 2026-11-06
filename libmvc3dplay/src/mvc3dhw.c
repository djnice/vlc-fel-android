// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3dplay hardware path: the legacy Amlogic MVC decoder + mvc3d (/dev/mvc3dhw) + a video tunnel,
 * no software decoder and no MediaCodec. See mvc3dplay.h (mvc3d_hw_*) and the kernel side in
 * mvc3d_uapi.h.
 *
 *   access units -> feeder thread: frame_num rewrite (mvcfnr) -> /dev/amstream_vbuf (VFORMAT_H264MVC)
 *   decoder -> mvc3d: composed into our uvm buffers -> mvc3d_hw_get_picture (pts assigned here:
 *   pictures come in display order, each takes the smallest pts not given out yet)
 *   mvc3d_hw_present -> /dev/videotunnel at the display time -> HWC -> video_composer.0 -> display
 *   return thread: buffers back from the tunnel (after their release fence) -> MVC3D_HW_RELEASE
 */
#include "mvc3dplay.h"
#include "mvcfnr.h"
#include "mvc3d_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "mvc3dhw", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "mvc3dhw", __VA_ARGS__)
#else
#define LOGI(...) (fprintf(stderr, "mvc3dhw: " __VA_ARGS__), fputc('\n', stderr))
#define LOGE LOGI
#endif

/* ---- amstream (legacy decoder input) ---- */
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

/* ---- uvm buffers ---- */
struct uvm_alloc_data {
	int size, align;
	unsigned int flags;
	int v4l2_fd, fd, byte_stride;
	uint32_t width, height;
	int scalar, scaled_buf_size;
};
#define UVM_IOC_ALLOC _IOWR('U', 0, struct uvm_alloc_data)
#define MUA_IMM_ALLOC 1

/* ---- video tunnel (producer) ---- */
enum { VT_ROLE_PRODUCER, VT_ROLE_CONSUMER };
enum { VT_CTRL_CONNECT, VT_CTRL_DISCONNECT, VT_CTRL_SEND_CMD, VT_CTRL_RECV_CMD, VT_CTRL_SET_BLOCK_MODE,
       VT_CTRL_SET_NONBLOCK_MODE };
struct vt_alloc_id_data { int tunnel_id; };
struct vt_krect { int left, top, right, bottom; };
struct vt_ctrl_data {
	int tunnel_id, role, ctrl_cmd, video_cmd, video_cmd_data, client_id;
	struct vt_krect rect;
};
struct vt_buffer_data {
	int tunnel_id, buffer_fd, fence_fd, buffer_status;
	int64_t time_stamp;
};
#define VT_IOC_ALLOC_ID _IOWR('V', 0, struct vt_alloc_id_data)
#define VT_IOC_FREE_ID _IOWR('V', 1, struct vt_alloc_id_data)
#define VT_IOC_CTRL _IOWR('V', 2, struct vt_ctrl_data)
#define VT_IOC_QUEUE_BUFFER _IOWR('V', 3, struct vt_buffer_data)
#define VT_IOC_DEQUEUE_BUFFER _IOWR('V', 4, struct vt_buffer_data)

/* ---- sideband stream on the player's window (Amlogic gralloc layout) ---- */
#define NATIVE_WINDOW_SET_SIDEBAND_STREAM 18
#define AM_SIDEBAND_MAGIC 0xabcdcdef
#define AM_SIDEBAND_TUNNEL 0x400
#define AM_SIDEBAND_OMX 0x20	/* the legacy video layer (amvideo): S905X4 direct mode */

struct anw_priv {
	int magic, version;
	void *reserved[4];
	void (*incRef)(void *), (*decRef)(void *);
	const uint32_t flags;
	const int min_swap, max_swap;
	const float xdpi, ydpi;
	intptr_t oem[4];
	int (*setSwapInterval)(void *, int);
	void *dequeue_dep, *lock_dep, *queue_dep;
	int (*query)(const void *, int, int *);
	int (*perform)(void *, int, ...);
};

struct sb_handle {
	int version, num_fds, num_ints;
	int data[3];
};

#define NBUFS 8
#define PTS_MAX 1024
#define QUEUE_AUS 48
#define INVALID_SEQS 2

struct au {
	struct au *next;
	size_t size;
	int config;
	uint8_t data[];
};

struct mvc3d_hw {
	int w, h, fp, out_h, stride;
	int direct;		/* MVC3D_HW_F_DIRECT (S905X4): the decoder's pictures, no buffers or tunnel */
	int returner_on;
	int hw, amfd, vt, tunnel;
	int buf_fd[NBUFS];
	int nbufs;
	void *window;
	struct sb_handle sb;

	/* feeder */
	pthread_t feeder;
	pthread_mutex_t qm;
	pthread_cond_t qc;
	struct au *q_head, *q_tail;
	size_t q_bytes;
	int q_cnt;
	int quit, flush_req;
	uint8_t *config;
	size_t config_size;
	struct mvcfnr fnr;

	/* pts of the access units fed, smallest first out (pm) */
	pthread_mutex_t pm;
	int64_t pts[PTS_MAX];
	int npts;
	uint32_t kgen;		/* the gen of the current decoder's pictures (mvc3d) */
	int skip;

	/* tunnel return */
	pthread_t returner;
	int on_display[NBUFS];

	/* stats */
	unsigned fed, shown, dropped, late_pts;
};

static int64_t mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int mvc3d_hw_available(void)
{
	/* the video tunnel and uvm (S928X) are checked at the open: the S905X4 module needs neither */
	return !access(MVC3D_HW_DEV, R_OK | W_OK) && !access("/dev/amstream_vbuf", R_OK | W_OK);
}

static int am_open(struct mvc3d_hw *h)
{
	int fd = open("/dev/amstream_vbuf", O_RDWR);
	if (fd < 0) {
		LOGE("open /dev/amstream_vbuf: %s", strerror(errno));
		return -1;
	}
	struct am_ioctl_parm parm = { .data_vformat = VFORMAT_H264MVC, .cmd = AMSTREAM_SET_VFORMAT };
	struct dec_sysinfo si = { .format = VIDEO_DEC_FORMAT_H264, .width = h->w, .height = h->h,
				  .rate = 4004, .param = (void *)EXTERNAL_PTS };
	if (ioctl(fd, AMSTREAM_IOC_SET, &parm) < 0 || ioctl(fd, AMSTREAM_IOC_SYSINFO, &si) < 0 ||
	    ioctl(fd, AMSTREAM_IOC_PORT_INIT, 0) < 0) {
		/* the decoder is in use (another player's MediaCodec): no hardware MVC now */
		LOGE("amstream setup: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int write_all(int fd, const uint8_t *d, size_t n, struct mvc3d_hw *h)
{
	while (n) {
		ssize_t w = write(fd, d, n);
		if (w < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				if (h->quit || h->flush_req)
					return -1;
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

static void *feeder_thread(void *arg)
{
	struct mvc3d_hw *h = arg;
	int need_config = 1;

	for (;;) {
		pthread_mutex_lock(&h->qm);
		while (!h->q_head && !h->quit && !h->flush_req)
			pthread_cond_wait(&h->qc, &h->qm);
		if (h->quit) {
			pthread_mutex_unlock(&h->qm);
			break;
		}
		if (h->flush_req) {
			/* seek: a new decoder, the stream starts again (key picture) */
			while (h->q_head) {
				struct au *a = h->q_head;
				h->q_head = a->next;
				free(a);
			}
			h->q_tail = NULL;
			h->q_bytes = 0;
			h->q_cnt = 0;
			pthread_mutex_unlock(&h->qm);
			if (h->amfd >= 0)
				close(h->amfd);
			uint32_t g = 0;
			if (!ioctl(h->hw, MVC3D_HW_FLUSH, &g)) {
				pthread_mutex_lock(&h->pm);
				h->kgen = g;
				pthread_mutex_unlock(&h->pm);
			}
			h->amfd = am_open(h);
			mvcfnr_reset(&h->fnr);
			need_config = 1;
			pthread_mutex_lock(&h->qm);
			h->flush_req = 0;
			pthread_cond_broadcast(&h->qc);
			pthread_mutex_unlock(&h->qm);
			continue;
		}
		struct au *a = h->q_head;
		h->q_head = a->next;
		if (!h->q_head)
			h->q_tail = NULL;
		h->q_bytes -= a->size;
		h->q_cnt--;
		pthread_cond_broadcast(&h->qc);
		pthread_mutex_unlock(&h->qm);

		if (h->amfd >= 0) {
			if (need_config && !a->config && h->config_size) {
				const uint8_t *o = h->config;
				size_t ol = h->config_size;
				if (mvcfnr_process(&h->fnr, h->config, h->config_size, &o, &ol) == MVCFNR_DROP)
					ol = 0;
				write_all(h->amfd, o, ol, h);
			}
			need_config = 0;
			const uint8_t *o = a->data;
			size_t ol = a->size;
			int r = mvcfnr_process(&h->fnr, a->data, a->size, &o, &ol);
			if (r != MVCFNR_DROP) {
				struct am_ioctl_parm ts = { .data_32 = 0, .cmd = AMSTREAM_SET_TSTAMP };
				ioctl(h->amfd, AMSTREAM_IOC_SET, &ts);
				write_all(h->amfd, o, ol, h);
			}
		}
		free(a);
	}
	return NULL;
}

static void *returner_thread(void *arg)
{
	struct mvc3d_hw *h = arg;

	while (!h->quit) {
		struct vt_buffer_data d = { .tunnel_id = h->tunnel, .fence_fd = -1 };
		if (ioctl(h->vt, VT_IOC_DEQUEUE_BUFFER, &d) < 0) {
			usleep(2000);
			continue;
		}
		if (d.fence_fd >= 0) {
			/* the display reads the buffer until its release fence signals */
			struct pollfd p = { .fd = d.fence_fd, .events = POLLIN };
			poll(&p, 1, 200);
			close(d.fence_fd);
		}
		for (int i = 0; i < h->nbufs; i++) {
			if (h->buf_fd[i] == d.buffer_fd && h->on_display[i]) {
				h->on_display[i] = 0;
				struct mvc3d_hw_buf b = { .index = i };
				ioctl(h->hw, MVC3D_HW_RELEASE, &b);
			}
		}
	}
	return NULL;
}

int mvc3d_hw_set_sideband(mvc3d_hw *h, void *window)
{
	struct anw_priv *p = window;

	if (!window)
		return -1;
	if (p->magic != (('_' << 24) | ('w' << 16) | ('n' << 8) | 'd')) {
		LOGE("not an ANativeWindow (magic 0x%x)", p->magic);
		return -1;
	}
	h->sb.version = sizeof(int) * 3;
	h->sb.num_fds = 0;
	h->sb.num_ints = 3;
	h->sb.data[0] = (int)AM_SIDEBAND_MAGIC;
	h->sb.data[1] = h->direct ? AM_SIDEBAND_OMX : AM_SIDEBAND_TUNNEL;
	h->sb.data[2] = h->direct ? 0 : h->tunnel;
	int r = p->perform(window, NATIVE_WINDOW_SET_SIDEBAND_STREAM, &h->sb);
	if (r) {
		LOGE("sideband stream: %d", r);
		return -1;
	}
	h->window = window;
	return 0;
}

mvc3d_hw *mvc3d_hw_open(int width, int height, int frame_packed)
{
	struct mvc3d_hw *h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;
	h->hw = h->amfd = h->vt = h->tunnel = -1;
	for (int i = 0; i < NBUFS; i++)
		h->buf_fd[i] = -1;
	h->w = width;
	h->h = height;
	h->fp = !!frame_packed;
	pthread_mutex_init(&h->qm, NULL);
	pthread_cond_init(&h->qc, NULL);
	pthread_mutex_init(&h->pm, NULL);
	mvcfnr_init(&h->fnr);
	h->skip = INVALID_SEQS;

	h->hw = open(MVC3D_HW_DEV, O_RDWR | O_CLOEXEC);
	if (h->hw < 0) {
		LOGE("open %s: %s", MVC3D_HW_DEV, strerror(errno));
		goto fail;
	}
	struct mvc3d_hw_start s = { .width = width, .height = height,
				    .mode = h->fp ? MVC3D_HW_FP : MVC3D_HW_2D };
	if (ioctl(h->hw, MVC3D_HW_START, &s) < 0) {
		LOGE("MVC3D_HW_START: %s", strerror(errno));
		goto fail;
	}
	h->out_h = s.out_height;
	h->stride = s.stride;
	h->kgen = s.gen;
	h->direct = !!(s.reserved[0] & MVC3D_HW_F_DIRECT);
	if (h->direct) {
		/* slots of the module, not buffers of ours */
		h->nbufs = MVC3D_HW_MAX_BUFS;
		h->amfd = am_open(h);
		if (h->amfd < 0)
			goto fail;
		if (pthread_create(&h->feeder, NULL, feeder_thread, h))
			goto fail;
		LOGI("open %dx%d %s, direct (decoder pictures to the video layer)", width, height,
		     h->fp ? "frame packed" : "2D");
		return h;
	}

	int uvm = open("/dev/uvm", O_RDWR | O_CLOEXEC);
	if (uvm < 0)
		goto fail;
	for (int i = 0; i < NBUFS; i++) {
		struct uvm_alloc_data ad = { .size = h->stride * h->out_h, .flags = MUA_IMM_ALLOC, .v4l2_fd = -1,
					     .byte_stride = h->stride, .width = width, .height = h->out_h };
		if (ioctl(uvm, UVM_IOC_ALLOC, &ad) < 0) {
			LOGE("UVM_IOC_ALLOC: %s", strerror(errno));
			break;
		}
		struct mvc3d_hw_buf b = { .fd = ad.fd };
		if (ioctl(h->hw, MVC3D_HW_ADD_BUF, &b) < 0) {
			LOGE("MVC3D_HW_ADD_BUF: %s", strerror(errno));
			close(ad.fd);
			break;
		}
		h->buf_fd[b.index] = ad.fd;
		h->nbufs++;
	}
	close(uvm);
	if (h->nbufs < 4)
		goto fail;

	h->vt = open("/dev/videotunnel", O_RDWR | O_CLOEXEC);
	struct vt_alloc_id_data id = { 0 };
	if (h->vt < 0 || ioctl(h->vt, VT_IOC_ALLOC_ID, &id) < 0) {
		LOGE("video tunnel: %s", strerror(errno));
		goto fail;
	}
	h->tunnel = id.tunnel_id;
	struct vt_ctrl_data c = { .tunnel_id = h->tunnel, .role = VT_ROLE_PRODUCER, .ctrl_cmd = VT_CTRL_CONNECT };
	struct vt_ctrl_data nb = { .tunnel_id = h->tunnel, .role = VT_ROLE_PRODUCER,
				   .ctrl_cmd = VT_CTRL_SET_NONBLOCK_MODE };
	if (ioctl(h->vt, VT_IOC_CTRL, &c) < 0 || ioctl(h->vt, VT_IOC_CTRL, &nb) < 0) {
		LOGE("video tunnel connect: %s", strerror(errno));
		goto fail;
	}

	h->amfd = am_open(h);
	if (h->amfd < 0)
		goto fail;
	if (pthread_create(&h->feeder, NULL, feeder_thread, h))
		goto fail;
	if (pthread_create(&h->returner, NULL, returner_thread, h)) {
		h->quit = 1;
		pthread_cond_broadcast(&h->qc);
		pthread_join(h->feeder, NULL);
		goto fail;
	}
	h->returner_on = 1;
	if (h->fp) {
		/* the 1080p24 (720p) modes the firmware sets go out frame packed */
		uint32_t on = 1;
		if (ioctl(h->hw, MVC3D_HW_SET_FP, &on) < 0)
			LOGE("frame packing: %s", strerror(errno));
	}
	LOGI("open %dx%d %s, %d buffers %dx%d, tunnel %d", width, height, h->fp ? "frame packed" : "2D",
	     h->nbufs, width, h->out_h, h->tunnel);
	return h;
fail:
	if (h->amfd >= 0)
		close(h->amfd);
	if (h->vt >= 0) {
		if (h->tunnel >= 0) {
			struct vt_alloc_id_data fid = { h->tunnel };
			ioctl(h->vt, VT_IOC_FREE_ID, &fid);
		}
		close(h->vt);
	}
	for (int i = 0; i < NBUFS; i++)
		if (h->buf_fd[i] >= 0)
			close(h->buf_fd[i]);
	if (h->hw >= 0)
		close(h->hw);
	mvcfnr_free(&h->fnr);
	free(h);
	return NULL;
}

int mvc3d_hw_tunnel_id(const mvc3d_hw *h)
{
	return h->tunnel;
}

static void enqueue(struct mvc3d_hw *h, const uint8_t *d, size_t n, int config)
{
	struct au *a = malloc(sizeof(*a) + n);
	if (!a)
		return;
	a->next = NULL;
	a->size = n;
	a->config = config;
	memcpy(a->data, d, n);
	pthread_mutex_lock(&h->qm);
	/* the decoder's input buffer is a few MB: hold the player back like a full MediaCodec */
	while ((h->q_bytes > (8u << 20) || h->q_cnt >= QUEUE_AUS) && !h->quit && !h->flush_req)
		pthread_cond_wait(&h->qc, &h->qm);
	if (h->q_tail)
		h->q_tail->next = a;
	else
		h->q_head = a;
	h->q_tail = a;
	h->q_bytes += n;
	h->q_cnt++;
	pthread_cond_broadcast(&h->qc);
	pthread_mutex_unlock(&h->qm);
}

void mvc3d_hw_config(mvc3d_hw *h, const uint8_t *data, size_t size)
{
	uint8_t *c = malloc(size);
	if (!c)
		return;
	memcpy(c, data, size);
	pthread_mutex_lock(&h->qm);
	free(h->config);
	h->config = c;
	h->config_size = size;
	pthread_mutex_unlock(&h->qm);
	enqueue(h, data, size, 1);
}

void mvc3d_hw_decode(mvc3d_hw *h, const uint8_t *au, size_t size, int64_t pts_us)
{
	pthread_mutex_lock(&h->pm);
	if (h->npts < PTS_MAX) {
		/* sorted ascending */
		int i = h->npts++;
		while (i > 0 && h->pts[i - 1] > pts_us) {
			h->pts[i] = h->pts[i - 1];
			i--;
		}
		h->pts[i] = pts_us;
	} else {
		h->late_pts++;
	}
	pthread_mutex_unlock(&h->pm);
	h->fed++;
	enqueue(h, au, size, 0);
}

int mvc3d_hw_get_picture(mvc3d_hw *h, int timeout_ms, int64_t *pts_us)
{
	for (;;) {
		struct mvc3d_hw_pic p = { .timeout_ms = timeout_ms };
		if (ioctl(h->hw, MVC3D_HW_WAIT, &p) < 0)
			return -1;
		int64_t pts = -1;
		int take = 0;
		pthread_mutex_lock(&h->pm);
		/* a picture of the decoder before a flush takes nothing */
		if (p.gen == h->kgen && h->npts) {
			pts = h->pts[0];
			memmove(h->pts, h->pts + 1, (h->npts - 1) * sizeof(h->pts[0]));
			h->npts--;
			/* the decoder's first pictures are not valid: they stand for the first access units */
			if (h->skip > 0)
				h->skip--;
			else
				take = 1;
		}
		pthread_mutex_unlock(&h->pm);
		if (!take) {
			struct mvc3d_hw_buf b = { .index = p.index };
			ioctl(h->hw, MVC3D_HW_RELEASE, &b);
			h->dropped++;
			continue;
		}
		*pts_us = pts;
		return p.index;
	}
}

int mvc3d_hw_present(mvc3d_hw *h, int index, int64_t mono_ns_at)
{
	if (index < 0 || index >= h->nbufs)
		return -1;
	if (h->direct) {
		struct mvc3d_hw_present q = { .index = index,
					      .due_ns = (uint64_t)(mono_ns_at > 0 ? mono_ns_at : mono_ns()) };
		if (ioctl(h->hw, MVC3D_HW_PRESENT, &q) < 0) {
			mvc3d_hw_discard(h, index);
			return -1;
		}
		h->shown++;
		return 0;
	}
	h->on_display[index] = 1;
	struct vt_buffer_data q = { .tunnel_id = h->tunnel, .buffer_fd = h->buf_fd[index], .fence_fd = -1,
				    .time_stamp = mono_ns_at > 0 ? mono_ns_at : mono_ns() };
	if (ioctl(h->vt, VT_IOC_QUEUE_BUFFER, &q) < 0) {
		h->on_display[index] = 0;
		mvc3d_hw_discard(h, index);
		return -1;
	}
	h->shown++;
	return 0;
}

void mvc3d_hw_discard(mvc3d_hw *h, int index)
{
	struct mvc3d_hw_buf b = { .index = index };
	if (h->direct) {
		if (index >= 0 && index < h->nbufs)
			ioctl(h->hw, MVC3D_HW_RELEASE, &b);
		return;
	}
	if (index >= 0 && index < h->nbufs && !h->on_display[index])
		ioctl(h->hw, MVC3D_HW_RELEASE, &b);
}

void mvc3d_hw_flush(mvc3d_hw *h)
{
	pthread_mutex_lock(&h->qm);
	h->flush_req = 1;
	pthread_cond_broadcast(&h->qc);
	/* the feeder reopens the decoder (new gen); the next access units go to the new one */
	while (h->flush_req && !h->quit)
		pthread_cond_wait(&h->qc, &h->qm);
	pthread_mutex_unlock(&h->qm);
	pthread_mutex_lock(&h->pm);
	h->npts = 0;
	h->skip = INVALID_SEQS;
	pthread_mutex_unlock(&h->pm);
}

int mvc3d_hw_set_fp(mvc3d_hw *h, int on)
{
	uint32_t v = !!on;
	return ioctl(h->hw, MVC3D_HW_SET_FP, &v);
}

void mvc3d_hw_close(mvc3d_hw *h)
{
	if (!h)
		return;
	pthread_mutex_lock(&h->qm);
	h->quit = 1;
	pthread_cond_broadcast(&h->qc);
	pthread_mutex_unlock(&h->qm);
	pthread_join(h->feeder, NULL);
	if (h->returner_on)
		pthread_join(h->returner, NULL);
	if (h->window) {
		struct anw_priv *p = h->window;
		p->perform(h->window, NATIVE_WINDOW_SET_SIDEBAND_STREAM, NULL);
	}
	/* the decoder goes first (its pictures reference nothing of ours), then the tunnel */
	if (h->amfd >= 0)
		close(h->amfd);
	if (h->vt >= 0) {
		struct vt_ctrl_data dc = { .tunnel_id = h->tunnel, .role = VT_ROLE_PRODUCER,
					   .ctrl_cmd = VT_CTRL_DISCONNECT };
		ioctl(h->vt, VT_IOC_CTRL, &dc);
		struct vt_alloc_id_data fid = { h->tunnel };
		ioctl(h->vt, VT_IOC_FREE_ID, &fid);
		close(h->vt);
	}
	LOGI("close: fed %u, shown %u, dropped %u, pts overflow %u, rewriter: %u idrs, %u dropped, %u errors",
	     h->fed, h->shown, h->dropped, h->late_pts, h->fnr.idrs_inserted, h->fnr.aus_dropped, h->fnr.errors);
	close(h->hw);
	for (int i = 0; i < NBUFS; i++)
		if (h->buf_fd[i] >= 0)
			close(h->buf_fd[i]);
	while (h->q_head) {
		struct au *a = h->q_head;
		h->q_head = a->next;
		free(a);
	}
	free(h->config);
	mvcfnr_free(&h->fnr);
	free(h);
}

int mvc3d_hw_wait_input(mvc3d_hw *h, int timeout_ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&h->qm);
	int r = 0;
	while ((h->q_bytes > (8u << 20) || h->q_cnt >= QUEUE_AUS) && !h->quit && !r)
		r = pthread_cond_timedwait(&h->qc, &h->qm, &ts);
	int room = !(h->q_bytes > (8u << 20) || h->q_cnt >= QUEUE_AUS);
	pthread_mutex_unlock(&h->qm);
	return room;
}
