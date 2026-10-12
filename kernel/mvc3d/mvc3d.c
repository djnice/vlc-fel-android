// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d - 3D MVC frame packed output for Amlogic S5 (S928X) Android
 *
 * vfm node between the video composer and the display while a user space
 * compositor is registered:
 *
 *     MediaCodec (H.264 base view) -> HWC -> video_composer.0 -> mvc3d -> video_render.0
 *
 * The player keeps decoding the base view of the MVC stream in hardware;
 * its frames carry the timing (A/V sync, pause, seek, display rate). A user
 * space process decodes both views in software and writes frame packed
 * pictures (L, active space, R; 8-bit 4:2:2) into buffers mapped from here.
 * For every frame the video composer releases, mvc3d posts a job; user
 * space answers with the buffer of that frame's picture, which then goes
 * to the display in place of the base view. See mvc3d_uapi.h.
 *
 * The video composer hands out a frame at its display time, at most two at
 * a time: mvc3d keeps a copy of what the display needs and gives the frame
 * back when the next one comes (early release). The display gets the
 * picture one vsync after the composer released the frame; a player that
 * renders its frames ahead (MVC3D_IOC_SET_LEAD) gets that back: mvc3d holds
 * each picture to the vsync it was meant for.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/version.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/ptrace.h>
#include <drm/drm_plane.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/amlogic/media/vfm/vfm_ext.h>
#include <linux/amlogic/media/codec_mm/codec_mm.h>

#include "mvc3d_uapi.h"

#define DRV_NAME	"mvc3d"
#define QUEUE_LEN	16

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
/* 5.15 renamed omx_index to frame_index (same slot) */
#define frame_index	omx_index
#endif

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "1: events, 4: log the first input frames of a stream");

static int show_timeout_ms = 150;
module_param(show_timeout_ms, int, 0644);
MODULE_PARM_DESC(show_timeout_ms, "max wait for user space's answer to a job before the frame is dropped");

static int vc_pace = 1;
module_param(vc_pace, int, 0644);
MODULE_PARM_DESC(vc_pace, "take at most two frames per vsync from the video composer");

static int out_lead_ms = 30;
module_param(out_lead_ms, int, 0644);
/*
 * the picture is ready a moment after the frame: one vsync (24 Hz; two at
 * 50/60 Hz) avoids the vsync edge. More would ask the H.264 decoder to run
 * further ahead than its buffers allow (125 ms: SurfaceFlinger drops 1 in 7).
 */
MODULE_PARM_DESC(out_lead_ms, "lead time the player renders with (user space reads it, rounded to whole vsyncs), 0: none");

static int lead_phase_us = 6000;
module_param(lead_phase_us, int, 0644);
MODULE_PARM_DESC(lead_phase_us, "a frame taken this long after a vsync was released mid-period (due next vsync)");

/* the display's vsync period as measured while frames go through (player: lead in whole vsyncs) */
static int vsync_us;
module_param(vsync_us, int, 0444);
MODULE_PARM_DESC(vsync_us, "measured vsync period (us), 0: not known");

/*
 * Android has no decoder -> display vfm path to insert mvc3d into: the video
 * composer creates "vcom-map-0 { video_composer.0 video_render.0 }" per
 * playback, and vfm uses the first map that lists a provider. A stock map
 * placed before it that the S5 never uses while video plays (the PIP Dolby
 * Vision base layer) is rewritten to route video_composer.0 through mvc3d
 * while a compositor is registered, and restored afterwards.
 */
static char *vc_map_id = "dvblpath2";
module_param(vc_map_id, charp, 0444);
MODULE_PARM_DESC(vc_map_id, "stock vfm map rewritten to route video_composer.0 through mvc3d");
static char *vc_map_off = "dvbldec2 videopip";
module_param(vc_map_off, charp, 0444);
MODULE_PARM_DESC(vc_map_off, "the stock chain of vc_map_id, restored when no compositor is registered");
#define VC_MAP_ON	"video_composer.0 " DRV_NAME " video_render.0"
static int vc_route;
module_param(vc_route, int, 0644);
MODULE_PARM_DESC(vc_route, "debug: 1 routes video_composer.0 through mvc3d without a compositor (frames pass through); applied at the next client change or upstream unreg");

/* ------------------------------------------------------------------ */
/* HDMI 1.4 frame packing on the Dune HD 5.4 kernel                   */
/* ------------------------------------------------------------------ */

/*
 * The firmware's hdmitx21 takes "3dfp" (/sys/class/amhdmitx/amhdmitx0/config:
 * the 3D flag, the vendor infoframe, the 3D clock table) but builds the mode
 * from the 2D timing: the frame packed timing (CoreELEC linux-amlogic
 * 95e29f15de: active lines 2 * 1080 + 45, double total and pixel clock, 8
 * bit) is what hdmitx21_get_fmtpara() lacks, and the encoder takes its
 * timing from the VIC table. Two return probes add it, for the modes the
 * firmware (its auto frame rate) sets while the compositor wants frame
 * packing (MVC3D_IOC_SET_FP): 1080p24 then goes out as 1080p24 frame packing
 * (1920x2205), and the display mode of Android stays what it set. The
 * desktop resolution has to be 1080p for that (there is no 2160p frame
 * packing): from 2160p the firmware switches to 2160p24.
 *
 * Layouts of the 5.4 hdmitx21 (hdmi_common.h at a3aaaa5, checked against
 * the firmware's hdmitx21_get_fmtpara): struct hdmi_format_para { cd, cs,
 * cr, bits, tmds_clk; struct hdmi_timing timing (+24, 88 bytes); struct
 * vinfo_s hdmitx_vinfo (+112) }.
 */
struct fp_hdmi_timing {
	u32 vic;
	const char *name, *sname;
	u16 pi_mode;
	u32 h_freq, v_freq, pixel_freq;
	u16 h_total, h_blank, h_front, h_sync, h_back, h_active;
	u16 v_total, v_blank, v_front, v_sync, v_back, v_active, v_sync_ln;
	u16 h_pol, v_pol, h_pict, v_pict, h_pixel, v_pixel;
	u32 pixel_repetition_factor;
};

#define FP_PARA_CD		0
#define FP_PARA_CS		4
#define FP_PARA_TMDS		16
#define FP_PARA_TIMING		24
#define FP_PARA_VINFO		112
/* struct vinfo_s */
#define FP_VINFO_HEIGHT		52
#define FP_VINFO_FIELD_HEIGHT	56
#define FP_VINFO_VIDEO_CLK	96
#define FP_VINFO_VTOTAL		104
#define FP_VINFO_INFO_3D	144
#define FP_COLORDEPTH_24B	4
#define FP_CS_YUV444		2
#define FP_CS_YUV420		3
#define FP_3D_INFO		2	/* FP_3D */

static int fp_timing;
module_param(fp_timing, int, 0644);
MODULE_PARM_DESC(fp_timing, "debug: 1 wants frame packing without a compositor asking for it (MVC3D_IOC_SET_FP)");
/*
 * the 3D flags of struct hdmitx_dev (bit 0: 3dfp, 1: 3dtb, 2: 3dss), from
 * the firmware's config_store ("3dfp": orr #1 into hdmitx21_device+0x2f08)
 */
static int fp_flags_off = -1;
module_param(fp_flags_off, int, 0444);
MODULE_PARM_DESC(fp_flags_off, "offset of the 3D flag bits in the firmware's struct hdmitx_dev (-1: by the kernel build)");

/*
 * the firmwares checked (their hdmitx21 is otherwise the same where the
 * probes look): the kernel build (uname -v) and the flag offset
 */
static const struct {
	const char *uts_version;
	int flags_off;
	const char *firmware;
} fp_firmwares[] = {
	{ "#1 SMP PREEMPT Thu Mar 5 00:38:53 UTC 2026", 0x2f08, "Dune HD R24 (260303)" },
	{ "#1 SMP PREEMPT Fri Aug 15 10:17:40 UTC 2025", 0x2ee8, "Dune HD R22 (250815)" },
	{ "#1 SMP PREEMPT Wed Aug 26 23:07:59 UTC 2026", 0x2f08, "Dune HD R24 (260827)" },
};

/*
 * The offset straight from the firmware's code: config_store()'s "3dfp"
 * branch sets the flag with
 *	mov w8, #off; ldrb w9, [x0, x8]; mov w1, wzr; and w9, w9, #0xf8; orr w9, w9, #0x1
 * (x0 = &hdmitx21_device). Searched in aml_media around get_hdmitx21_device
 * with fault tolerant reads; a match must lie in config_store and all
 * matches must agree.
 */
#define FP_SCAN_RANGE	(4 << 20)

void *get_hdmitx21_device(void);

