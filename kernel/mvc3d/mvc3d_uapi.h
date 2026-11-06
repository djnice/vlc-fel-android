/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/*
 * mvc3d userspace interface (/dev/mvc3d)
 *
 * 3D MVC (Blu-ray 3D) on Amlogic S5 Android: the player decodes the base
 * view with MediaCodec as usual (MediaCodec -> HWC -> video_composer.0 ->
 * video_render.0); a user space process decodes both views in software and
 * composes a frame packed picture per frame. While that process is
 * registered (MVC3D_IOC_ALLOC until close), mvc3d sits between
 * video_composer.0 and video_render.0 and replaces every frame the video
 * composer releases by the picture user space names for it, so timing,
 * A/V sync, pause and seek stay the player's.
 *
 * Usage:
 *   MVC3D_IOC_ALLOC, mmap each buffer (offset = index * map_size)
 *   picture thread:  MVC3D_IOC_DEQUEUE -> write a picture -> keep it
 *                    (MVC3D_IOC_RETURN gives an unused buffer back)
 *   frame thread:    MVC3D_IOC_WAIT_JOB -> pick the picture of that frame
 *                    -> MVC3D_IOC_SHOW (buf, or MVC3D_SHOW_DROP)
 * Jobs come in display order and must be answered in that order; a job not
 * answered within show_timeout_ms is dropped (the display repeats the
 * previous picture).
 *
 * Buffer layout MVC3D_FMT_UYVY: packed 4:2:2, 8 bit, bytes U Y0 V Y1, stride
 * bytes per line (what the S5 VD1 reads as VIDTYPE_VIU_422).
 */
#ifndef _UAPI_MVC3D_H
#define _UAPI_MVC3D_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MVC3D_DEV		"/dev/mvc3d"
/* module version (/sys/module/mvc3d/version) */
#define MVC3D_VERSION		"2026.11.06.1"
/* the S905X4 (SC2) module (mvc3d_sc2.c: /dev/mvc3dhw only, MVC3D_HW_F_DIRECT) */
#define MVC3D_SC2_VERSION "2026.11.06.8-sc2"

#define MVC3D_MAX_BUFS		16
#define MVC3D_FMT_UYVY		0

struct mvc3d_alloc {
	__u32 width;		/* in: picture size, e.g. 1920 x 2205 (1080p frame packing) */
	__u32 height;
	__u32 format;		/* in: MVC3D_FMT_* */
	__u32 count;		/* in: 2..MVC3D_MAX_BUFS */
	__u32 stride;		/* out: bytes per line */
	__u32 size;		/* out: bytes used per buffer */
	__u32 map_size;		/* out: mmap offset step between buffers */
	__u32 reserved;
};

#define MVC3D_JOB_NEW_STREAM	(1 << 0)	/* first job after a start/seek */

struct mvc3d_job {
	__u32 timeout_ms;	/* in: max wait */
	__u32 id;		/* out */
	__u32 flags;		/* out: MVC3D_JOB_* */
	__u32 seq;		/* out: frames taken since the stream (re)started */
	/*
	 * out: the decoder picture behind the video composer's frame (where
	 * the composer hands over a post processed copy, its source): its
	 * output index (omx_index) and timestamps as the decoder set them
	 */
	__u32 src_index;
	__u32 width;		/* out: the upstream picture (base view) */
	__u32 height;
	__u32 src_type;		/* out: vframe type of the upstream frame (diagnostics) */
	__u64 pts_us;		/* out: decoder picture pts_us64 */
	__u64 timestamp;	/* out: decoder picture timestamp */
	__u64 arrival_us;	/* out: video composer pts_us64 (arrival from SurfaceFlinger) */
	__u64 taken_ns;		/* out: CLOCK_MONOTONIC when mvc3d took the frame */
	__u32 reserved[4];
};

#define MVC3D_SHOW_DROP		(-1)	/* nothing for this frame: not shown */

struct mvc3d_show {
	__u32 id;		/* the job */
	__s32 buf;		/* a dequeued buffer holding its picture, or MVC3D_SHOW_DROP */
	__u32 width;		/* picture size within the buffer (0: the allocated size) */
	__u32 height;
};

struct mvc3d_buf {
	__u32 timeout_ms;	/* DEQUEUE in: 0 = do not wait */
	__s32 index;		/* DEQUEUE out, RETURN in */
};

#define MVC3D_IOC_MAGIC		'3'
#define MVC3D_IOC_ALLOC		_IOWR(MVC3D_IOC_MAGIC, 1, struct mvc3d_alloc)
#define MVC3D_IOC_DEQUEUE	_IOWR(MVC3D_IOC_MAGIC, 2, struct mvc3d_buf)
#define MVC3D_IOC_RETURN	_IOW(MVC3D_IOC_MAGIC, 3, struct mvc3d_buf)
#define MVC3D_IOC_WAIT_JOB	_IOWR(MVC3D_IOC_MAGIC, 4, struct mvc3d_job)
#define MVC3D_IOC_SHOW		_IOW(MVC3D_IOC_MAGIC, 5, struct mvc3d_show)
/*
 * the player renders its frames this many microseconds (__u32, <= 500000)
 * before their time; mvc3d holds each picture back by as many whole vsyncs
 * and shows it in the vsync it was meant for
 */
