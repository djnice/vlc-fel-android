// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dvfel (SC2) - Dolby Vision FEL composition by the Dolby Vision core of an
 * Amlogic S905X4 (SC2) on Android firmwares with the 5.4.125 kernel (Dune HD).
 *
 * The SoC composes BL + EL itself when the Dolby Vision driver gets the EL
 * picture of each frame from the "dveldec" provider (vfm: dveldec -> dvel), as
 * in the stream mode dual layer decoder. Android players decode with
 * MediaCodec (frame mode), which outputs the base layer only, so:
 *
 *   - the player reports every base layer access unit it queues (packet pts +
 *     RPU CRC, DVFEL_IOC_SC2_BL_HINT), decodes the EL with a second MediaCodec
 *     into an AImageReader and queues each picture here with the pts of its
 *     frame (DVFEL_IOC_SC2_EL_QUEUE: uvm dma-buf); dvfel registers as
 *     "dveldec" and passes the decoder's own vframe of that picture on;
 *   - kprobes on is_dovi_dual_layer_frame() and dolby_vision_parse_metadata()
 *     see each base layer frame the driver processes: its RPU CRC names its
 *     pts (the earliest pending hint with that CRC, as the S5 dvfel does in
 *     userspace); when the EL picture of that pts is queued, older ones are
 *     dropped, the frame is marked dual layer (src_fmt.dual_layer, which the
 *     driver reads in frame mode) and is_dovi_dual_layer_frame() returns true
 *     (it asks a "dvbldec" provider, which frame mode does not have); the
 *     driver then peeks/gets the head of the dveldec queue, that picture;
 *   - Dolby Vision FEL is switched on in the driver (debug "enable_fel 1");
 *   - the video is muted (black) at the start of a session, from before its
 *     first frame until its EL comes steadily (see start_mute()).
 *
 * The decoder keeps an EL picture's buffer until the display is done with it:
 * each queued picture gets a release fence (sync file) that the player gives
 * to AImage_deleteAsync() at once; it is signalled when the driver puts the
 * picture back (or it is dropped), and the id is reported to the player
 * (DVFEL_IOC_SC2_WAIT_DONE) for its pacing. The reader's images are not held:
 * the decoder allows only 12 output buffers in all.
 *
 * Layout: built against 5.4.125 headers (CoreELEC 7c5e663^1); of struct
 * vframe_s only src_fmt is used, its inner layout as in the Dune aml_media
 * binary. Its offset differs (Dune r24: 1480, headers: 1528), so it is
 * measured at load with the firmware's own clear_vframe_src_fmt(); the base
 * layer frames and the EL vframes are the firmware's, never copied.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/dma-buf.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/file.h>
#include <asm/unaligned.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/amlogic/meson_uvm_core.h>

#include "dvfel_sc2_uapi.h"

#define DRV_NAME	"dvfel"
#define EL_PROV_NAME	"dveldec"
#define DV_DEBUG_PATH	"/sys/class/amdolby_vision/debug"
#define AUX_DV_SEI	0x01000000	/* aux record of the Dolby Vision RPU NAL unit */
#define MAX_EL		DVFEL_SC2_MAX_EL
#define MAX_HINTS	DVFEL_SC2_MAX_HINTS
#define DONE_RING	64
/* pending hints a frame may skip (frames dropped before the display) */
#define HINT_SEARCH	64
/* an EL picture the driver still holds after the session ended: released after this */
#define ORPHAN_MS	1000

/* src_fmt in the struct vframe_s of the Dune r24 SC2 aml_media (measured at load, see below) */
#define SC2_VF_SRC_FMT_OFF	1480
/* diagnostics: the bytes of an EL vframe compared between its queueing and the
 * driver's get (the decoder's own vframe: changed = the buffer reused) */
#define VF_SNAP		256
/* an EL picture on display longer than this is counted */
#define DISP_LONG_MS	200

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "1: session events, 2: every EL picture");

static int early_mute = 2;
module_param(early_mute, int, 0644);
MODULE_PARM_DESC(early_mute, "start mute from 0: the first frame without EL, 1: the session start, "
		 "2: the first base layer access unit (default)");

