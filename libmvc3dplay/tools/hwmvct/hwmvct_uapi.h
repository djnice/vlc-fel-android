/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
#ifndef HWMVCT_UAPI_H
#define HWMVCT_UAPI_H

#include <linux/types.h>

#define HWMVCT_MAGIC 0x43564d48 /* "HMVC" */
#define HWMVCT_MAX_PLANE (4096 * 2304)
/* read() buffer: header + 6 planes of a 1920x1088 4:2:0 picture pair, with margin */
#define HWMVCT_MAX_REC (16 << 20)

struct hwmvct_plane {
	__u64 phys;
	__u32 width, height, block_mode, size;
};

struct hwmvct_rec {
	__u32 magic, index, starts, error;
	__u32 pts, type, width, height, dirty, canvas0, canvas1, plane_num;
	__u64 pts_us64;
	struct hwmvct_plane plane[6]; /* view 0 Y U V, view 1 Y U V; size 0: absent */
};

#endif