static int fp_flags_scan(void)
{
	static const u32 tail[] = { 0x38686809, 0x2a1f03e1, 0x121d1129, 0x32000129 };
	unsigned long base = (unsigned long)get_hdmitx21_device;
	unsigned long a, start = base - FP_SCAN_RANGE, end = base + FP_SCAN_RANGE;
	char sym[KSYM_SYMBOL_LEN];
	int found = -1, matches = 0, i;
	u32 *page;

	page = kmalloc(PAGE_SIZE + sizeof(tail), GFP_KERNEL);
	if (!page)
		return -1;
	for (a = start & PAGE_MASK; a < end; a += PAGE_SIZE) {
		if (probe_kernel_read(page, (void *)a, PAGE_SIZE))
			continue;
		/* the instructions after the page (a match across the border) */
		if (probe_kernel_read(page + PAGE_SIZE / 4, (void *)(a + PAGE_SIZE), sizeof(tail)))
			memset(page + PAGE_SIZE / 4, 0, sizeof(tail));
		for (i = 0; i < PAGE_SIZE / 4; i++) {
			u32 mov = page[i];
			int off;

			unsigned long pc, dev;
			u32 adrp, add;

			/* movz w8, #imm16 */
			if ((mov & 0xffe0001f) != 0x52800008 || memcmp(&page[i + 1], tail, sizeof(tail)))
				continue;
			sprint_symbol(sym, a + 4 * i);
			if (strncmp(sym, "config_store+", 13))
				continue;
			/*
			 * x0 must be hdmitx21's device (the 2.0 driver in the same
			 * module has the same code for its own): adrp x0 + add x0
			 * right before, decoded and compared with get_hdmitx21_device()
			 */
			pc = a + 4 * i - 8;
			if (probe_kernel_read(&adrp, (void *)pc, 4) ||
			    probe_kernel_read(&add, (void *)(pc + 4), 4) ||
			    (adrp & 0x9f00001f) != 0x90000000 || (add & 0xffc003ff) != 0x91000000)
				continue;
			dev = (pc & ~0xfffUL) +
			      ((long)sign_extend64(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3), 20) << 12) +
			      ((add >> 10) & 0xfff);
			if (dev != (unsigned long)get_hdmitx21_device())
				continue;
			off = (mov >> 5) & 0xffff;
			if (found >= 0 && off != found) {
				pr_warn(DRV_NAME ": 3D flag offsets %#x and %#x in the hdmitx config code\n",
					found, off);
				kfree(page);
				return -1;
			}
			found = off;
			matches++;
		}
	}
	kfree(page);
	if (found >= 0)
		pr_info(DRV_NAME ": 3D flags at %#x (hdmitx config code, %d match%s)\n", found,
			matches, matches > 1 ? "es" : "");
	return found;
}

/*
 * the kernel build from /proc/version (not utsname(): task and namespace
 * layouts differ between the firmwares' kernels)
 */
static int fp_flags_offset(void)
{
	static char v[512];
	struct file *f = filp_open("/proc/version", O_RDONLY, 0);
	loff_t pos = 0;
	ssize_t n;
	int i;

	if (IS_ERR(f))
		return -1;
	n = kernel_read(f, v, sizeof(v) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return -1;
	v[n] = 0;
	v[strcspn(v, "\n")] = 0;
	for (i = 0; i < ARRAY_SIZE(fp_firmwares); i++)
		if (strstr(v, fp_firmwares[i].uts_version)) {
			pr_info(DRV_NAME ": %s: 3D flags at %#x\n", fp_firmwares[i].firmware,
				fp_firmwares[i].flags_off);
			return fp_firmwares[i].flags_off;
		}
	pr_warn(DRV_NAME ": kernel build not known: no HDMI frame packing (fp_flags_off= to force): %s\n", v);
	return -1;
}


static bool fp_probe_on;
static bool fp_client;		/* the compositor wants frame packing */
static bool fp_out;		/* the HDMI output is frame packed */
module_param(fp_out, bool, 0444);
MODULE_PARM_DESC(fp_out, "the HDMI output is frame packed");
static u64 fp_applied, fp_modesets;
static struct delayed_work fp_work;
static bool fp_work_on;		/* fp_work: "3dfp" (true) or "3doff" */

static bool fp_wanted(void)
{
	return READ_ONCE(fp_timing) || READ_ONCE(fp_client);
}

/* the HDMI 1.4 frame packing modes (the 3D clock table has them) */
static bool fp_mode_ok(const char *m)
{
	static const char *const modes[] = {
		"1080p24hz", "1080p25hz", "1080p30hz", "720p50hz", "720p60hz",
	};
	int i;

	for (i = 0; m && i < ARRAY_SIZE(modes); i++)
		if (!strcmp(m, modes[i]))
			return true;
	return false;
}

/*
 * a 2160p desktop: the firmware's frame rate switch picks 2160p24 for 24p,
 * which has no frame packing. While frame packing is wanted, the hdmitx
 * gets the 1080p mode of the same rate instead; Android (DRM, the composer)
 * keeps its 2160p24 (its frame buffer is 1080p anyway).
 */
static int fp_from_2160 = 1;
module_param(fp_from_2160, int, 0644);
MODULE_PARM_DESC(fp_from_2160, "frame packing also when the firmware sets 2160p24/25/30 (output 1080p FP instead)");

static const char *fp_mode_1080(const char *m)
{
	static const char *const map[][2] = {
		{ "2160p24hz", "1080p24hz" }, { "2160p25hz", "1080p25hz" },
		{ "2160p30hz", "1080p30hz" },
	};
	int i;

	for (i = 0; m && i < ARRAY_SIZE(map); i++)
		if (!strcmp(m, map[i][0]))
			return map[i][1];
	return NULL;
}

/* the last mode set gave 1080p frame packing for a 2160p mode of Android */
static bool fp_2160;

/*
 * The GUI (OSD planes) is then still placed in 2160p coordinates (the
 * composer's destination, scaled up from its 1080p frame buffer): the frame
 * packed output shows its top left quarter, enlarged. While so, the plane
 * check gets the destination halved, which puts the GUI where it is on a
 * 1080p desktop (the left view's lines).
 */
static int plane_check_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct drm_plane_state *state = (struct drm_plane_state *)regs->regs[1];

	if (!READ_ONCE(fp_2160) || !READ_ONCE(fp_out) || !state || !state->crtc)
		return 0;
	if (state->crtc_w > 1920 || state->crtc_h > 1080 || state->crtc_x >= 1920 ||
	    state->crtc_y >= 1080) {
		state->crtc_x /= 2;
		state->crtc_y /= 2;
		state->crtc_w = max(state->crtc_w / 2, 1u);
		state->crtc_h = max(state->crtc_h / 2, 1u);
	}
	return 0;
}

static struct kprobe plane_check_probe = {
	.symbol_name = "meson_plane_atomic_check",
	.pre_handler = plane_check_pre,
};
static bool plane_probe_on;

enum fp_call { FP_CALL_OTHER, FP_CALL_MODESET, FP_CALL_UPDATE };

struct fp_call_data {
	enum fp_call call;
	bool fp;
	bool from_2160;
};

static void fp_flag_set(bool on)
{
	u8 *flags = (u8 *)get_hdmitx21_device() + fp_flags_off;

	*flags = on ? (*flags & ~7) | 1 : *flags & ~1;
}

/*
 * hdmitx21_get_fmtpara(mode, attr) entry. The mode set (validate, then set
 * the current mode) decides: the 3dfp flag for the 3D clock table, and the
 * frame packed timing on return, for a frame packing mode while wanted.
 * Calls that only refresh the format of the current mode (HDR and vendor
 * packets) keep what the output has.
 */
static int fmtpara_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct fp_call_data *c = (struct fp_call_data *)ri->data;
	const char *mode = (const char *)regs->regs[0];
	char sym[KSYM_SYMBOL_LEN];

	sprint_symbol(sym, regs->regs[30]);
	if (!strncmp(sym, "hdmitx_validate_vmode+", 22) ||
	    !strncmp(sym, "hdmitx_set_current_vmode+", 25)) {
		c->call = FP_CALL_MODESET;
		c->from_2160 = false;
		if (fp_wanted() && READ_ONCE(fp_from_2160) && fp_mode_1080(mode)) {
			mode = fp_mode_1080(mode);
			regs->regs[0] = (unsigned long)mode;
			c->from_2160 = true;
		}
		c->fp = fp_wanted() && fp_mode_ok(mode);
		if (c->fp || READ_ONCE(fp_out))
			fp_flag_set(c->fp);
	} else if (!strncmp(sym, "hdmitx_set_drm_pkt+", 19) ||
		   !strncmp(sym, "hdmitx_set_vsif_pkt+", 20)) {
		c->call = FP_CALL_UPDATE;
		c->fp = READ_ONCE(fp_out) && fp_mode_ok(mode);
	} else {
		c->call = FP_CALL_OTHER;
		c->fp = false;
	}
	return 0;
}