#define dbg(lvl, fmt, ...) \
	do { if (debug >= (lvl)) pr_info(DRV_NAME ": " fmt, ##__VA_ARGS__); } while (0)

enum el_state { EL_FREE, EL_QUEUED, EL_OUT, EL_RELEASE };

struct el_ent {
	enum el_state state;
	struct dma_buf *db;
	struct vframe_s *vf;
	struct dma_fence *fence;	/* the decoder waits on it before reusing the buffer */
	u32 crc;
	u32 id;
	s64 pts_us;
	unsigned long out_jiffies;
	/* diagnostics */
	u32 sess;			/* the session that queued it */
	unsigned long q_jiffies;
	u8 snap[VF_SNAP];		/* the vframe as queued */
};

struct bl_hint {
	s64 pts_us;
	u32 crc;
};

static struct dvfel_sc2 {
	spinlock_t lock;		/* everything below; taken in the vsync interrupt */
	struct el_ent ent[MAX_EL];
	struct el_ent *q[MAX_EL];	/* queued, in pts (display) order */
	int nq;
	struct bl_hint hint[MAX_HINTS];	/* base layer access units, in pts order */
	int nh;
	u64 fence_ctx;
	u32 fence_seq;
	spinlock_t fence_lock;
	u32 next_id;
	u32 done[DONE_RING];
	u32 done_head, done_tail;
	wait_queue_head_t done_wq;
	struct work_struct rel_work;
	struct delayed_work orphan_work;

	struct mutex sess_lock;		/* session start/stop */
	struct file *owner;
	bool active;			/* pairing on (kprobes act) */
	bool prov_reg;
	struct vframe_provider_s prov;

	struct vframe_s *bl_vf;		/* last base layer frame paired */
	bool bl_found;
	/* the start of a session (start_mute()) */
	bool start_done;		/* the EL came steadily (or the time is up) */
	bool muted;			/* video muted */
	bool first_frame;		/* the session's first frame came */
	u32 paired_run;			/* frames in a row with their EL */
	unsigned long start_jiffies;	/* the start began (muted early, or the first frame), 0: not yet */
	unsigned long mute_jiffies;
	u64 start_muted;		/* times muted at a start */
	struct delayed_work start_work;	/* the end of the start without frames */

	/* stats */
	u64 el_in, el_out, el_put, el_dropped, el_flushed, bl_paired, bl_no_el, bl_no_crc, bl_no_hint;
	u64 hints_in, hints_skipped, hints_overflow;
	/* diagnostics */
	u32 sess;			/* the current session (counts up) */
	u32 mark_id;			/* the EL picture of the last frame marked dual layer */
	u64 el_vf_changed;		/* vframe changed between queueing and get */
	u64 el_get_other;		/* the driver got another EL picture than the marked one */
	u64 el_disp_long;		/* on display longer than DISP_LONG_MS */
	u64 el_orphans;			/* released after the driver kept them past their session */
	u32 q_max_ms, disp_max_ms;	/* longest time queued / on display (this session) */
	bool vf_logged, change_logged;
	struct dvfel_sc2_totals {
		u64 el_in, el_out, el_put, el_dropped, el_flushed, bl_paired, bl_no_el, bl_no_hint;
		u64 hints_in, hints_skipped, vf_changed, get_other, disp_long, orphans;
	} at_start;			/* the counters at the session start */
} g;

static struct kretprobe krp_dual;
static struct kprobe kp_parse;
static bool krp_dual_ok, kp_parse_ok;
static struct dentry *dbg_dir;

/* offset of src_fmt in the firmware's struct vframe_s, measured at load; -1: unknown */
static int src_fmt_off = -1;

static inline struct vframe_src_fmt_s *sc2_src_fmt(struct vframe_s *vf)
{
	return (struct vframe_src_fmt_s *)((u8 *)vf + src_fmt_off);
}

/*
 * A FEL frame without its EL picture shows wrong colours (green with
 * enable_fel on, red without it, as in the firmware's own player): the
 * mapping of the RPU is made for BL + EL. At a start the base layer decoder
 * (the player's) gives its first frames before the EL decoder (here, 0.3 to
 * 4 s: its output buffers come late after its resolution change), and the
 * player shows them at once. The first EL picture often comes alone (from
 * the decoder's buffers before its resolution change), the next ones 1 to 2 s
 * later. So the video is muted (black, the driver's own set_video_mute(),
 * which it uses from the vsync too) during the start of a session: from its
 * first frame without EL until START_STEADY frames in a row had theirs; a
 * frame without EL mutes it again until START_DONE frames in a row had
 * theirs, all of it for at most START_MUTE_MS (no EL comes). The stream mode
 * dual layer decoder (CoreELEC) has no such frames.
 *
 * The driver applies the mute a vsync after it is asked for (its
 * check_video_mute() runs after the frame's Dolby Vision settings, then the
 * core 3 black comes with the next settings): muted at the first frame
 * without EL, that frame is on the screen for a vsync or two. So the mute
 * starts before the first frame (early_mute): at the first base layer access
 * unit the player reports (default; the decoder has it then, the display not
 * yet), or at the session start. It ends as above, or START_MUTE_MS after it
 * began also when no frame reaches the Dolby Vision driver (start_work). Only
 * the start of a session: a seek is not muted.
 */
#define START_STEADY	6
#define START_DONE	48
#define START_MUTE_MS	10000

/* aml_media (video_sink/video.h): sets a flag the vsync reads, callable in any context */
void set_video_mute(bool on);

/* (g.lock held) the start ends START_MUTE_MS after it began, frames or not */
static void start_timeout_arm(void)
{
	unsigned long end = g.start_jiffies + msecs_to_jiffies(START_MUTE_MS);

	schedule_delayed_work(&g.start_work, time_after(end, jiffies) ? end - jiffies : 0);
}

/* (g.lock held) */
static void start_unmute_timeout(void)
{
	g.start_done = true;
	if (g.muted) {
		set_video_mute(false);
		g.muted = false;
		pr_info(DRV_NAME ": session %u: no steady EL for %d ms: unmuted\n", g.sess, START_MUTE_MS);
	}
}

/* (g.lock held) the session's video muted before its first frame */
static void start_mute_early(const char *at)
{
	if (g.start_done || g.muted || g.first_frame)
		return;
	set_video_mute(true);
	g.muted = true;
	g.mute_jiffies = jiffies;
	g.start_jiffies = jiffies ? jiffies : 1;
	g.start_muted++;
	start_timeout_arm();
	pr_info(DRV_NAME ": session %u: muted at the %s\n", g.sess, at);
}

static void start_work_fn(struct work_struct *work)
{
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	if (READ_ONCE(g.active) && !g.start_done && g.start_jiffies &&
	    !time_before(jiffies, g.start_jiffies + msecs_to_jiffies(START_MUTE_MS)))
		start_unmute_timeout();
	spin_unlock_irqrestore(&g.lock, flags);
}

/* (g.lock held) a new base layer frame of the session: with its EL or not */
static void start_mute(bool found)
{
	if (!READ_ONCE(g.active) || g.start_done)
		return;
	if (!g.first_frame) {
		g.first_frame = true;
		if (g.muted)
			pr_info(DRV_NAME ": session %u: first frame %u ms after the mute, %s EL\n", g.sess,
				jiffies_to_msecs(jiffies - g.mute_jiffies), found ? "with" : "without");
	}
	if (!g.start_jiffies)
		g.start_jiffies = jiffies ? jiffies : 1;
	if (time_after(jiffies, g.start_jiffies + msecs_to_jiffies(START_MUTE_MS))) {
		start_unmute_timeout();
		return;
	}
	if (found) {
		g.paired_run++;
		if (g.muted && g.paired_run >= START_STEADY) {
			set_video_mute(false);
			g.muted = false;
			pr_info(DRV_NAME ": session %u: EL steady: unmuted after %u ms\n", g.sess,
				jiffies_to_msecs(jiffies - g.mute_jiffies));
		}
		if (g.paired_run >= START_DONE)
			g.start_done = true;
		return;
	}
	g.paired_run = 0;
	if (!g.muted) {
		set_video_mute(true);
		g.muted = true;
		g.mute_jiffies = jiffies;
		g.start_muted++;
		start_timeout_arm();
	}
}

/* (g.lock held) the end of a session or of the module */
static void unmute_locked(void)
{
	if (g.muted) {
		set_video_mute(false);
		g.muted = false;
	}
}

/*
 * The firmware's own clear_vframe_src_fmt() writes src_fmt (fmt = -1, the
 * pointers and sizes 0, dual_layer 0) only where it finds the SEI magic code
 * (src_fmt + 4). With the magic in every word of a scratch buffer, the word
 * set to -1 is where its src_fmt is. Another layout than the Dune r24 build
 * (other firmwares) is used as measured; none found: no pairing (the frames
 * play as base layer + RPU) instead of writing into a wrong place.
 */
#define SEI_MAGIC	0x53656920	/* "Sei " */
static int measure_src_fmt_off(void)
{
	const int lo = 512, hi = 3072;
	int off = -1, k, n = 0;
	u8 *buf = kzalloc(hi + 256, GFP_KERNEL);

	if (!buf)
		return -1;
	for (k = lo; k < hi; k += 4)
		*(u32 *)(buf + k) = SEI_MAGIC;
	if (clear_vframe_src_fmt((struct vframe_s *)buf) == 0) {
		for (k = lo; k < hi - 24; k += 4) {
			if (*(u32 *)(buf + k) != 0xffffffff)
				continue;
			/* sei_ptr, sei_size cleared; dual_layer (a bool) cleared */
			if (*(u64 *)(buf + k + 8) || *(u32 *)(buf + k + 16) || buf[k + 20])
				continue;
			off = k;
			n++;
		}
	}
	kfree(buf);
	return n == 1 ? off : -1;
}

/* rpu_data_crc32 of the RPU attached to a base layer frame (as dvfel S5) */
static bool rpu_crc_of(struct vframe_s *vf, u32 *crc)
{
	struct vframe_src_fmt_s *sf = sc2_src_fmt(vf);
	const u8 *p = sf->sei_ptr, *end;
	u8 win[5] = { 0 }, last[5] = { 0 };
	u32 size, type, i, n = 0;
	bool have = false;
	int zeros = 0;

	if (!p || sf->sei_size < 8 || sf->sei_size > 64 * 1024)
		return false;
	end = p + sf->sei_size;
	while (end - p >= 8) {
		size = get_unaligned_be32(p);
		type = get_unaligned_be32(p + 4);
		p += 8;
		if (!size || size > end - p)
			return false;
		if (type != AUX_DV_SEI) {
			p += size;
			continue;
		}
		/* EBSP -> RBSP; the 5 bytes that end with the last non-zero one */
		for (i = 0; i < size; i++) {
			u8 b = p[i];

			if (zeros >= 2 && b == 3) {
				zeros = 0;
				continue;
			}
			zeros = b ? 0 : zeros + 1;
			memmove(win, win + 1, 4);
			win[4] = b;
			n++;
			if (b && n >= 5) {
				memcpy(last, win, 5);
				have = true;
			}
		}
		if (!have || last[4] != 0x80)
			return false;
		*crc = get_unaligned_be32(last);
		return true;
	}
	return false;
}

/* (g.lock held) remove q[i], keeping the order */
static struct el_ent *q_remove(int i)
{
	struct el_ent *e = g.q[i];

	memmove(&g.q[i], &g.q[i + 1], (g.nq - i - 1) * sizeof(e));
	g.nq--;
	return e;
}

/* (g.lock held) the entry goes back to the decoder: release in process context */
static void ent_release_locked(struct el_ent *e)
{
	e->state = EL_RELEASE;
	schedule_work(&g.rel_work);
}

static void rel_work_fn(struct work_struct *work)
{
	struct el_ent *rel[MAX_EL];
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		struct el_ent *e = &g.ent[i];

		if (e->state != EL_RELEASE)
			continue;
		rel[n++] = e;
	}
	spin_unlock_irqrestore(&g.lock, flags);

	for (i = 0; i < n; i++) {
		struct el_ent *e = rel[i];

		dmabuf_put_vframe(e->db);
		dma_buf_put(e->db);
		/* the decoder may decode into the buffer again */
		if (e->fence) {
			dma_fence_signal(e->fence);
			dma_fence_put(e->fence);
			e->fence = NULL;
		}
		spin_lock_irqsave(&g.lock, flags);
		if (g.done_head - g.done_tail < DONE_RING)
			g.done[g.done_head++ % DONE_RING] = e->id;
		e->db = NULL;
		e->vf = NULL;
		e->state = EL_FREE;
		spin_unlock_irqrestore(&g.lock, flags);
	}
	if (n)
		wake_up_interruptible(&g.done_wq);
}

/* (g.lock held) the pts of a frame by its RPU CRC: the earliest pending hint */
static bool pts_of_crc(u32 crc, s64 *pts)
{
	int i, n = min(g.nh, HINT_SEARCH);

	for (i = 0; i < n; i++) {
		if (g.hint[i].crc != crc)
			continue;
		*pts = g.hint[i].pts_us;
		g.hints_skipped += i;
		memmove(&g.hint[0], &g.hint[i + 1], (g.nh - i - 1) * sizeof(g.hint[0]));
		g.nh -= i + 1;
		return true;
	}
	return false;
}

/*
 * A base layer frame the Dolby Vision driver processes (vsync interrupt):
 * find its EL picture and put it at the head of the queue.
 */
static bool pair_bl(struct vframe_s *vf)
{
	unsigned long flags;
	bool found = false, fresh = true;
	s64 pts;
	u32 crc;

	if (!vf || !READ_ONCE(g.active) || src_fmt_off < 0)
		return false;
	if (!rpu_crc_of(vf, &crc)) {
		spin_lock_irqsave(&g.lock, flags);
		g.bl_no_crc++;
		spin_unlock_irqrestore(&g.lock, flags);
		return false;
	}
	spin_lock_irqsave(&g.lock, flags);
	if (g.bl_vf == vf) {
		/* asked again for the same frame (wait, then parse) */
		found = g.bl_found && g.nq;
		fresh = false;
	} else if (!pts_of_crc(crc, &pts)) {
		/* decoded before a seek, or not reported */
		g.bl_no_hint++;
		g.bl_vf = vf;
		g.bl_found = false;
	} else {
		/* older ones: their frames are gone */
		while (g.nq && g.q[0]->pts_us < pts) {
			struct el_ent *e = q_remove(0);

			g.el_dropped++;
			dbg(2, "EL id %u pts %lld dropped (frame %lld)\n", e->id, e->pts_us, pts);
			ent_release_locked(e);
		}
		found = g.nq && g.q[0]->pts_us == pts;
		if (found) {
			g.bl_paired++;
			g.mark_id = g.q[0]->id;
		} else
			g.bl_no_el++;
		g.bl_vf = vf;
		g.bl_found = found;
	}
	if (found)
		sc2_src_fmt(vf)->dual_layer = true;
	if (fresh)
		start_mute(found);
	spin_unlock_irqrestore(&g.lock, flags);
	return found;
}

/* is_dovi_dual_layer_frame(vf): true for a frame paired here */
static int dual_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	*(bool *)ri->data = pair_bl((struct vframe_s *)regs->regs[0]);
	return 0;
}