#define MVC3D_IOC_SET_LEAD	_IOW(MVC3D_IOC_MAGIC, 6, __u32)
/*
 * __u32 1: HDMI frame packing wanted (until 0 or close): the 1080p24/25/30
 * and 720p50/60 modes the firmware sets from now on go out frame packed
 * (1920x2205 / 1280x1470); /sys/module/mvc3d/parameters/fp_out tells
 */
#define MVC3D_IOC_SET_FP	_IOW(MVC3D_IOC_MAGIC, 7, __u32)

/*
 * /dev/mvc3dhw - hardware MVC decoding (no software decoder, no MediaCodec)
 *
 * The player feeds the stream (frame_num rewritten, see libmvc3dplay
 * mvcfnr) to the legacy Amlogic MVC decoder (/dev/amstream_vbuf,
 * VFORMAT_H264MVC). While a client has MVC3D_HW_START'ed, the decoder's
 * pictures (both views) come here ("decoder.h264mvc mvc3dhw" in place of the
 * vfm map "default"); mvc3d composes each into one of the client's uvm
 * buffers (/dev/uvm, packed 4:2:2 UYVY, stride = width * 2): frame packed
 * (L, active space, R) or the base view alone (2D), and attaches the vframe
 * of that picture to the buffer as the decoder's. The client queues the
 * buffer into a video tunnel (/dev/videotunnel) at its display time; the
 * window's sideband stream (Amlogic type 0x400, channel = tunnel id) makes
 * the HWC show it through video_composer.0 (-> mvc3d -> video_render.0).
 *
 * Pictures come in display order (MVC3D_HW_WAIT). The decoder's first two
 * pictures after a start are not valid (seq 0 and 1: drop them; they stand for
 * the stream's first two pictures, which never come); its last pictures stay
 * in it at the end of the stream.
 */
#define MVC3D_HW_DEV		"/dev/mvc3dhw"
#define MVC3D_HW_MAX_BUFS	12
#define MVC3D_HW_2D		0
#define MVC3D_HW_FP		1

struct mvc3d_hw_start {
	__u32 width;		/* in: one view (1920x1080 or 1280x720) */
	__u32 height;
	__u32 mode;		/* in: MVC3D_HW_2D / MVC3D_HW_FP */
	__u32 out_height;	/* out: picture height (2D: height; FP: 2 * height + active space) */
	__u32 stride;		/* out: bytes per line (width * 2) */
	__u32 gen;		/* out: pictures of this start have this gen (MVC3D_HW_FLUSH changes it) */
	__u32 reserved[2];
};

struct mvc3d_hw_buf {
	__s32 fd;		/* ADD_BUF in: uvm dma-buf of >= stride * out_height bytes */
	__s32 index;		/* ADD_BUF out, RELEASE in */
};

struct mvc3d_hw_pic {
	__u32 timeout_ms;	/* in */
	__s32 index;		/* out: the buffer holding the picture (client owns it now) */
	__u32 seq;		/* out: decoder picture number since the decoder started */
	__u32 gen;		/* out: MVC3D_HW_FLUSH count */
	__u64 decoded_ns;	/* out: CLOCK_MONOTONIC when composed */
	__u32 reserved[2];
};

#define MVC3D_HW_START		_IOWR(MVC3D_IOC_MAGIC, 0x20, struct mvc3d_hw_start)
#define MVC3D_HW_ADD_BUF	_IOWR(MVC3D_IOC_MAGIC, 0x21, struct mvc3d_hw_buf)
#define MVC3D_HW_WAIT		_IOWR(MVC3D_IOC_MAGIC, 0x22, struct mvc3d_hw_pic)
/* the buffer is back from the display (video tunnel) or dropped */
#define MVC3D_HW_RELEASE	_IOW(MVC3D_IOC_MAGIC, 0x23, struct mvc3d_hw_buf)
/* seek: pictures not yet taken are dropped; out: the gen of the pictures from now on */
#define MVC3D_HW_FLUSH		_IOR(MVC3D_IOC_MAGIC, 0x24, __u32)
/* __u32 as MVC3D_IOC_SET_FP */
#define MVC3D_HW_SET_FP		_IOW(MVC3D_IOC_MAGIC, 0x25, __u32)

/*
 * S905X4 (SC2) module (mvc3d_sc2.c): MVC3D_HW_START sets reserved[0] (zero it before the call) to
 * MVC3D_HW_F_DIRECT. Then there are no client buffers (no MVC3D_HW_ADD_BUF, no uvm, no video tunnel):
 * the decoder's own pictures go to the display (vfm "decoder.h264mvc mvc3dhw amvideo", the legacy video
 * layer; the VPP shows the base view, or with frame packing both views itself). MVC3D_HW_WAIT gives a
 * picture slot (index); the client passes it on with MVC3D_HW_PRESENT at its display time (the slot is
 * shown from the first vsync near due_ns and goes back to the decoder by itself) or drops it with
 * MVC3D_HW_RELEASE. The window needs the sideband stream of type 2 (OMX, 0x20), channel 0.
 */
#define MVC3D_HW_F_DIRECT	1

struct mvc3d_hw_present {
	__s32 index;		/* in: a slot from MVC3D_HW_WAIT */
	__u32 reserved;
	__u64 due_ns;		/* in: CLOCK_MONOTONIC display time */
};

#define MVC3D_HW_PRESENT	_IOW(MVC3D_IOC_MAGIC, 0x26, struct mvc3d_hw_present)

#endif