static int fmtpara_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct fp_call_data *c = (struct fp_call_data *)ri->data;
	u8 *para = (u8 *)regs_return_value(regs);
	struct fp_hdmi_timing *t;
	u32 pix;

	if (c->call == FP_CALL_MODESET) {
		/* validate (the 2160p name) decides; setting the mode then names 1080p */
		if (!c->fp)
			WRITE_ONCE(fp_2160, false);
		else if (c->from_2160)
			WRITE_ONCE(fp_2160, true);
		/* the vendor infoframe goes out after the mode change */
		if (c->fp != READ_ONCE(fp_out)) {
			WRITE_ONCE(fp_out, c->fp);
			fp_work_on = c->fp;
			fp_modesets++;
			schedule_delayed_work(&fp_work, msecs_to_jiffies(500));
		}
	}
	if (!c->fp || !para)
		return 0;
	t = (struct fp_hdmi_timing *)(para + FP_PARA_TIMING);
	/* 1080p24/25/30 and 720p50/60: the frame packing modes of HDMI 1.4 */
	if (!t->pi_mode || (t->vic != 32 && t->vic != 33 && t->vic != 34 &&
			    t->vic != 4 && t->vic != 19) ||
	    (t->v_active != 1080 && t->v_active != 720))
		return 0;
	t->v_active = t->v_active * 2 + t->v_blank;
	t->v_total *= 2;
	t->pixel_freq *= 2;
	pix = t->pixel_freq;
	/*
	 * 8 bit: the frame packed 1080p24 clock is that of 1080p60 already;
	 * 4:2:0 (left in the attr by a 4K desktop mode) is not allowed at 1080
	 */
	*(u32 *)(para + FP_PARA_CD) = FP_COLORDEPTH_24B;
	if (*(u32 *)(para + FP_PARA_CS) == FP_CS_YUV420)
		*(u32 *)(para + FP_PARA_CS) = FP_CS_YUV444;
	*(u32 *)(para + FP_PARA_TMDS) = pix;
	*(u32 *)(para + FP_PARA_VINFO + FP_VINFO_HEIGHT) = t->v_active;
	*(u32 *)(para + FP_PARA_VINFO + FP_VINFO_FIELD_HEIGHT) = t->v_active;
	*(u32 *)(para + FP_PARA_VINFO + FP_VINFO_VTOTAL) = t->v_total;
	*(u32 *)(para + FP_PARA_VINFO + FP_VINFO_VIDEO_CLK) = pix;
	*(u32 *)(para + FP_PARA_VINFO + FP_VINFO_INFO_3D) = FP_3D_INFO;
	fp_applied++;
	return 0;
}

static struct kretprobe fmtpara_probe = {
	.kp.symbol_name = "hdmitx21_get_fmtpara",
	.entry_handler = fmtpara_entry,
	.handler = fmtpara_ret,
	.data_size = sizeof(struct fp_call_data),
	.maxactive = 8,
};

/* "3dfp" / "3doff" into the hdmitx config: the 3D vendor infoframe (and the flag) */
static void fp_work_fn(struct work_struct *work)
{
	const char *cmd = fp_work_on ? "3dfp" : "3doff";
	struct file *f = filp_open("/sys/class/amhdmitx/amhdmitx0/config", O_WRONLY, 0);
	loff_t pos = 0;

	if (IS_ERR(f)) {
		pr_err(DRV_NAME ": hdmitx config: %ld\n", PTR_ERR(f));
		return;
	}
	kernel_write(f, cmd, strlen(cmd), &pos);
	filp_close(f, NULL);
	pr_info(DRV_NAME ": HDMI frame packing %s\n", fp_work_on ? "on" : "off");
}

/*
 * The encoder (ENCP) timing comes from the VIC table, not from the format:
 * set_tv_encp_new() (config_tv_enc_calc() inlined) looks it up with
 * hdmitx21_gettiming_from_vic(). For that caller only, the lookup returns a
 * copy with the frame packed vertical timing (the PLL callers keep the 2D
 * timing: the 3D clock table of "3dfp" doubles the clock already).
 */
static struct fp_hdmi_timing fp_enc_timing;
static u64 fp_enc_applied;

static int timing_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	char sym[KSYM_SYMBOL_LEN];
	bool *from_enc = (bool *)ri->data;

	*from_enc = false;
	if (!READ_ONCE(fp_out))
		return 0;
	/* the caller: the return address in the link register */
	sprint_symbol(sym, regs->regs[30]);
	*from_enc = !strncmp(sym, "set_tv_encp_new+", 16);
	return 0;
}

static int timing_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	const struct fp_hdmi_timing *t = (const void *)regs_return_value(regs);

	if (!*(bool *)ri->data || !t || !t->pi_mode ||
	    (t->vic != 32 && t->vic != 33 && t->vic != 34 && t->vic != 4 && t->vic != 19) ||
	    (t->v_active != 1080 && t->v_active != 720))
		return 0;
	fp_enc_timing = *t;
	fp_enc_timing.v_active = t->v_active * 2 + t->v_blank;
	fp_enc_timing.v_total = t->v_total * 2;
	fp_enc_timing.pixel_freq = t->pixel_freq * 2;
	regs_set_return_value(regs, (unsigned long)&fp_enc_timing);
	fp_enc_applied++;
	return 0;
}

static struct kretprobe timing_probe = {
	.kp.symbol_name = "hdmitx21_gettiming_from_vic",
	.entry_handler = timing_entry,
	.handler = timing_ret,
	.data_size = sizeof(bool),
	.maxactive = 16,
};