static int dual_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if (*(bool *)ri->data)
		regs->regs[0] = 1;
	return 0;
}

/* dolby_vision_parse_metadata(vf, ...): marks the frame before the EL get */
static int parse_pre(struct kprobe *p, struct pt_regs *regs)
{
	pair_bl((struct vframe_s *)regs->regs[0]);
	return 0;
}

/*
 * --- driver fixes ---
 *
 * The SC2 aml_media of the Dune firmwares (r24 260303, r25 261021) lacks four
 * changes FEL needs (the .101 test box ran a binary-patched r24 aml_media,
 * dune-p7-fel/kernel):
 *
 *   omx:    dolby_vision_update_metadata() clears its BL/EL table without
 *           putting the EL when vf->omx_index == 0, which it is for every
 *           MediaCodec frame: no EL is ever put back, the EL decoder runs dry.
 *           Patched: "cbnz w8" (omx_index) -> "b" (never clear).
 *   core1a: apply_stb_core_settings(): vd2 to core1a (DOLBY_PATH_CTRL bit 1 =
 *           0) for cpu 3..4 only; patched "cmp w10, #1" -> "#2" (cpu 3..5, SC2).
 *   bl:     apply_stb_core_settings(): the run_mode_delay BL disable written
 *           for G12 (cpu 2) only; patched "cmp w8, #2" -> "#5" (SC2).
 *   vd2:    dvel_swap_frame() (inlined twice into vsync_isr_in) passes
 *           &vd_layer[0] / &glayer_info[0] to set_layer_display_canvas(): the
 *           EL AFBC addresses go into VD1, the EL shows as noise. Patched: the
 *           relocation addends to index 1 (vd_layer stride 0x5c8, glayer_info
 *           0x8c); here the call's arguments.
 *
 * The omx one acts during a session only, the others always (as the patched
 * binary; the core is set up before a session starts). Done with kprobes on
 * those instructions, found by their surroundings; a firmware with the fixes
 * in place, or another build, has no match and is left alone.
 */
