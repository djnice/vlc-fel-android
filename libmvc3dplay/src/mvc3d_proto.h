/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libmvc3dplay <-> mvc3dhelper pipe protocol (the helper's stdin): a header,
 * then size bytes of payload. The helper answers on stdout with one text
 * line, "ready <width> <height> <stride>" or "error <text>"; its stderr lines
 * go to the Android log.
 */
#ifndef MVC3D_PROTO_H
#define MVC3D_PROTO_H

#include <stdint.h>

#define M3P_MAGIC	0x5044334dU	/* "M3DP" */

enum m3p_type {
	M3P_AU = 1,		/* an Annex-B access unit as queued to MediaCodec; au: its number */
	M3P_FLUSH = 2,		/* seek: drop what is decoded, the next AU starts a new stream */
	M3P_LEAD = 3,		/* arg: the player's lead time (us) */
	M3P_QUIT = 4,
};

struct m3p_hdr {
	uint32_t magic;
	uint32_t type;
	uint32_t size;		/* payload bytes */
	uint32_t reserved;
	int64_t pts_us;
	/*
	 * the access unit's number: the MediaCodec input buffers (codec config
	 * excluded) queued before it since the decoder started; the Amlogic
	 * decoder hands it to the display as vframe timestamp = au * 10^9
	 */
	int64_t au;
	int64_t arg;
};

#endif