#define m3_dbg(fmt, ...) \
	do { if (debug) pr_info(DRV_NAME ": " fmt, ##__VA_ARGS__); } while (0)

/* BT.709, limited range, colour description present */
#define SIGNAL_BT709 ((1 << 29) | (5 << 26) | (1 << 24) | (1 << 16) | (1 << 8) | 1)

int get_vsync_count(unsigned char reset);
int is_in_vsync_isr(void);

enum buf_state {
	BUF_FREE,	/* may be dequeued */
	BUF_USER,	/* dequeued: user space owns it */
	BUF_QUEUED,	/* named for a job, waits for the display */
	BUF_SHOWN,	/* taken by the display */
	BUF_HELD,	/* on screen at a reset: the display or the keeper may still show it */
};

struct m3_buf {
	struct codec_mm_s *mm;
	ulong phys;
	enum buf_state state;
	struct vframe_s vf;
};

enum job_state { JOB_POSTED, JOB_TAKEN, JOB_DONE };

struct m3_job {
	enum job_state state;
	u32 gen;
	int buf;		/* JOB_DONE: the picture, or MVC3D_SHOW_DROP */
	u32 w, h;
	ktime_t posted;
	u32 due;
	bool hold;
	struct mvc3d_job info;
	struct vframe_s snap;	/* the upstream frame (its pointers cleared) */
};

/* output queue entry: one of our buffers, or an upstream frame passed through */
struct m3_out {
	struct vframe_s *vf;
	int buf;		/* -1: passthrough */
	u32 gen;
	u32 due;
	bool hold;
};

struct m3_dev {
	struct miscdevice misc;
	struct mutex mlock;		/* ioctl ALLOC, client registration */
	spinlock_t lock;		/* everything below */
	wait_queue_head_t wq_client, wq_thread;
	bool kick;
	struct task_struct *thread;

	/* client */
	struct file *owner;
	bool client_ready;
	u32 client_lead_us;
	u32 job_id;

	/* buffers */
	struct m3_buf bufs[MVC3D_MAX_BUFS];
	int nbufs;
	u32 w, h, fmt, stride, size, map_size;
	struct delayed_work free_work;

	/* upstream */
	struct vframe_receiver_s recv;
	struct vframe_provider_s prov;
	bool prov_reg, vc_upstream, new_stream, shown_new;
	u32 gen, in_seq;
	struct vframe_s *up_held;	/* early release: the newest upstream frame */
	struct vframe_s *pass[QUEUE_LEN];	/* passed through, not yet returned */
	u32 pass_gen[QUEUE_LEN];
	u32 get_vsync;
	int gets_this_vsync;
	int dump_left;
	bool vc_on;
	struct work_struct vc_work;

	/* jobs (display order) and output queue */
	struct m3_job jobs[QUEUE_LEN];
	int job_head, job_cnt;
	struct m3_out outq[QUEUE_LEN];
	int out_head, out_cnt;

	/* vsync timeline, lead time (as in dvfel) */
	ktime_t vs_ts;
	u32 vs_seq;
	s64 vs_period;
	int vs_odd;
	bool ch_valid;
	u64 ch_pts;
	u32 ch_orig;
	int ch_late;
	u32 lead_vs;

	/* stats */
	u64 frames_in, jobs_posted, shown, dropped_client, dropped_timeout, passed;
	u64 slot_waits, in_gap[5], disp_gap[5], av_hist[6], av_cnt, lead_frames, lead_late;
	u32 in_vsync, disp_vsync;
	bool in_any, disp_any;
	s64 us_answer_sum, us_answer_max;
	u64 us_answer_cnt;
	struct dentry *dbg;
};

static struct m3_dev *gdev;

/* ------------------------------------------------------------------ */
/* buffers                                                            */
/* ------------------------------------------------------------------ */

static bool buf_kept(struct m3_buf *b)
{
	return b->mm && atomic_read(&b->mm->use_cnt) > 1;
}

/*
 * a buffer that was on screen at a reset is written again only once a
 * picture of the new stream went out and the keeper does not hold it
 * (d->lock held)
 */
static bool buf_free_locked(struct m3_dev *d, struct m3_buf *b)
{
	if (b->state == BUF_HELD && !buf_kept(b) && (d->shown_new || !d->prov_reg))
		b->state = BUF_FREE;
	return b->state == BUF_FREE;
}

static bool bufs_busy_locked(struct m3_dev *d)
{
	int i;

	for (i = 0; i < d->nbufs; i++) {
		struct m3_buf *b = &d->bufs[i];

		if (b->state == BUF_QUEUED || b->state == BUF_SHOWN ||
		    (b->state == BUF_HELD && !buf_free_locked(d, b)))
			return true;
	}
	return false;
}

static void free_bufs(struct m3_dev *d)
{
	int i;

	/* a keeper reference keeps its frame alive past this */
	for (i = 0; i < d->nbufs; i++) {
		if (d->bufs[i].mm)
			codec_mm_release(d->bufs[i].mm, DRV_NAME);
		d->bufs[i].mm = NULL;
		d->bufs[i].phys = 0;
		d->bufs[i].state = BUF_FREE;
	}
	d->nbufs = 0;
	d->map_size = 0;
}

static void free_work_fn(struct work_struct *work)
{
	struct m3_dev *d = container_of(to_delayed_work(work), struct m3_dev, free_work);
	unsigned long flags;
	bool busy;

	mutex_lock(&d->mlock);
	spin_lock_irqsave(&d->lock, flags);
	busy = d->owner || bufs_busy_locked(d);
	spin_unlock_irqrestore(&d->lock, flags);
	if (!busy && d->nbufs) {
		free_bufs(d);
		pr_info(DRV_NAME ": buffers released\n");
	} else if (!d->owner && d->nbufs) {
		schedule_delayed_work(&d->free_work, msecs_to_jiffies(3000));
	}
	mutex_unlock(&d->mlock);
}

static struct m3_buf *buf_of(struct m3_dev *d, struct vframe_s *vf)
{
	int i;

	for (i = 0; i < d->nbufs; i++)
		if (vf == &d->bufs[i].vf)
			return &d->bufs[i];
	return NULL;
}

/* the display frame of buffer @idx for the upstream frame @snap */
static void build_vf(struct m3_dev *d, int idx, const struct vframe_s *snap, u32 w, u32 h)
{
	struct m3_buf *b = &d->bufs[idx];
	struct vframe_s *o = &b->vf;

	/* timing, axis and signal of the composer's frame; the picture is ours */
	*o = *snap;
	INIT_LIST_HEAD(&o->list);
	o->type = VIDTYPE_PROGRESSIVE | VIDTYPE_VIU_FIELD | VIDTYPE_VIU_422 |
		  VIDTYPE_VIU_SINGLE_PLANE;
	o->type_ext = 0;
	o->type_backup = o->type;
	o->type_original = o->type;
	o->flag = (snap->flag & (VFRAME_FLAG_VIDEO_COMPOSER | VFRAME_FLAG_VIDEO_COMPOSER_BYPASS)) |
		  VFRAME_FLAG_VIDEO_LINEAR;
	o->bitdepth = BITDEPTH_Y8 | BITDEPTH_U8 | BITDEPTH_V8;
	if (!o->signal_type)
		o->signal_type = SIGNAL_BT709;
	o->compHeadAddr = 0;
	o->compBodyAddr = 0;
	o->compWidth = 0;
	o->compHeight = 0;
	o->width = w;
	o->height = h;
	memset(o->crop, 0, sizeof(o->crop));
	/*
	 * frame packed output: the picture is the whole frame, 1:1 (the
	 * window the composer gives is in the coordinates of the 2D mode)
	 */
	if (READ_ONCE(fp_out) && h > 1400) {
		o->axis[0] = 0;
		o->axis[1] = 0;
		o->axis[2] = w - 1;
		o->axis[3] = h - 1;
	}
	o->canvas0Addr = (u32)-1;
	o->canvas1Addr = (u32)-1;
	o->plane_num = 1;
	memset(o->canvas0_config, 0, sizeof(o->canvas0_config));
	o->canvas0_config[0].phy_addr = b->phys;
	o->canvas0_config[0].width = d->stride;
	o->canvas0_config[0].height = h;
	o->canvas0_config[0].block_mode = CANVAS_BLKMODE_LINEAR;
	memcpy(o->canvas1_config, o->canvas0_config, sizeof(o->canvas1_config));
	/* the video keeper holds the frame on screen by this handle */
	o->mem_handle = b->mm;
	o->mem_handle_1 = NULL;
	o->mem_head_handle = NULL;
	o->mem_dw_handle = NULL;
}

/* ------------------------------------------------------------------ */
/* vsync timeline and lead time (d->lock held), from dvfel            */
/* ------------------------------------------------------------------ */

/* in the display's vsync interrupt: count it, track the period */
static void vs_tick_locked(struct m3_dev *d)
{
	ktime_t now;
	s64 dt, p;
	u32 k;

	if (!is_in_vsync_isr())
		return;
	now = ktime_get();
	if (!d->vs_ts) {
		d->vs_ts = now;
		d->vs_seq++;
		return;
	}
	dt = ktime_to_ns(ktime_sub(now, d->vs_ts));
	if (dt < 3 * NSEC_PER_MSEC)
		return;		/* the same interrupt */
	p = d->vs_period;
	if (p <= 0 || dt > 10 * p) {
		/* first interval or after a pause of the display */
		k = 1;
		if (dt <= 100 * NSEC_PER_MSEC)
			d->vs_period = dt;
	} else {
		k = div64_s64(dt + p / 2, p);
		if (!k)
			k = 1;
		if (abs(dt - (s64)k * p) > p / 4) {
			/* a new display rate (AFR) after a few such intervals */
			if (++d->vs_odd >= 3) {
				d->vs_period = dt;
				d->vs_odd = 0;
				k = 1;
			}
		} else {
			d->vs_odd = 0;
			if (k == 1)
				d->vs_period = p + (dt - p) / 8;
		}
	}
	d->vs_seq += k;
	vsync_us = (int)div64_s64(d->vs_period, NSEC_PER_USEC);
	d->vs_ts = now;
}

/* the vsync (vs_seq) at time now and how long after it now is */
static u32 vs_at(struct m3_dev *d, ktime_t now, s64 *phase)
{
	s64 dt = ktime_to_ns(ktime_sub(now, d->vs_ts)), p = d->vs_period;
	u32 n = 0;

	if (p > 0 && dt >= p) {
		n = div64_s64(dt, p);
		dt -= (s64)n * p;
	}
	if (phase)
		*phase = dt;
	return d->vs_seq + n;
}

/*
 * the due vsync of a frame taken from the video composer now: the vsync the
 * display would have shown it in itself, plus the compositor's lead. Frames
 * come in at the same phase of the vsync (SurfaceFlinger), so the intervals
 * of their arrival times (the composer's pts_us64) are whole vsyncs: a frame
 * taken late (no free job) keeps its due vsync from the chain.
 */
static u32 due_of_locked(struct m3_dev *d, struct vframe_s *vf, bool *hold)
{
	s64 phase, p = d->vs_period;
	u32 orig, lead_vs = 0;

	*hold = false;
	if (!d->vs_ts || p <= 0)
		return 0;
	orig = vs_at(d, ktime_get(), &phase);
	if (phase >= (s64)lead_phase_us * NSEC_PER_USEC)
		orig++;
	if (d->ch_valid && vf->pts_us64 > d->ch_pts &&
	    vf->pts_us64 - d->ch_pts < USEC_PER_SEC) {
		s64 da = (s64)(vf->pts_us64 - d->ch_pts) * NSEC_PER_USEC;
		u32 c = d->ch_orig + (u32)div64_s64(da + p / 2, p);

		if ((s32)(orig - c) > 0) {
			if (++d->ch_late <= 16)
				orig = c;
		} else {
			d->ch_late = 0;
		}
	}
	d->ch_valid = true;
	d->ch_pts = vf->pts_us64;
	d->ch_orig = orig;
	if (d->client_ready && d->client_lead_us) {
		lead_vs = (u32)div64_s64((s64)d->client_lead_us * NSEC_PER_USEC + p / 2, p);
		*hold = lead_vs > 0;
	}
	d->lead_vs = lead_vs;
	return orig + lead_vs;
}

/* the display may take outq entry i (from the middle of the period before due) */
static bool outq_visible_locked(struct m3_dev *d, int i)
{
	struct m3_out *e = &d->outq[(d->out_head + i) % QUEUE_LEN];
	s64 p = d->vs_period, rel;
	s32 ahead;
	ktime_t now;

	if (!e->hold || p <= 0)
		return true;
	now = ktime_get();
	if (ktime_to_ns(ktime_sub(now, d->vs_ts)) > 200 * NSEC_PER_MSEC)
		return true;	/* no vsync interrupts: do not stall */
	ahead = (s32)(e->due - d->vs_seq);
	if (ahead > 2 + (s32)d->lead_vs)
		return true;	/* cannot be right */
	rel = (s64)(ahead - 1) * p + p / 2;
	return ktime_to_ns(ktime_sub(now, d->vs_ts)) >= rel;
}

/* ------------------------------------------------------------------ */
/* queues (d->lock held)                                              */
/* ------------------------------------------------------------------ */

static void outq_push(struct m3_dev *d, struct vframe_s *vf, int buf, u32 due, bool hold)
{
	struct m3_out *e = &d->outq[(d->out_head + d->out_cnt) % QUEUE_LEN];

	e->vf = vf;
	e->buf = buf;
	e->gen = d->gen;
	e->due = due;
	e->hold = hold;
	d->out_cnt++;
}

static int pass_free(struct m3_dev *d)
{
	int i;

	for (i = 0; i < QUEUE_LEN; i++)
		if (!d->pass[i])
			return i;
	return -1;
}

/* drop queued pictures and jobs: a new stream (or none) follows */
static void reset_locked(struct m3_dev *d)
{
	int i;

	d->gen++;
	d->new_stream = true;
	d->shown_new = false;
	d->ch_valid = false;
	d->in_seq = 0;
	while (d->out_cnt) {
		struct m3_out *e = &d->outq[d->out_head];

		if (e->buf >= 0)
			d->bufs[e->buf].state = BUF_FREE;
		d->out_head = (d->out_head + 1) % QUEUE_LEN;
		d->out_cnt--;
	}
	/* user space keeps the buffers of jobs it answers late (SHOW fails) */
	for (i = 0; i < d->job_cnt; i++) {
		struct m3_job *j = &d->jobs[(d->job_head + i) % QUEUE_LEN];

		if (j->state == JOB_DONE && j->buf >= 0)
			d->bufs[j->buf].state = BUF_FREE;
	}
	d->job_head = d->job_cnt = 0;
	for (i = 0; i < d->nbufs; i++)
		if (d->bufs[i].state == BUF_SHOWN)
			d->bufs[i].state = BUF_HELD;
	/* frames passed through are not returned to a reset upstream */
	memset(d->pass, 0, sizeof(d->pass));
}

/* ------------------------------------------------------------------ */
/* the vfm map                                                        */
/* ------------------------------------------------------------------ */

/* process context; the map may change only while nothing plays through it */
static void vc_path_apply(struct m3_dev *d)
{
	char id[64], chain[128];
	bool want = d->client_ready || vc_route;

	if (want == d->vc_on)
		return;
	if (d->vc_on && d->prov_reg)
		return;		/* retried at the upstream UNREG */
	strscpy(id, vc_map_id, sizeof(id));
	strscpy(chain, want ? VC_MAP_ON : vc_map_off, sizeof(chain));
	vfm_map_remove(id);
	if (vfm_map_add(id, chain) < 0) {
		pr_err(DRV_NAME ": cannot set vfm map %s\n", id);
		return;
	}
	d->vc_on = want;
	pr_info(DRV_NAME ": video_composer.0 %s mvc3d\n", want ? "routed through" : "no longer through");
}

static void vc_path_work(struct work_struct *work)
{
	struct m3_dev *d = container_of(work, struct m3_dev, vc_work);

	mutex_lock(&d->mlock);
	vc_path_apply(d);
	mutex_unlock(&d->mlock);
}

/* ------------------------------------------------------------------ */
/* the thread: upstream frames -> jobs -> output queue                */
/* ------------------------------------------------------------------ */

static bool vc_may_get(struct m3_dev *d)
{
	struct vframe_provider_s *p;
	struct vframe_states st;
	u32 now;

	if (!vc_pace || !d->vc_upstream)
		return true;
	now = get_vsync_count(0);
	if (now != d->get_vsync) {
		d->get_vsync = now;
		d->gets_this_vsync = 0;
	}
	if (!d->gets_this_vsync)
		return true;
	if (d->gets_this_vsync >= 2)
		return false;
	p = vf_get_provider(DRV_NAME);
	return p && !vf_get_states(p, &st) && st.buf_avail_num >= 2;
}

/* the decoder picture behind a video composer frame */
static struct vframe_s *src_of(struct vframe_s *vf)
{
	/* post processed (deinterlacer) copy: the decoder frame is linked to it */
	if ((vf->type & VIDTYPE_DI_PW) && vf->vf_ext)
		return vf->vf_ext;
	return vf;
}

static void dump_in(struct m3_dev *d, struct vframe_s *vf)
{
	struct vframe_s *s = src_of(vf);

	pr_info(DRV_NAME ": in #%llu idx %u fi %u type 0x%x flag 0x%x %ux%u pts %llu ts %llu dur %u | src %s idx %u fi %u type 0x%x flag 0x%x %ux%u pts %u pts_us %llu ts %llu disp %llu\n",
		d->frames_in, vf->index, vf->frame_index, vf->type, vf->flag, vf->width,
		vf->height, vf->pts_us64, vf->timestamp, vf->duration,
		s == vf ? "self" : "ext", s->index, s->frame_index, s->type, s->flag,
		s->width, s->height, s->pts, s->pts_us64, s->timestamp, s->disp_pts_us64);
}

/* early release: give the frame back when the next one comes (d->lock not held) */
static void up_hold(struct m3_dev *d, struct vframe_s *vf)
{
	struct vframe_s *prev;
	unsigned long flags;

	/*
	 * The video composer signals the buffer's release fence only for
	 * rendered frames: without this the player runs out of buffers.
	 */
	vf->rendered = true;
	spin_lock_irqsave(&d->lock, flags);
	prev = d->up_held;
	d->up_held = vf;
	spin_unlock_irqrestore(&d->lock, flags);
	if (prev)
		vf_put(prev, DRV_NAME);
}

static void up_release_held(struct m3_dev *d)
{
	struct vframe_s *vf;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	vf = d->up_held;
	d->up_held = NULL;
	spin_unlock_irqrestore(&d->lock, flags);
	if (vf)
		vf_put(vf, DRV_NAME);
}

static void snap_frame(struct vframe_s *snap, struct vframe_s *vf)
{
	*snap = *vf;
	INIT_LIST_HEAD(&snap->list);
	/* state of the upstream frame, gone with it */
	memset(&snap->src_fmt, 0, sizeof(snap->src_fmt));
	snap->vf_ext = NULL;
	snap->vc_private = NULL;
	snap->file_vf = NULL;
	snap->fence = NULL;
	snap->hf_info = NULL;
	snap->di_hd = NULL;
	snap->composer_info = NULL;
	snap->decontour_pre = NULL;
	snap->hdr10p_data_buf = NULL;
	snap->hdr10p_data_size = 0;
	snap->meta_data_buf = NULL;
	snap->meta_data_size = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	snap->uvm_vf = NULL;
#endif
	snap->early_process_fun = NULL;
	snap->process_fun = NULL;
	snap->private_data = NULL;
	snap->mem_handle = NULL;
	snap->mem_handle_1 = NULL;
	snap->mem_head_handle = NULL;
	snap->mem_dw_handle = NULL;
	snap->fgs_valid = false;
}

/* finished or timed out jobs at the head go to the output queue (d->lock held) */
static bool jobs_out_locked(struct m3_dev *d)
{
	bool any = false;

	while (d->job_cnt && d->out_cnt < QUEUE_LEN) {
		struct m3_job *j = &d->jobs[d->job_head];

		if (j->state != JOB_DONE) {
			if (d->client_ready &&
			    ktime_ms_delta(ktime_get(), j->posted) < show_timeout_ms)
				break;
			d->dropped_timeout++;
		} else if (j->buf >= 0) {
			build_vf(d, j->buf, &j->snap, j->w, j->h);
			outq_push(d, &d->bufs[j->buf].vf, j->buf, j->due, j->hold);
			any = true;
		} else {
			d->dropped_client++;
		}
		d->job_head = (d->job_head + 1) % QUEUE_LEN;
		d->job_cnt--;
	}
	return any;
}

/* the next job's deadline, for the thread's wait (d->lock held) */
static long head_wait_locked(struct m3_dev *d)
{
	s64 left;

	if (!d->job_cnt || d->jobs[d->job_head].state == JOB_DONE)
		return MAX_SCHEDULE_TIMEOUT;
	left = show_timeout_ms - ktime_ms_delta(ktime_get(), d->jobs[d->job_head].posted);
	return max_t(long, msecs_to_jiffies(max_t(s64, left, 1)), 1);
}

static int m3_thread(void *data)
{
	struct m3_dev *d = data;

	while (!kthread_should_stop()) {
		unsigned long flags;
		long wait;
		bool ready;

		spin_lock_irqsave(&d->lock, flags);
		wait = head_wait_locked(d);
		spin_unlock_irqrestore(&d->lock, flags);
		/* the video composer releases a frame at its display time: poll it */
		if (d->vc_upstream && d->prov_reg)
			wait = min_t(long, wait, max_t(long, usecs_to_jiffies(2000), 1));
		wait_event_interruptible_timeout(d->wq_thread, d->kick || kthread_should_stop(), wait);
		d->kick = false;

		spin_lock_irqsave(&d->lock, flags);
		ready = jobs_out_locked(d);
		spin_unlock_irqrestore(&d->lock, flags);

		for (;;) {
			struct vframe_s *vf;
			bool job, hold = false;
			u32 due = 0, gen;
			int pi = -1;

			if (!vc_may_get(d))
				break;
			spin_lock_irqsave(&d->lock, flags);
			job = d->client_ready && d->nbufs;
			if (!d->prov_reg || (job && d->job_cnt >= QUEUE_LEN) ||
			    (!job && (pi = pass_free(d)) < 0) || d->out_cnt >= QUEUE_LEN) {
				if (d->prov_reg && job)
					d->slot_waits++;
				spin_unlock_irqrestore(&d->lock, flags);
				break;
			}
			gen = d->gen;
			spin_unlock_irqrestore(&d->lock, flags);

			vf = vf_peek(DRV_NAME) ? vf_get(DRV_NAME) : NULL;
			if (!vf)
				break;
			d->frames_in++;
			d->gets_this_vsync++;
			if ((debug & 4) && d->dump_left > 0) {
				d->dump_left--;
				dump_in(d, vf);
			}
			if (d->vc_upstream) {
				u32 nv = (u32)get_vsync_count(0);

				if (d->in_any)
					d->in_gap[min_t(u32, nv - d->in_vsync, 4)]++;
				d->in_vsync = nv;
				d->in_any = true;
			}

			spin_lock_irqsave(&d->lock, flags);
			/* a reset meanwhile: the frame belongs to the new stream */
			gen = d->gen;
			if (d->vc_upstream)
				due = due_of_locked(d, vf, &hold);
			if (job && d->client_ready && d->job_cnt < QUEUE_LEN) {
				struct m3_job *j = &d->jobs[(d->job_head + d->job_cnt) % QUEUE_LEN];
				struct vframe_s *s = src_of(vf);

				memset(&j->info, 0, sizeof(j->info));
				j->info.id = ++d->job_id;
				j->info.flags = d->new_stream ? MVC3D_JOB_NEW_STREAM : 0;
				j->info.seq = d->in_seq++;
				j->info.src_index = s->frame_index;
				j->info.width = vf->width;
				j->info.height = vf->height;
				j->info.src_type = vf->type;
				j->info.pts_us = s->pts_us64;
				j->info.timestamp = s->timestamp;
				j->info.arrival_us = vf->pts_us64;
				j->info.taken_ns = ktime_get_ns();
				snap_frame(&j->snap, vf);
				j->state = JOB_POSTED;
				j->gen = gen;
				j->buf = MVC3D_SHOW_DROP;
				j->posted = ktime_get();
				j->due = due;
				j->hold = hold;
				d->new_stream = false;
				d->job_cnt++;
				d->jobs_posted++;
				spin_unlock_irqrestore(&d->lock, flags);
				/* the snapshot stands in for vf from here on */
				up_hold(d, vf);
				wake_up_interruptible(&d->wq_client);
				continue;
			}
			/* no compositor (it went away while this stream plays): pass through */
			if (pi < 0)
				pi = pass_free(d);
			if (pi >= 0 && d->out_cnt < QUEUE_LEN) {
				d->pass[pi] = vf;
				d->pass_gen[pi] = gen;
				outq_push(d, vf, -1, due, false);
				d->passed++;
				ready = true;
				spin_unlock_irqrestore(&d->lock, flags);
				continue;
			}
			spin_unlock_irqrestore(&d->lock, flags);
			vf_put(vf, DRV_NAME);
		}
		if (ready)
			vf_notify_receiver(DRV_NAME, VFRAME_EVENT_PROVIDER_VFRAME_READY, NULL);
	}
	return 0;
}

static void m3_kick(struct m3_dev *d)
{
	d->kick = true;
	wake_up_interruptible(&d->wq_thread);
}

/* ------------------------------------------------------------------ */
/* provider side (the display calls us, often from the vsync irq)     */
/* ------------------------------------------------------------------ */

static struct vframe_s *m3_peek(void *op_arg)
{
	struct m3_dev *d = op_arg;
	struct vframe_s *vf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	vs_tick_locked(d);
	if (d->out_cnt && outq_visible_locked(d, 0))
		vf = d->outq[d->out_head].vf;
	spin_unlock_irqrestore(&d->lock, flags);
	return vf;
}

static struct vframe_s *m3_get(void *op_arg)
{
	struct m3_dev *d = op_arg;
	struct vframe_s *vf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	vs_tick_locked(d);
	if (d->out_cnt && outq_visible_locked(d, 0)) {
		struct m3_out *e = &d->outq[d->out_head];

		vf = e->vf;
		if (e->buf >= 0)
			d->bufs[e->buf].state = BUF_SHOWN;
		d->out_head = (d->out_head + 1) % QUEUE_LEN;
		d->out_cnt--;
		d->shown++;
		d->shown_new = true;
		if (d->vc_upstream && d->vs_ts && d->vs_period > 0 && is_in_vsync_isr()) {
			/* A/V: the vsync shown in minus the due one */
			s32 late = (s32)(d->vs_seq - e->due);
			u32 now = (u32)get_vsync_count(0);

			d->av_hist[late < 0 ? 0 : 1 + min_t(s32, late, 4)]++;
			d->av_cnt++;
			if (e->hold) {
				d->lead_frames++;
				if (late > 0)
					d->lead_late++;
			}
			if (d->disp_any)
				d->disp_gap[min_t(u32, now - d->disp_vsync, 4)]++;
			d->disp_vsync = now;
			d->disp_any = true;
		}
	}
	spin_unlock_irqrestore(&d->lock, flags);
	if (vf)
		m3_kick(d);
	return vf;
}

static void m3_put(struct vframe_s *vf, void *op_arg)
{
	struct m3_dev *d = op_arg;
	struct vframe_s *upstream = NULL;
	struct m3_buf *b;
	unsigned long flags;
	int i;

	if (!vf)
		return;
	spin_lock_irqsave(&d->lock, flags);
	b = buf_of(d, vf);
	if (b) {
		/* returned by the display: only the keeper may still hold it */
		if (b->state == BUF_SHOWN || b->state == BUF_HELD)
			b->state = buf_kept(b) ? BUF_HELD : BUF_FREE;
	} else {
		for (i = 0; i < QUEUE_LEN; i++) {
			if (d->pass[i] == vf) {
				if (d->pass_gen[i] == d->gen)
					upstream = vf;
				d->pass[i] = NULL;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&d->lock, flags);
	if (upstream && d->prov_reg)
		vf_put(upstream, DRV_NAME);
	wake_up_interruptible(&d->wq_client);
	m3_kick(d);
}

static int m3_prov_event(int type, void *data, void *op_arg)
{
	vf_notify_provider(DRV_NAME, type, data);
	return 0;
}

static int m3_vf_states(struct vframe_states *states, void *op_arg)
{
	struct m3_dev *d = op_arg;
	unsigned long flags;
	int i, nfree = 0;

	spin_lock_irqsave(&d->lock, flags);
	for (i = 0; i < d->nbufs; i++)
		if (d->bufs[i].state == BUF_FREE)
			nfree++;
	states->vf_pool_size = max(d->nbufs, 1);
	states->buf_free_num = nfree;
	states->buf_recycle_num = 0;
	states->buf_avail_num = 0;
	for (i = 0; i < d->out_cnt && outq_visible_locked(d, i); i++)
		states->buf_avail_num++;
	spin_unlock_irqrestore(&d->lock, flags);
	return 0;
}

static const struct vframe_operations_s m3_vf_ops = {
	.peek = m3_peek,
	.get = m3_get,
	.put = m3_put,
	.event_cb = m3_prov_event,
	.vf_states = m3_vf_states,
};

/* ------------------------------------------------------------------ */
/* receiver side (upstream events)                                    */
/* ------------------------------------------------------------------ */

static int m3_recv_event(int type, void *data, void *op_arg)
{
	struct m3_dev *d = op_arg;
	unsigned long flags;

	if (type != VFRAME_EVENT_PROVIDER_VFRAME_READY)
		m3_dbg("upstream event 0x%x\n", type);
	switch (type) {
	case VFRAME_EVENT_PROVIDER_REG:
		m3_dbg("upstream REG %s\n", data ? (char *)data : "");
		spin_lock_irqsave(&d->lock, flags);
		reset_locked(d);
		d->up_held = NULL;
		d->prov_reg = true;
		d->vc_upstream = data && !strncmp(data, "video_composer", 14);
		d->dump_left = 40;
		spin_unlock_irqrestore(&d->lock, flags);
		vf_reg_provider(&d->prov);
		m3_kick(d);
		break;
	case VFRAME_EVENT_PROVIDER_UNREG:
		spin_lock_irqsave(&d->lock, flags);
		d->prov_reg = false;
		/* the provider is gone (vf_put cannot reach it): it cleans up itself */
		d->up_held = NULL;
		reset_locked(d);
		spin_unlock_irqrestore(&d->lock, flags);
		vf_unreg_provider(&d->prov);
		wake_up_interruptible(&d->wq_client);
		/* a vfm map change postponed while playing */
		schedule_work(&d->vc_work);
		if (!d->owner)
			schedule_delayed_work(&d->free_work, msecs_to_jiffies(3000));
		break;
	case VFRAME_EVENT_PROVIDER_LIGHT_UNREG:
		up_release_held(d);
		spin_lock_irqsave(&d->lock, flags);
		reset_locked(d);
		spin_unlock_irqrestore(&d->lock, flags);
		vf_light_unreg_provider(&d->prov);
		wake_up_interruptible(&d->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_RESET:
		up_release_held(d);
		spin_lock_irqsave(&d->lock, flags);
		reset_locked(d);
		spin_unlock_irqrestore(&d->lock, flags);
		vf_notify_receiver(DRV_NAME, type, data);
		wake_up_interruptible(&d->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_VFRAME_READY:
		m3_kick(d);
		break;
	case VFRAME_EVENT_PROVIDER_QUREY_STATE:
		return RECEIVER_ACTIVE;
	default:
		/* START, FR_HINT, FR_END_HINT, ... */
		return vf_notify_receiver(DRV_NAME, type, data);
	}
	return 0;
}

static const struct vframe_receiver_op_s m3_recv_ops = {
	.event_cb = m3_recv_event,
};

/* ------------------------------------------------------------------ */
/* /dev/mvc3d                                                         */
/* ------------------------------------------------------------------ */

static long ioc_alloc(struct m3_dev *d, void __user *arg)
{
	struct mvc3d_alloc a;
	unsigned long flags;
	u32 stride, size;
	bool busy;
	int i;

	if (copy_from_user(&a, arg, sizeof(a)))
		return -EFAULT;
	if (a.width < 64 || a.width > 4096 || a.height < 64 || a.height > 4400 ||
	    (a.width & 1) || a.count < 2 || a.count > MVC3D_MAX_BUFS ||
	    a.format != MVC3D_FMT_UYVY)
		return -EINVAL;
	stride = ALIGN(a.width * 2, 64);
	size = stride * a.height;

	cancel_delayed_work_sync(&d->free_work);
	spin_lock_irqsave(&d->lock, flags);
	busy = bufs_busy_locked(d);
	spin_unlock_irqrestore(&d->lock, flags);
	if (d->nbufs && (d->nbufs != a.count || d->w != a.width || d->h != a.height ||
			 d->client_ready)) {
		/* another layout: the display may still hold the old buffers */
		if (busy || d->client_ready)
			return -EBUSY;
		free_bufs(d);
	}
	if (!d->nbufs) {
		d->map_size = PAGE_ALIGN(size);
		for (i = 0; i < a.count; i++) {
			struct m3_buf *b = &d->bufs[i];

			b->mm = codec_mm_alloc(DRV_NAME, d->map_size, 0, CODEC_MM_FLAGS_DMA);
			if (!b->mm) {
				pr_err(DRV_NAME ": allocation of buffer %d (%u KiB) failed\n",
				       i, d->map_size >> 10);
				d->nbufs = i;
				free_bufs(d);
				return -ENOMEM;
			}
			b->phys = b->mm->phy_addr;
			b->state = BUF_FREE;
		}
		pr_info(DRV_NAME ": %d buffers %ux%u uyvy, %u KiB each\n", a.count, a.width,
			a.height, d->map_size >> 10);
	}
	spin_lock_irqsave(&d->lock, flags);
	d->nbufs = a.count;
	d->w = a.width;
	d->h = a.height;
	d->fmt = a.format;
	d->stride = stride;
	d->size = size;
	for (i = 0; i < d->nbufs; i++)
		if (d->bufs[i].state == BUF_USER)
			d->bufs[i].state = BUF_FREE;
	d->client_ready = true;
	d->client_lead_us = 0;
	d->new_stream = true;
	spin_unlock_irqrestore(&d->lock, flags);
	vc_path_apply(d);

	a.stride = d->stride;
	a.size = d->size;
	a.map_size = d->map_size;
	return copy_to_user(arg, &a, sizeof(a)) ? -EFAULT : 0;
}

static int take_free(struct m3_dev *d)
{
	unsigned long flags;
	int i, idx = -1;

	spin_lock_irqsave(&d->lock, flags);
	for (i = 0; i < d->nbufs; i++) {
		if (buf_free_locked(d, &d->bufs[i])) {
			d->bufs[i].state = BUF_USER;
			idx = i;
			break;
		}
	}
	spin_unlock_irqrestore(&d->lock, flags);
	return idx;
}

static long ioc_dequeue(struct m3_dev *d, void __user *arg)
{
	struct mvc3d_buf f;
	unsigned long deadline;
	long ret;
	int idx;

	if (copy_from_user(&f, arg, sizeof(f)))
		return -EFAULT;
	if (!d->client_ready)
		return -EINVAL;
	deadline = jiffies + msecs_to_jiffies(f.timeout_ms);
	/* a held buffer frees itself without an event: recheck every 10 ms */
	while ((idx = take_free(d)) < 0) {
		if (time_after_eq(jiffies, deadline))
			return -EAGAIN;
		ret = wait_event_interruptible_timeout(d->wq_client, !d->client_ready,
				min_t(unsigned long, deadline - jiffies, msecs_to_jiffies(10)));
		if (ret < 0)
			return ret;
		if (!d->client_ready)
			return -EINVAL;
	}
	f.index = idx;
	return copy_to_user(arg, &f, sizeof(f)) ? -EFAULT : 0;
}

static long ioc_return(struct m3_dev *d, void __user *arg)
{
	struct mvc3d_buf f;
	unsigned long flags;
	long ret = -EINVAL;

	if (copy_from_user(&f, arg, sizeof(f)))
		return -EFAULT;
	spin_lock_irqsave(&d->lock, flags);
	if (f.index >= 0 && f.index < d->nbufs && d->bufs[f.index].state == BUF_USER) {
		d->bufs[f.index].state = BUF_FREE;
		ret = 0;
	}
	spin_unlock_irqrestore(&d->lock, flags);
	return ret;
}

/* the oldest posted job, taken (d->lock held) */
static struct m3_job *job_take_locked(struct m3_dev *d)
{
	int i;

	for (i = 0; i < d->job_cnt; i++) {
		struct m3_job *j = &d->jobs[(d->job_head + i) % QUEUE_LEN];

		if (j->state == JOB_POSTED) {
			j->state = JOB_TAKEN;
			return j;
		}
	}
	return NULL;
}

static long ioc_wait_job(struct m3_dev *d, void __user *arg)
{
	struct mvc3d_job info;
	struct m3_job *j = NULL;
	unsigned long flags;
	u32 timeout;
	long ret;

	if (copy_from_user(&timeout, arg, sizeof(timeout)))
		return -EFAULT;
	ret = wait_event_interruptible_timeout(d->wq_client, ({
			spin_lock_irqsave(&d->lock, flags);
			j = d->client_ready ? job_take_locked(d) : NULL;
			if (j)
				info = j->info;
			spin_unlock_irqrestore(&d->lock, flags);
			j || !d->client_ready; }),
			msecs_to_jiffies(timeout));
	if (ret < 0)
		return ret;
	if (!j)
		return d->client_ready ? -EAGAIN : -EINVAL;
	info.timeout_ms = timeout;
	return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long ioc_show(struct m3_dev *d, void __user *arg)
{
	struct mvc3d_show s;
	unsigned long flags;
	long ret = -ENOENT;
	int i;

	if (copy_from_user(&s, arg, sizeof(s)))
		return -EFAULT;
	if (s.buf != MVC3D_SHOW_DROP && (s.buf < 0 || s.buf >= d->nbufs))
		return -EINVAL;
	spin_lock_irqsave(&d->lock, flags);
	for (i = 0; i < d->job_cnt; i++) {
		struct m3_job *j = &d->jobs[(d->job_head + i) % QUEUE_LEN];
		s64 us;

		if (j->info.id != s.id)
			continue;
		if (j->state != JOB_TAKEN) {
			ret = -EINVAL;
			break;
		}
		if (s.buf >= 0) {
			if (d->bufs[s.buf].state != BUF_USER) {
				ret = -EINVAL;
				break;
			}
			d->bufs[s.buf].state = BUF_QUEUED;
		}
		j->buf = s.buf;
		j->w = s.width && s.width <= d->w ? s.width : d->w;
		j->h = s.height && s.height <= d->h ? s.height : d->h;
		j->state = JOB_DONE;
		us = ktime_us_delta(ktime_get(), j->posted);
		d->us_answer_sum += us;
		d->us_answer_max = max(d->us_answer_max, us);
		d->us_answer_cnt++;
		ret = 0;
		break;
	}
	spin_unlock_irqrestore(&d->lock, flags);
	/* -ENOENT: the job timed out or the stream was reset; the buffer stays the caller's */
	if (!ret)
		m3_kick(d);
	return ret;
}

static long m3_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct m3_dev *d = gdev;
	void __user *p = (void __user *)arg;
	long ret;

	if (d->owner != f)
		return -EBUSY;
	switch (cmd) {
	case MVC3D_IOC_ALLOC:
		mutex_lock(&d->mlock);
		ret = ioc_alloc(d, p);
		mutex_unlock(&d->mlock);
		return ret;
	case MVC3D_IOC_DEQUEUE:
		return ioc_dequeue(d, p);
	case MVC3D_IOC_RETURN:
		return ioc_return(d, p);
	case MVC3D_IOC_WAIT_JOB:
		return ioc_wait_job(d, p);
	case MVC3D_IOC_SHOW:
		return ioc_show(d, p);
	case MVC3D_IOC_SET_FP: {
		u32 on;

		if (copy_from_user(&on, p, sizeof(on)))
			return -EFAULT;
		if (on && !fp_probe_on)
			return -ENODEV;
		WRITE_ONCE(fp_client, !!on);
		pr_info(DRV_NAME ": frame packing %s\n", on ? "wanted" : "not wanted");
		return 0;
	}
	case MVC3D_IOC_SET_LEAD: {
		u32 us;
		unsigned long flags;

		if (copy_from_user(&us, p, sizeof(us)))
			return -EFAULT;
		if (us > 500000)
			return -EINVAL;
		spin_lock_irqsave(&d->lock, flags);
		d->client_lead_us = us;
		spin_unlock_irqrestore(&d->lock, flags);
		pr_info(DRV_NAME ": lead %u us\n", us);
		return 0;
	}
	}
	return -ENOTTY;
}

static int m3_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct m3_dev *d = gdev;
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long idx;
	int ret = -EINVAL;

	mutex_lock(&d->mlock);
	if (d->owner != f || !d->map_size)
		goto out;
	idx = vma->vm_pgoff / (d->map_size >> PAGE_SHIFT);
	if (vma->vm_pgoff % (d->map_size >> PAGE_SHIFT) || idx >= d->nbufs || len > d->map_size)
		goto out;
	/* CPU writes whole lines once: write combining, no cache maintenance */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
	ret = remap_pfn_range(vma, vma->vm_start, d->bufs[idx].phys >> PAGE_SHIFT, len,
			      vma->vm_page_prot);
out:
	mutex_unlock(&d->mlock);
	return ret;
}

static int m3_open(struct inode *inode, struct file *f)
{
	struct m3_dev *d = gdev;
	int ret = 0;

	mutex_lock(&d->mlock);
	if (d->owner)
		ret = -EBUSY;
	else
		d->owner = f;
	mutex_unlock(&d->mlock);
	return ret;
}

/* runs once every mapping is gone too */
static int m3_release(struct inode *inode, struct file *f)
{
	struct m3_dev *d = gdev;
	unsigned long flags;
	int i;

	mutex_lock(&d->mlock);
	if (d->owner == f) {
		spin_lock_irqsave(&d->lock, flags);
		d->client_ready = false;
		d->client_lead_us = 0;
		for (i = 0; i < d->nbufs; i++)
			if (d->bufs[i].state == BUF_USER)
				d->bufs[i].state = BUF_FREE;
		spin_unlock_irqrestore(&d->lock, flags);
		/* the next mode the firmware sets (end of playback) is 2D again */
		WRITE_ONCE(fp_client, false);
		wake_up_interruptible(&d->wq_client);
		m3_kick(d);
		vc_path_apply(d);
		d->owner = NULL;
		/* the display may still scan out the last picture for a while */
		schedule_delayed_work(&d->free_work, msecs_to_jiffies(3000));
	}
	mutex_unlock(&d->mlock);
	return 0;
}

static const struct file_operations m3_fops = {
	.owner = THIS_MODULE,
	.open = m3_open,
	.release = m3_release,
	.unlocked_ioctl = m3_ioctl,
	.compat_ioctl = m3_ioctl,
	.mmap = m3_mmap,
};

/* ------------------------------------------------------------------ */
/* debugfs                                                            */
/* ------------------------------------------------------------------ */

static int stats_show(struct seq_file *m, void *v)
{
	struct m3_dev *d = gdev;
	unsigned long flags;
	char st[MVC3D_MAX_BUFS + 1];
	int i;

	spin_lock_irqsave(&d->lock, flags);
	for (i = 0; i < d->nbufs; i++)
		st[i] = "FUQSH"[d->bufs[i].state];
	st[i] = 0;
	seq_printf(m, "version %s, client %s, lead %u us, upstream %s, map %s\n", MVC3D_VERSION,
		   d->client_ready ? "ready" : "none", d->client_lead_us,
		   d->prov_reg ? (d->vc_upstream ? "video composer" : "other") : "none",
		   d->vc_on ? "on" : "off");
	seq_printf(m, "buffers %d %ux%u stride %u: %s (Free User Queued Shown Held)\n", d->nbufs,
		   d->w, d->h, d->stride, st);
	seq_printf(m, "frames in %llu, jobs %llu, shown %llu, dropped by client %llu, timeouts %llu, passed through %llu, job waits %llu\n",
		   d->frames_in, d->jobs_posted, d->shown, d->dropped_client, d->dropped_timeout,
		   d->passed, d->slot_waits);
	seq_printf(m, "jobs queued %d, output queued %d, gen %u\n", d->job_cnt, d->out_cnt, d->gen);
	seq_printf(m, "answer avg %lld us max %lld us\n",
		   d->us_answer_cnt ? div64_s64(d->us_answer_sum, d->us_answer_cnt) : 0,
		   d->us_answer_max);
	seq_printf(m, "vsync: period %lld us, lead %u vsyncs, lead frames %llu (late %llu)\n",
		   div64_s64(d->vs_period, NSEC_PER_USEC), d->lead_vs, d->lead_frames, d->lead_late);
	seq_printf(m, "av: shown minus due <0:%llu 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu\n",
		   d->av_hist[0], d->av_hist[1], d->av_hist[2], d->av_hist[3], d->av_hist[4],
		   d->av_hist[5]);
	seq_printf(m, "input: frames per vsync gap 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu\n",
		   d->in_gap[0], d->in_gap[1], d->in_gap[2], d->in_gap[3], d->in_gap[4]);
	seq_printf(m, "frame packing: probes %s%s, %s, output %s%s, mode sets %llu, format %llu, encoder %llu\n",
		   fp_probe_on ? "on" : "off", plane_probe_on ? " (+gui)" : "",
		   fp_wanted() ? "wanted" : "not wanted", fp_out ? "frame packed" : "2D",
		   fp_2160 ? " for 2160p" : "", fp_modesets, fp_applied, fp_enc_applied);
	seq_printf(m, "display: frames per vsync gap 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu\n",
		   d->disp_gap[0], d->disp_gap[1], d->disp_gap[2], d->disp_gap[3], d->disp_gap[4]);
	spin_unlock_irqrestore(&d->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static ssize_t stats_reset_write(struct file *f, const char __user *buf, size_t len,
				 loff_t *ppos)
{
	struct m3_dev *d = gdev;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	d->frames_in = d->jobs_posted = d->shown = d->dropped_client = 0;
	d->dropped_timeout = d->passed = d->slot_waits = 0;
	memset(d->in_gap, 0, sizeof(d->in_gap));
	memset(d->disp_gap, 0, sizeof(d->disp_gap));
	memset(d->av_hist, 0, sizeof(d->av_hist));
	d->av_cnt = d->lead_frames = d->lead_late = 0;
	d->us_answer_sum = d->us_answer_max = 0;
	d->us_answer_cnt = 0;
	d->in_any = d->disp_any = false;
	spin_unlock_irqrestore(&d->lock, flags);
	return len;
}

static const struct file_operations stats_reset_fops = {
	.owner = THIS_MODULE,
	.write = stats_reset_write,
};

/* ------------------------------------------------------------------ */
/* module                                                             */
/* ------------------------------------------------------------------ */

static int __init m3_init(void)
{
	struct m3_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);
	int ret;

	if (!d)
		return -ENOMEM;
	mutex_init(&d->mlock);
	spin_lock_init(&d->lock);
	init_waitqueue_head(&d->wq_client);
	init_waitqueue_head(&d->wq_thread);
	INIT_DELAYED_WORK(&d->free_work, free_work_fn);
	INIT_WORK(&d->vc_work, vc_path_work);
	INIT_DELAYED_WORK(&fp_work, fp_work_fn);
	gdev = d;

	vf_receiver_init(&d->recv, DRV_NAME, &m3_recv_ops, d);
	vf_provider_init(&d->prov, DRV_NAME, &m3_vf_ops, d);
	ret = vf_reg_receiver(&d->recv);
	if (ret)
		goto fail;
	d->thread = kthread_run(m3_thread, d, DRV_NAME);
	if (IS_ERR(d->thread)) {
		ret = PTR_ERR(d->thread);
		goto fail_recv;
	}
	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name = DRV_NAME;
	d->misc.fops = &m3_fops;
	d->misc.mode = 0666;	/* Android: ueventd sets its own (the installer chmods) */
	ret = misc_register(&d->misc);
	if (ret)
		goto fail_thread;
	d->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("stats", 0444, d->dbg, NULL, &stats_fops);
	debugfs_create_file("stats_reset", 0200, d->dbg, NULL, &stats_reset_fops);
	/* the firmware's code tells; the list of checked kernel builds is the fallback */
	if (fp_flags_off < 0)
		fp_flags_off = fp_flags_scan();
	if (fp_flags_off < 0)
		fp_flags_off = fp_flags_offset();
	if (fp_flags_off < 0) {
		/* frame packing off: MVC3D_IOC_SET_FP fails, user space plays 2D */
	} else if (register_kretprobe(&fmtpara_probe)) {
		pr_warn(DRV_NAME ": no probe on hdmitx21_get_fmtpara: no frame packed HDMI timing\n");
	} else if (register_kretprobe(&timing_probe)) {
		pr_warn(DRV_NAME ": no probe on hdmitx21_gettiming_from_vic: no frame packed HDMI timing\n");
		unregister_kretprobe(&fmtpara_probe);
	} else {
		fp_probe_on = true;
		/* the GUI on a 2160p desktop (optional) */
		if (register_kprobe(&plane_check_probe))
			pr_warn(DRV_NAME ": no probe on meson_plane_atomic_check: GUI misplaced in frame packing from 2160p\n");
		else
			plane_probe_on = true;
	}
	/* the map as this module wants it, whatever an earlier load left */
	mutex_lock(&d->mlock);
	d->vc_on = !vc_route;
	vc_path_apply(d);
	mutex_unlock(&d->mlock);
	pr_info(DRV_NAME ": %s ready (vfm map %s while a compositor is registered)\n",
		MVC3D_VERSION, vc_map_id);
	return 0;

fail_thread:
	kthread_stop(d->thread);
fail_recv:
	vf_unreg_receiver(&d->recv);
fail:
	gdev = NULL;
	kfree(d);
	return ret;
}

static void __exit m3_exit(void)
{
	struct m3_dev *d = gdev;
	bool playing;

	if (plane_probe_on)
		unregister_kprobe(&plane_check_probe);
	if (fp_probe_on) {
		unregister_kretprobe(&timing_probe);
		unregister_kretprobe(&fmtpara_probe);
	}
	debugfs_remove_recursive(d->dbg);
	misc_deregister(&d->misc);
	kthread_stop(d->thread);
	cancel_work_sync(&d->vc_work);
	cancel_delayed_work_sync(&fp_work);
	/* the stock map back: nothing may route to a receiver that is gone */
	playing = d->prov_reg;

	mutex_lock(&d->mlock);
	d->client_ready = false;
	vc_route = 0;
	d->prov_reg = false;
	d->vc_on = true;
	vc_path_apply(d);
	mutex_unlock(&d->mlock);
	up_release_held(d);
	if (playing)
		vf_unreg_provider(&d->prov);
	vf_unreg_receiver(&d->recv);
	cancel_delayed_work_sync(&d->free_work);
	free_bufs(d);
	kfree(d);
	gdev = NULL;
}

module_init(m3_init);
module_exit(m3_exit);

MODULE_DESCRIPTION("3D MVC frame packed output between the video composer and the display (Amlogic S5 Android)");
MODULE_LICENSE("GPL");
/* /sys/module/mvc3d/version (the installer compares it) */
MODULE_VERSION(MVC3D_VERSION);