#define FIX_SCAN_UPDATE	0x100
#define FIX_SCAN_APPLY	0x2000
#define FIX_SCAN_VSYNC	0x8000
#define VD_LAYER_STRIDE	0x5c8	/* struct video_layer_s */
#define GLAYER_STRIDE	0x8c	/* struct disp_info_s */

#define BCOND	0xff00001f	/* b.cond / cbz / cbnz: the opcode and the cond or register */
#define ADRP	0x9f00001f	/* adrp: the opcode and the register */
#define ADDIMM	0xffc003ff	/* add/ldr immediate: all but the (relocated) imm12 */

struct insn_pat {
	s8 off;			/* in instructions from the probe; the list ends with mask 0 */
	u32 val, mask;
};

enum fix_kind { FIX_OMX, FIX_CORE1A, FIX_BL, FIX_VD2 };

struct dv_fix {
	const char *name, *func;
	enum fix_kind kind;
	u32 scan;
	u8 matches, nth;	/* expected matches (0: 1), the one probed */
	struct insn_pat pat[7];
	struct kprobe kp;
	unsigned long target;	/* the branch target the handler uses */
	unsigned long x0, x3;	/* vd2: &vd_layer[0], &glayer_info[0] at the call */
	u32 frame_par_off;	/* vd2: offsetof(struct video_layer_s, cur_frame_par) */
	bool ok;
	u64 hits;
};

/* adrp x0; add x0, x0, #lo12; ldr x2, [x0, #cur_frame_par]; adrp x3; add x3, x3, #lo12;
 * mov x1, x22; >bl set_layer_display_canvas */
#define VD2_PAT { { -6, 0x90000000, ADRP }, { -5, 0x91000000, ADDIMM }, \
		  { -4, 0xf9400002, ADDIMM }, { -3, 0x90000003, ADRP }, \
		  { -2, 0x91000063, ADDIMM }, { -1, 0xaa1603e1, ~0u }, { 0, 0x94000000, 0xfc000000 } }

static struct dv_fix dv_fixes[] = {
	/* ldr w8, [x19, #8]; >cbnz w8 */
	{ "omx", "dolby_vision_update_metadata", FIX_OMX, FIX_SCAN_UPDATE, 0, 0,
	  { { -1, 0xb9400a68, ~0u }, { 0, 0x35000008, BCOND } } },
	/* ldrb w9, [x9, #:lo12:var] (relocated at load); sub w10, w8, #3; >cmp w10, #1;
	 * b.hi; cbnz w9; mov w0, #0x1a0c (DOLBY_PATH_CTRL); mov w2, #1 (bit 1) */
	{ "core1a", "apply_stb_core_settings", FIX_CORE1A, FIX_SCAN_APPLY, 0, 0,
	  { { -2, 0x39400129, ADDIMM }, { -1, 0x51000d0a, ~0u }, { 0, 0x7100055f, ~0u },
	    { 1, 0x54000008, BCOND }, { 3, 0x52834180, ~0u }, { 4, 0x52800022, ~0u } } },
	/* ldr w21, [sp, #28]; >cmp w8, #2; b.eq */
	{ "bl", "apply_stb_core_settings", FIX_BL, FIX_SCAN_APPLY, 0, 0,
	  { { -1, 0xb9401ff5, ~0u }, { 0, 0x7100091f, ~0u }, { 1, 0x54000000, BCOND } } },
	{ "vd2a", "vsync_isr_in", FIX_VD2, FIX_SCAN_VSYNC, 2, 0, VD2_PAT },
	{ "vd2b", "vsync_isr_in", FIX_VD2, FIX_SCAN_VSYNC, 2, 1, VD2_PAT },
};

static long insn_imm19(u32 insn)
{
	return (long)((s32)(((insn >> 5) & 0x7ffff) << 13) >> 11);
}

/* the address an adrp + add pair (relocated) computes */
static unsigned long adrp_add(unsigned long pc, u32 adrp, u32 add)
{
	s64 imm = (s64)((((adrp >> 5) & 0x7ffff) << 2) | ((adrp >> 29) & 3)) << 43 >> 31;

	return (pc & ~0xfffUL) + imm + ((add >> 10) & 0xfff);
}

static int fix_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct dv_fix *f = container_of(p, struct dv_fix, kp);

	switch (f->kind) {
	case FIX_OMX:
		if (!READ_ONCE(g.active))
			return 0;
		break;
	case FIX_CORE1A:
		/* w10 = cpu_id - 3; SC2 (5) passes as "ls": on to the next condition */
		if ((u32)regs->regs[10] != 2)
			return 0;
		break;
	case FIX_BL:
		/* w8 = cpu_id; SC2 (5) takes the G12 branch */
		if ((u32)regs->regs[8] != 5)
			return 0;
		break;
	case FIX_VD2:
		/* the EL to VD2: layer 1, its frame parameters, display info 1 */
		if (regs->regs[0] != f->x0 || regs->regs[3] != f->x3)
			return 0;
		regs->regs[0] += VD_LAYER_STRIDE;
		regs->regs[2] = *(u64 *)(regs->regs[0] + f->frame_par_off);
		regs->regs[3] += GLAYER_STRIDE;
		f->hits++;
		return 0;	/* the bl runs, with these arguments */
	}
	f->hits++;
	regs->pc = f->target;
	return 1;
}

static unsigned long func_addr(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long a;

	if (register_kprobe(&kp))
		return 0;
	a = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return a;
}

/* all sites are looked up before any probe goes in (a probe changes the code) */
static void fixes_register(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dv_fixes); i++) {
		struct dv_fix *f = &dv_fixes[i];
		unsigned long base = func_addr(f->func), at = 0;
		u32 k, n = 0, want = f->matches ? f->matches : 1;
		const u32 *w;

		if (!base) {
			pr_info(DRV_NAME ": fix %s: %s not found\n", f->name, f->func);
			continue;
		}
		w = (const u32 *)base;
		for (k = 6; k + 4 < f->scan / 4; k++) {
			const struct insn_pat *p;

			for (p = f->pat; p < f->pat + ARRAY_SIZE(f->pat) && p->mask; p++)
				if ((w[k + p->off] & p->mask) != p->val)
					break;
			if (p < f->pat + ARRAY_SIZE(f->pat) && p->mask)
				continue;
			if (n == f->nth)
				at = base + k * 4;
			n++;
		}
		if (n != want) {
			pr_info(DRV_NAME ": fix %s: %u matches in %s (fixed or other build): off\n",
				f->name, n, f->func);
			continue;
		}
		w = (const u32 *)at;
		switch (f->kind) {
		case FIX_OMX:		/* the cbnz's own target */
			f->target = at + insn_imm19(w[0]);
			break;
		case FIX_CORE1A:	/* past cmp + b.hi */
			f->target = at + 8;
			break;
		case FIX_BL:		/* the b.eq target */
			f->target = at + 4 + insn_imm19(w[1]);
			break;
		case FIX_VD2:
			f->x0 = adrp_add(at - 24, w[-6], w[-5]);
			f->x3 = adrp_add(at - 12, w[-3], w[-2]);
			f->frame_par_off = ((w[-4] >> 10) & 0xfff) * 8;
			break;
		}
		f->kp.addr = (kprobe_opcode_t *)at;
		f->kp.pre_handler = fix_pre;
		pr_info(DRV_NAME ": fix %s at %s+%#lx\n", f->name, f->func, at - base);
	}
	for (i = 0; i < ARRAY_SIZE(dv_fixes); i++) {
		struct dv_fix *f = &dv_fixes[i];

		if (!f->kp.addr)
			continue;
		/* the vd2 call looks the same with the patched addends: only with the
		 * omx one unfixed too (an unpatched module) */
		if (f->kind == FIX_VD2 && !dv_fixes[0].ok) {
			pr_info(DRV_NAME ": fix %s: module not the stock one: off\n", f->name);
			continue;
		}
		if (register_kprobe(&f->kp)) {
			pr_info(DRV_NAME ": fix %s: kprobe failed\n", f->name);
			continue;
		}
		f->ok = true;
	}
}

static void fixes_unregister(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dv_fixes); i++)
		if (dv_fixes[i].ok) {
			unregister_kprobe(&dv_fixes[i].kp);
			dv_fixes[i].ok = false;
		}
}

/* --- the "dveldec" provider, called by the Dolby Vision driver --- */

static struct vframe_s *prov_peek(void *op_arg)
{
	struct vframe_s *vf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	if (g.active && g.nq)
		vf = g.q[0]->vf;
	spin_unlock_irqrestore(&g.lock, flags);
	return vf;
}

static struct vframe_s *prov_get(void *op_arg)
{
	struct vframe_s *vf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	if (g.active && g.nq) {
		struct el_ent *e = q_remove(0);
		u32 q_ms;
		int k;

		e->state = EL_OUT;
		e->out_jiffies = jiffies;
		vf = e->vf;
		g.el_out++;
		q_ms = jiffies_to_msecs(e->out_jiffies - e->q_jiffies);
		g.q_max_ms = max(g.q_max_ms, q_ms);
		if (e->id != g.mark_id)
			g.el_get_other++;
		/* the decoder's vframe changed since the queueing: buffer reused */
		if (memcmp(e->snap, vf, VF_SNAP)) {
			g.el_vf_changed++;
			if (!g.change_logged) {
				g.change_logged = true;
				for (k = 0; k < VF_SNAP && e->snap[k] == ((u8 *)vf)[k]; k++)
					;
				pr_info(DRV_NAME ": EL id %u vframe changed after queueing (first byte %d, %u ms queued)\n",
					e->id, k, q_ms);
			}
		}
		dbg(2, "EL id %u crc %08x to display\n", e->id, e->crc);
	}
	spin_unlock_irqrestore(&g.lock, flags);
	return vf;
}

static void prov_put(struct vframe_s *vf, void *op_arg)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		struct el_ent *e = &g.ent[i];

		if (e->state == EL_OUT && e->vf == vf) {
			u32 d_ms = jiffies_to_msecs(jiffies - e->out_jiffies);

			g.el_put++;
			if (e->sess == g.sess)
				g.disp_max_ms = max(g.disp_max_ms, d_ms);
			if (d_ms > DISP_LONG_MS)
				g.el_disp_long++;
			ent_release_locked(e);
			break;
		}
	}
	spin_unlock_irqrestore(&g.lock, flags);
	if (i == MAX_EL)
		dbg(1, "put of an unknown EL frame %p\n", vf);
}

static int prov_event(int type, void *data, void *op_arg)
{
	return 0;
}

static int prov_states(struct vframe_states *states, void *op_arg)
{
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	states->vf_pool_size = MAX_EL;
	states->buf_recycle_num = 0;
	states->buf_free_num = MAX_EL - g.nq;
	states->buf_avail_num = g.nq;
	spin_unlock_irqrestore(&g.lock, flags);
	return 0;
}

static const struct vframe_operations_s prov_ops = {
	.peek = prov_peek,
	.get = prov_get,
	.put = prov_put,
	.event_cb = prov_event,
	.vf_states = prov_states,
};

/* --- session --- */

/*
 * Dolby Vision FEL on in the driver (its debug file: root only). Written from
 * a kernel worker: the player's ioctl runs with the app's credentials.
 */
static void fel_on_work_fn(struct work_struct *work)
{
	static const char cmd[] = "enable_fel 1";
	struct file *f = filp_open(DV_DEBUG_PATH, O_WRONLY, 0);
	loff_t pos = 0;

	if (IS_ERR(f)) {
		pr_warn(DRV_NAME ": %s: %ld\n", DV_DEBUG_PATH, PTR_ERR(f));
		return;
	}
	kernel_write(f, cmd, sizeof(cmd) - 1, &pos);
	filp_close(f, NULL);
}
static DECLARE_WORK(fel_on_work, fel_on_work_fn);

static void fel_on(void)
{
	schedule_work(&fel_on_work);
	flush_work(&fel_on_work);
}

/*
 * The session ended (also when the player died): the release fences of the EL
 * pictures still on display are signalled now. The decoder is torn down at
 * once; with a fence still pending its buffers stay with the media codec
 * service for good (9 of them per such stop measured, until the codec memory
 * runs out: no EL any more). The dma-buf stays referenced here until the
 * driver puts the picture back (or the orphan timeout), so the memory the
 * display reads stays valid.
 */
static void signal_out_fences(void)
{
	struct dma_fence *sig[MAX_EL];
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		struct el_ent *e = &g.ent[i];

		if (e->state == EL_OUT && e->fence) {
			sig[n++] = e->fence;
			e->fence = NULL;
		}
	}
	spin_unlock_irqrestore(&g.lock, flags);
	for (i = 0; i < n; i++) {
		dma_fence_signal(sig[i]);
		dma_fence_put(sig[i]);
	}
	if (n)
		dbg(1, "%d EL fences of pictures on display signalled at the session end\n", n);
}

/* drop the queued EL pictures (not the ones on display) and / or the hints */
static void flush_queue(u32 what)
{
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	if (what & DVFEL_SC2_FLUSH_EL) {
		while (g.nq) {
			g.el_flushed++;
			ent_release_locked(q_remove(0));
		}
		g.bl_vf = NULL;
		g.bl_found = false;
	}
	if (what & DVFEL_SC2_FLUSH_HINTS)
		g.nh = 0;
	spin_unlock_irqrestore(&g.lock, flags);
}

static long ioc_bl_hint(struct file *f, void __user *arg)
{
	struct dvfel_sc2_hint r;
	unsigned long flags;
	int i;

	if (copy_from_user(&r, arg, sizeof(r)))
		return -EFAULT;
	if (g.owner != f)
		return -EINVAL;
	spin_lock_irqsave(&g.lock, flags);
	/* the first access unit of the session: in the decoder now, not yet displayed */
	if (early_mute == 2 && g.hints_in == g.at_start.hints_in)
		start_mute_early("first base layer access unit");
	g.hints_in++;
	if (g.nh == MAX_HINTS) {
		/* frames that never reached the display: the oldest goes */
		memmove(&g.hint[0], &g.hint[1], (MAX_HINTS - 1) * sizeof(g.hint[0]));
		g.nh--;
		g.hints_overflow++;
	}
	/* decode order in, display (pts) order kept */
	for (i = g.nh; i > 0 && g.hint[i - 1].pts_us > r.pts_us; i--)
		;
	memmove(&g.hint[i + 1], &g.hint[i], (g.nh - i) * sizeof(g.hint[0]));
	g.hint[i].pts_us = r.pts_us;
	g.hint[i].crc = r.rpu_crc;
	g.nh++;
	spin_unlock_irqrestore(&g.lock, flags);
	return 0;
}

static const char *fence_driver_name(struct dma_fence *f)
{
	return DRV_NAME;
}

static const char *fence_timeline_name(struct dma_fence *f)
{
	return "el";
}

static const struct dma_fence_ops el_fence_ops = {
	.get_driver_name = fence_driver_name,
	.get_timeline_name = fence_timeline_name,
};

/*
 * EL pictures the driver kept after the provider went away (their session
 * ended; a new session may run already: they are not its pictures)
 */
static void orphan_work_fn(struct work_struct *work)
{
	unsigned long flags;
	bool more = false;
	int i;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		struct el_ent *e = &g.ent[i];

		if (e->state != EL_OUT || (g.active && e->sess == g.sess))
			continue;
		if (time_after(jiffies, e->out_jiffies + msecs_to_jiffies(ORPHAN_MS))) {
			dbg(1, "EL id %u not returned by the driver, released\n", e->id);
			g.el_orphans++;
			ent_release_locked(e);
		} else {
			more = true;
		}
	}
	spin_unlock_irqrestore(&g.lock, flags);
	if (more)
		schedule_delayed_work(&g.orphan_work, msecs_to_jiffies(ORPHAN_MS / 4));
}

/* (g.lock held) the counters now */
static void totals_locked(struct dvfel_sc2_totals *t)
{
	t->el_in = g.el_in;
	t->el_out = g.el_out;
	t->el_put = g.el_put;
	t->el_dropped = g.el_dropped;
	t->el_flushed = g.el_flushed;
	t->bl_paired = g.bl_paired;
	t->bl_no_el = g.bl_no_el;
	t->bl_no_hint = g.bl_no_hint;
	t->hints_in = g.hints_in;
	t->hints_skipped = g.hints_skipped;
	t->vf_changed = g.el_vf_changed;
	t->get_other = g.el_get_other;
	t->disp_long = g.el_disp_long;
	t->orphans = g.el_orphans;
}

/* a line per session in the kernel log: what it did */
static void sess_summary(void)
{
	struct dvfel_sc2_totals t, *s = &g.at_start;
	unsigned long flags;
	int i, out = 0, old = 0;

	spin_lock_irqsave(&g.lock, flags);
	totals_locked(&t);
	for (i = 0; i < MAX_EL; i++) {
		if (g.ent[i].state != EL_OUT)
			continue;
		out++;
		old += g.ent[i].sess != g.sess;
	}
	spin_unlock_irqrestore(&g.lock, flags);
	pr_info(DRV_NAME ": session %u: el in %llu out %llu put %llu dropped %llu flushed %llu, "
		"bl paired %llu no_el %llu no_hint %llu, hints %llu skipped %llu, "
		"vf_changed %llu get_other %llu disp_long %llu orphans %llu, max ms queued %u on display %u, "
		"on display %d (older sessions %d)\n", g.sess,
		t.el_in - s->el_in, t.el_out - s->el_out, t.el_put - s->el_put,
		t.el_dropped - s->el_dropped, t.el_flushed - s->el_flushed,
		t.bl_paired - s->bl_paired, t.bl_no_el - s->bl_no_el, t.bl_no_hint - s->bl_no_hint,
		t.hints_in - s->hints_in, t.hints_skipped - s->hints_skipped,
		t.vf_changed - s->vf_changed, t.get_other - s->get_other, t.disp_long - s->disp_long,
		t.orphans - s->orphans, g.q_max_ms, g.disp_max_ms, out, old);
}

static long sess_start(struct file *f)
{
	unsigned long flags;
	long ret = 0;

	mutex_lock(&g.sess_lock);
	if (g.owner && g.owner != f) {
		ret = -EBUSY;
		goto out;
	}
	if (!g.prov_reg) {
		if (vf_get_provider(EL_PROV_NAME)) {
			/* a stream mode dual layer decoder is running */
			ret = -EBUSY;
			goto out;
		}
		vf_provider_init(&g.prov, EL_PROV_NAME, &prov_ops, NULL);
		ret = vf_reg_provider(&g.prov);
		if (ret < 0)
			goto out;
		ret = 0;
		g.prov_reg = true;
	}
	g.owner = f;
	fel_on();
	spin_lock_irqsave(&g.lock, flags);
	g.sess++;
	totals_locked(&g.at_start);
	g.q_max_ms = g.disp_max_ms = 0;
	g.vf_logged = g.change_logged = false;
	g.mark_id = 0;
	g.start_done = false;
	g.first_frame = false;
	g.paired_run = 0;
	g.start_jiffies = 0;
	if (early_mute == 1)
		start_mute_early("session start");
	spin_unlock_irqrestore(&g.lock, flags);
	WRITE_ONCE(g.active, true);
	/* pictures an earlier session left with the driver go back now */
	schedule_delayed_work(&g.orphan_work, msecs_to_jiffies(ORPHAN_MS / 4));
	dbg(1, "session %u start\n", g.sess);
out:
	mutex_unlock(&g.sess_lock);
	return ret;
}

static void sess_stop(struct file *f)
{
	unsigned long flags;

	mutex_lock(&g.sess_lock);
	if (g.owner != f) {
		mutex_unlock(&g.sess_lock);
		return;
	}
	WRITE_ONCE(g.active, false);
	/* after the active flag: a frame being paired now does not mute again */
	spin_lock_irqsave(&g.lock, flags);
	unmute_locked();
	spin_unlock_irqrestore(&g.lock, flags);
	cancel_delayed_work(&g.start_work);
	flush_queue(DVFEL_SC2_FLUSH_EL | DVFEL_SC2_FLUSH_HINTS);
	signal_out_fences();
	if (g.prov_reg) {
		vf_unreg_provider(&g.prov);
		g.prov_reg = false;
	}
	g.owner = NULL;
	schedule_delayed_work(&g.orphan_work, msecs_to_jiffies(ORPHAN_MS / 4));
	sess_summary();
	mutex_unlock(&g.sess_lock);
}

static long ioc_el_queue(struct file *f, void __user *arg)
{
	struct dvfel_sc2_el r;
	struct el_ent *e = NULL;
	struct dma_buf *db;
	struct vframe_s *vf;
	struct dma_fence *fence;
	struct sync_file *sf = NULL;
	unsigned long flags;
	int i, fd;

	if (copy_from_user(&r, arg, sizeof(r)))
		return -EFAULT;
	if (g.owner != f || !READ_ONCE(g.active))
		return -EINVAL;
	db = dma_buf_get(r.fd);
	if (IS_ERR(db))
		return PTR_ERR(db);
	if (!dmabuf_is_uvm(db)) {
		dma_buf_put(db);
		return -ENODEV;
	}
	vf = dmabuf_get_vframe(db);
	if (!vf) {
		dma_buf_put(db);
		return -ENOENT;
	}

	/* the release fence: one reference for the sync file, one kept here */
	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence && fd >= 0) {
		dma_fence_init(fence, &el_fence_ops, &g.fence_lock, g.fence_ctx, ++g.fence_seq);
		sf = sync_file_create(fence);
	}
	if (!sf) {
		if (fd >= 0)
			put_unused_fd(fd);
		if (fence && fence->ops)
			dma_fence_put(fence);
		else
			kfree(fence);
		dmabuf_put_vframe(db);
		dma_buf_put(db);
		return -ENOMEM;
	}

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		if (g.ent[i].state == EL_FREE) {
			e = &g.ent[i];
			break;
		}
	}
	if (e) {
		e->state = EL_QUEUED;
		e->db = db;
		e->vf = vf;
		e->fence = fence;
		e->crc = r.rpu_crc;
		e->pts_us = r.pts_us;
		e->id = ++g.next_id;
		e->sess = g.sess;
		e->q_jiffies = jiffies;
		memcpy(e->snap, vf, VF_SNAP);
		/* in pts order (the EL decoder's output is in display order) */
		for (i = g.nq; i > 0 && g.q[i - 1]->pts_us > r.pts_us; i--)
			;
		memmove(&g.q[i + 1], &g.q[i], (g.nq - i) * sizeof(e));
		g.q[i] = e;
		g.nq++;
		g.el_in++;
		r.id = e->id;
		r.queued = g.nq;
	}
	spin_unlock_irqrestore(&g.lock, flags);
	if (!e) {
		fput(sf->file);
		put_unused_fd(fd);
		dma_fence_signal(fence);
		dma_fence_put(fence);
		dmabuf_put_vframe(db);
		dma_buf_put(db);
		return -EBUSY;
	}
	r.fence_fd = fd;
	if (!g.vf_logged) {
		/* the first EL vframe of a session, for the log (its layout is the firmware's) */
		g.vf_logged = true;
		pr_info(DRV_NAME ": session %u first EL vframe: %*ph\n", g.sess, 64, vf);
	}
	dbg(2, "EL id %u crc %08x pts %lld queued (%u)\n", r.id, r.rpu_crc, r.pts_us, r.queued);
	if (copy_to_user(arg, &r, sizeof(r))) {
		/* the entry stays (released with the session); the fence with it */
		fput(sf->file);
		put_unused_fd(fd);
		return -EFAULT;
	}
	fd_install(fd, sf->file);
	return 0;
}

static bool done_ready(void)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&g.lock, flags);
	ready = g.done_head != g.done_tail;
	spin_unlock_irqrestore(&g.lock, flags);
	return ready;
}

static long ioc_wait_done(struct file *f, void __user *arg)
{
	struct dvfel_sc2_done r;
	unsigned long flags;
	long t;

	if (copy_from_user(&r, arg, sizeof(r)))
		return -EFAULT;
	if (r.timeout_ms) {
		t = wait_event_interruptible_timeout(g.done_wq, done_ready(),
						     msecs_to_jiffies(r.timeout_ms));
		if (t < 0)
			return t;
	}
	r.count = 0;
	spin_lock_irqsave(&g.lock, flags);
	while (g.done_head != g.done_tail && r.count < DVFEL_SC2_MAX_DONE)
		r.id[r.count++] = g.done[g.done_tail++ % DONE_RING];
	spin_unlock_irqrestore(&g.lock, flags);
	return copy_to_user(arg, &r, sizeof(r)) ? -EFAULT : 0;
}

static long dvfel_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *p = (void __user *)arg;

	switch (cmd) {
	case DVFEL_IOC_SC2_INFO: {
		struct dvfel_sc2_info r = {};

		strscpy(r.version, DVFEL_SC2_VERSION, sizeof(r.version));
		return copy_to_user(p, &r, sizeof(r)) ? -EFAULT : 0;
	}
	case DVFEL_IOC_SC2_START:
		return sess_start(f);
	case DVFEL_IOC_SC2_EL_QUEUE:
		return ioc_el_queue(f, p);
	case DVFEL_IOC_SC2_WAIT_DONE:
		return ioc_wait_done(f, p);
	case DVFEL_IOC_SC2_FLUSH: {
		u32 what;

		if (g.owner != f)
			return -EINVAL;
		if (get_user(what, (u32 __user *)p))
			return -EFAULT;
		flush_queue(what);
		return 0;
	}
	case DVFEL_IOC_SC2_STOP:
		sess_stop(f);
		return 0;
	case DVFEL_IOC_SC2_BL_HINT:
		return ioc_bl_hint(f, p);
	default:
		return -ENOTTY;
	}
}

static int dvfel_release(struct inode *inode, struct file *f)
{
	sess_stop(f);
	return 0;
}

static const struct file_operations dvfel_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = dvfel_ioctl,
	.compat_ioctl = dvfel_ioctl,
	.release = dvfel_release,
};

static struct miscdevice dvfel_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRV_NAME,
	.fops = &dvfel_fops,
	.mode = 0666,
};

static int stats_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i, out = 0;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++)
		out += g.ent[i].state == EL_OUT;
	seq_printf(m, "version %s active %d provider %d probes %d/%d\n", DVFEL_SC2_VERSION,
		   g.active, g.prov_reg, krp_dual_ok, kp_parse_ok);
	seq_printf(m, "el in %llu out %llu put %llu dropped %llu flushed %llu queued %d on_display %d\n",
		   g.el_in, g.el_out, g.el_put, g.el_dropped, g.el_flushed, g.nq, out);
	seq_printf(m, "bl paired %llu no_el %llu no_hint %llu no_crc %llu\n", g.bl_paired, g.bl_no_el,
		   g.bl_no_hint, g.bl_no_crc);
	seq_printf(m, "hints in %llu pending %d skipped %llu overflow %llu start_muted %llu muted %d early_mute %d\n",
		   g.hints_in, g.nh, g.hints_skipped, g.hints_overflow, g.start_muted, g.muted, early_mute);
	seq_printf(m, "session %u vf_changed %llu get_other %llu disp_long %llu orphans %llu max_ms queued %u on_display %u\n",
		   g.sess, g.el_vf_changed, g.el_get_other, g.el_disp_long, g.el_orphans, g.q_max_ms,
		   g.disp_max_ms);
	seq_puts(m, "fixes");
	for (i = 0; i < ARRAY_SIZE(dv_fixes); i++)
		seq_printf(m, " %s %d/%llu", dv_fixes[i].name, dv_fixes[i].ok, dv_fixes[i].hits);
	seq_putc(m, '\n');
	spin_unlock_irqrestore(&g.lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int __init dvfel_init(void)
{
	int ret;

	/* the inner layout of src_fmt used here, as in the Dune binary (see the top) */
	BUILD_BUG_ON(offsetof(struct vframe_src_fmt_s, sei_ptr) != 8);
	BUILD_BUG_ON(offsetof(struct vframe_src_fmt_s, sei_size) != 16);
	BUILD_BUG_ON(offsetof(struct vframe_src_fmt_s, dual_layer) != 20);

	spin_lock_init(&g.lock);
	spin_lock_init(&g.fence_lock);
	g.fence_ctx = dma_fence_context_alloc(1);
	mutex_init(&g.sess_lock);
	init_waitqueue_head(&g.done_wq);
	INIT_WORK(&g.rel_work, rel_work_fn);
	INIT_DELAYED_WORK(&g.orphan_work, orphan_work_fn);
	INIT_DELAYED_WORK(&g.start_work, start_work_fn);

	krp_dual.kp.symbol_name = "is_dovi_dual_layer_frame";
	krp_dual.entry_handler = dual_entry;
	krp_dual.handler = dual_ret;
	krp_dual.data_size = sizeof(bool);
	krp_dual.maxactive = 4;
	ret = register_kretprobe(&krp_dual);
	if (ret) {
		pr_err(DRV_NAME ": kretprobe is_dovi_dual_layer_frame: %d\n", ret);
		return ret;
	}
	krp_dual_ok = true;
	kp_parse.symbol_name = "dolby_vision_parse_metadata";
	kp_parse.pre_handler = parse_pre;
	ret = register_kprobe(&kp_parse);
	if (ret) {
		pr_err(DRV_NAME ": kprobe dolby_vision_parse_metadata: %d\n", ret);
		goto err_krp;
	}
	kp_parse_ok = true;
	fixes_register();
	ret = misc_register(&dvfel_misc);
	if (ret)
		goto err_kp;
	dbg_dir = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("stats", 0444, dbg_dir, NULL, &stats_fops);
	src_fmt_off = measure_src_fmt_off();
	if (src_fmt_off < 0)
		pr_warn(DRV_NAME ": vframe layout of this firmware not found: no FEL pairing\n");
	else if (src_fmt_off != SC2_VF_SRC_FMT_OFF)
		pr_info(DRV_NAME ": vframe src_fmt at %d (Dune r24: %d)\n", src_fmt_off, SC2_VF_SRC_FMT_OFF);
	/* at boot already (the driver keeps it): frames before a session too */
	fel_on();
	pr_info(DRV_NAME ": SC2 %s loaded (src_fmt %d)\n", DVFEL_SC2_VERSION, src_fmt_off);
	return 0;

err_kp:
	fixes_unregister();
	unregister_kprobe(&kp_parse);
err_krp:
	unregister_kretprobe(&krp_dual);
	return ret;
}

static void __exit dvfel_exit(void)
{
	unsigned long flags;
	int i;

	debugfs_remove_recursive(dbg_dir);
	misc_deregister(&dvfel_misc);
	fixes_unregister();
	unregister_kprobe(&kp_parse);
	unregister_kretprobe(&krp_dual);
	WRITE_ONCE(g.active, false);
	cancel_delayed_work_sync(&g.start_work);
	spin_lock_irqsave(&g.lock, flags);
	unmute_locked();
	spin_unlock_irqrestore(&g.lock, flags);
	mutex_lock(&g.sess_lock);
	flush_queue(DVFEL_SC2_FLUSH_EL | DVFEL_SC2_FLUSH_HINTS);
	if (g.prov_reg) {
		vf_unreg_provider(&g.prov);
		g.prov_reg = false;
	}
	g.owner = NULL;
	mutex_unlock(&g.sess_lock);
	cancel_delayed_work_sync(&g.orphan_work);
	/* whatever the driver still holds goes back now */
	for (i = 0; i < MAX_EL; i++)
		if (g.ent[i].state == EL_OUT)
			g.ent[i].state = EL_RELEASE;
	rel_work_fn(NULL);
	cancel_work_sync(&g.rel_work);
}

module_init(dvfel_init);
module_exit(dvfel_exit);
MODULE_DESCRIPTION("Dolby Vision FEL by the Dolby Vision core of Amlogic SC2 (Android)");
MODULE_AUTHOR("djnice");
MODULE_LICENSE("GPL");
MODULE_VERSION(DVFEL_SC2_VERSION);
