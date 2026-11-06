// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d for the Amlogic S905X4 (SC2, Dune HD 5.4.125): the hardware MVC path (/dev/mvc3dhw), see
 * MVC3D_HW_F_DIRECT in mvc3d_uapi.h.
 *
 * The legacy MVC decoder (amvdec_h264mvc, provider "decoder.h264mvc") gives its pictures as canvas
 * indices here, which the legacy video layer (amvideo) shows as they are: view_mode 0 = the base view
 * (2D), view_mode 3 = both views (VIDTYPE_MVC; with framepacking_support the VPP itself puts the right
 * view under the left one, 1920x2205). So nothing is composed: while a client plays, the vfm path is
 * "decoder.h264mvc mvc3dhw amvideo" and this module holds the decoder's pictures until the player names
 * their display time (MVC3D_HW_PRESENT); the provider side gives amvideo (freerun: it takes what it
 * peeks) a picture only from the vsync near that time. Only the vframe pointers pass through: the
 * Dune SC2 struct vframe_s differs from the 5.4.125 headers after its first ~450 bytes, no field of it
 * is read or written here.
 *
 * The software MVC path of mvc3d.c (frame packing composed from MediaCodec + edge264) is not in this
 * module: /dev/mvc3d does not exist and players play such streams in 2D.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/hardirq.h>
#include <linux/workqueue.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/dma-mapping.h>
#include <drm/drm_plane.h>

#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/amlogic/media/vfm/vfm_ext.h>

#include "mvc3d_uapi.h"

#define DRV_NAME	"mvc3d"
#define SC2_VERSION	MVC3D_SC2_VERSION
#define HW_NAME		"mvc3dhw"
#define HW_PROVIDER	"decoder.h264mvc"
#define HW_MAP_ID	"mvc3dhw"
#define HW_MAP_CHAIN	HW_PROVIDER " " HW_NAME " amvideo"
#define NSLOT		MVC3D_HW_MAX_BUFS
#define MVC_PARAMS	"/sys/module/amvdec_h264mvc/parameters/"
#define FP_PARAM	"/sys/module/aml_media/parameters/framepacking_support"

void set_freerun_mode(int mode);

static int hw_2d_view;
module_param(hw_2d_view, int, 0644);
MODULE_PARM_DESC(hw_2d_view, "2D: 0 the base view, 1 the dependent view (tests)");

static int lead_us = -1;
module_param(lead_us, int, 0644);
MODULE_PARM_DESC(lead_us, "a picture is shown from the first vsync after due - lead_us (-1: half a vsync)");


static int vsync_us;
module_param(vsync_us, int, 0444);
MODULE_PARM_DESC(vsync_us, "the display's vsync period as measured (us)");

enum slot_state { S_FREE, S_READY, S_CLIENT, S_QUEUED, S_SHOWN, S_RETIRED };

struct slot {
	struct vframe_s *vf;
	enum slot_state state;
	u32 seq, gen;
	u64 decoded_ns, due_ns;
	u32 retire_seq;		/* S_RETIRED: the vsync it came back from the display at */
};

struct sc2_dev {
	struct miscdevice misc;
	struct mutex mlock;		/* ioctls start/stop, map */
	spinlock_t lock;		/* slots, queues, prov_reg (also from the vsync interrupt) */
	wait_queue_head_t wq_client, wq_thread;
	bool kick;
	struct task_struct *thread;
	struct vframe_receiver_s recv;
	struct vframe_provider_s prov;
	struct dentry *dbg;

	struct file *owner;
	bool started, prov_reg, routed, fp;
	u32 seq, gen;
	struct slot slots[NSLOT];
	int ready[NSLOT], ready_head, ready_cnt;	/* got from the decoder, for MVC3D_HW_WAIT */
	int dispq[NSLOT], disp_head, disp_cnt;		/* presented, by due time */
	char default_chain[160];

	/* vsync period, measured from amvideo's peeks in its vsync interrupt */
	u64 vs_last_ns, vs_period_ns, vs_odd_ns;
	u32 vs_odd;
	u32 vs_seq;			/* vsyncs seen */
	u64 retired_late;

	u64 pics_in, shown, dropped_late, dropped_client, released, put_back, stale_put;
	u64 late_us_sum, late_cnt;
	s64 late_max_us;
};

static struct sc2_dev *gd;
static u64 sc2_dma_mask = DMA_BIT_MASK(32);
static void pool_decoder_gone(void);
static bool fp_on;
static void fp_conv_vf(struct vframe_s *vf);

/* ------------------------------------------------------------------ */
/* vfm route, decoder and video parameters                            */
/* ------------------------------------------------------------------ */

struct kw {
	struct work_struct work;
	const char *path, *val;
	int ret;
};

static void kw_fn(struct work_struct *work)
{
	struct kw *k = container_of(work, struct kw, work);
	struct file *f = filp_open(k->path, O_WRONLY, 0);
	loff_t pos = 0;
	ssize_t n;

	if (IS_ERR(f)) {
		k->ret = PTR_ERR(f);
		return;
	}
	n = kernel_write(f, k->val, strlen(k->val), &pos);
	filp_close(f, NULL);
	k->ret = n < 0 ? n : 0;
}

/*
 * The parameter files are root's (0664): written by a kernel worker, not with the credentials of the
 * player that calls the ioctl
 */
static int sysfs_write(const char *path, const char *val)
{
	struct kw k = { .path = path, .val = val };

	INIT_WORK_ONSTACK(&k.work, kw_fn);
	schedule_work(&k.work);
	flush_work(&k.work);
	destroy_work_on_stack(&k.work);
	if (k.ret)
		pr_warn(DRV_NAME ": %s <- %s: %d\n", path, val, k.ret);
	return k.ret;
}

/*
 * Anything that opens a file runs in a kernel worker: the ioctl caller may be a process in its exit
 * (hw_release from do_exit has no fs context any more: filp_open there oopsed, 11-05)
 */
struct kcall {
	struct work_struct work;
	void (*fn)(void *);
	void *arg;
};

static void kcall_fn(struct work_struct *work)
{
	struct kcall *k = container_of(work, struct kcall, work);

	k->fn(k->arg);
}

static void in_worker(void (*fn)(void *), void *arg)
{
	struct kcall k = { .fn = fn, .arg = arg };

	INIT_WORK_ONSTACK(&k.work, kcall_fn);
	schedule_work(&k.work);
	flush_work(&k.work);
	destroy_work_on_stack(&k.work);
}

/* the chain of the map "default" from /sys/class/vfm/map: "[00]  default { decoder(0) ppmgr(0) amvideo}" */
static void read_default(struct sc2_dev *h)
{
	struct file *f = filp_open("/sys/class/vfm/map", O_RDONLY, 0);
	char *buf, *l, *e, *o = h->default_chain;
	loff_t pos = 0;
	ssize_t n;

	strscpy(h->default_chain, "decoder ppmgr deinterlace amvideo", sizeof(h->default_chain));
	if (IS_ERR(f))
		return;
	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return;
	}
	n = kernel_read(f, buf, PAGE_SIZE - 1, &pos);
	filp_close(f, NULL);
	l = n > 0 ? strstr(buf, " default {") : NULL;
	if (l && (e = strchr(l, '}'))) {
		char *p = strchr(l, '{') + 1;
		size_t left = sizeof(h->default_chain) - 1;

		*e = 0;
		while (*p && left) {
			if (*p == '(') {
				while (*p && *p != ')')
					p++;
				if (*p)
					p++;
				continue;
			}
			if (*p != ' ' || (o > h->default_chain && o[-1] != ' ')) {
				*o++ = *p;
				left--;
			}
			p++;
		}
		while (o > h->default_chain && o[-1] == ' ')
			o--;
		*o = 0;
		if (!h->default_chain[0])
			strscpy(h->default_chain, "decoder ppmgr deinterlace amvideo", sizeof(h->default_chain));
	}
	kfree(buf);
}

/* "decoder.h264mvc" matches "default" by its prefix and the first map wins: "default" is off meanwhile */
static int route(struct sc2_dev *h, bool on)
{
	char id[32], chain[160];

	if (on == h->routed)
		return 0;
	if (on) {
		read_default(h);
		strscpy(id, "default", sizeof(id));
		vfm_map_remove(id);
		strscpy(id, HW_MAP_ID, sizeof(id));
		strscpy(chain, HW_MAP_CHAIN, sizeof(chain));
		vfm_map_remove(id);
		if (vfm_map_add(id, chain) < 0) {
			pr_err(DRV_NAME ": cannot add vfm map %s\n", id);
			strscpy(id, "default", sizeof(id));
			strscpy(chain, h->default_chain, sizeof(chain));
			vfm_map_add(id, chain);
			return -EIO;
		}
	} else {
		strscpy(id, HW_MAP_ID, sizeof(id));
		vfm_map_remove(id);
		strscpy(id, "default", sizeof(id));
		strscpy(chain, h->default_chain, sizeof(chain));
		if (vfm_map_add(id, chain) < 0)
			pr_err(DRV_NAME ": cannot restore vfm map default (%s)\n", chain);
	}
	h->routed = on;
	pr_info(DRV_NAME ": %s routed %s\n", HW_PROVIDER, on ? "through " HW_NAME " to amvideo" : "back to default");
	return 0;
}

/* ------------------------------------------------------------------ */
/* slots (h->lock held)                                               */
/* ------------------------------------------------------------------ */

static int slot_of_locked(struct sc2_dev *h, struct vframe_s *vf)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state != S_FREE && h->slots[i].vf == vf)
			return i;
	return -1;
}

static int free_slot_locked(struct sc2_dev *h)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state == S_FREE)
			return i;
	return -1;
}

/* the slot's picture back to the decoder (it may be called from the vsync interrupt: so does amvideo) */
static void slot_put_locked(struct sc2_dev *h, int i)
{
	struct slot *s = &h->slots[i];

	if (h->prov_reg && s->vf) {
		vf_put(s->vf, HW_NAME);
		h->put_back++;
	}
	s->vf = NULL;
	s->state = S_FREE;
}

/* all pictures not on the display back to the decoder; shown ones come back by amvideo's put */
static void drop_all_locked(struct sc2_dev *h)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state == S_READY || h->slots[i].state == S_CLIENT || h->slots[i].state == S_QUEUED ||
		    h->slots[i].state == S_RETIRED)
			slot_put_locked(h, i);
	h->ready_cnt = h->disp_cnt = 0;
	h->ready_head = h->disp_head = 0;
}

/* the decoder is gone: its frames are invalid, nothing is put back */
static void forget_all_locked(struct sc2_dev *h)
{
	int i;

	for (i = 0; i < NSLOT; i++) {
		h->slots[i].vf = NULL;
		h->slots[i].state = S_FREE;
	}
	h->ready_cnt = h->disp_cnt = 0;
	h->ready_head = h->disp_head = 0;
}

/* ------------------------------------------------------------------ */
/* the decoder side: pull its pictures into slots                     */
/* ------------------------------------------------------------------ */

static void kick(struct sc2_dev *h)
{
	h->kick = true;
	wake_up_interruptible(&h->wq_thread);
}

static int pull_thread(void *data)
{
	struct sc2_dev *h = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible_timeout(h->wq_thread, h->kick || kthread_should_stop(),
						 msecs_to_jiffies(50));
		h->kick = false;
		for (;;) {
			struct vframe_s *vf = NULL;
			unsigned long flags;
			bool got = false;
			int i;

			spin_lock_irqsave(&h->lock, flags);
			if (h->prov_reg && h->started && (i = free_slot_locked(h)) >= 0 && vf_peek(HW_NAME)) {
				vf = vf_get(HW_NAME);
				if (vf) {
					struct slot *s = &h->slots[i];

					s->vf = vf;
					s->state = S_READY;
					s->seq = h->seq++;
					s->gen = h->gen;
					s->decoded_ns = ktime_get_ns();
					h->ready[(h->ready_head + h->ready_cnt) % NSLOT] = i;
					h->ready_cnt++;
					h->pics_in++;
					got = true;
				}
			}
			spin_unlock_irqrestore(&h->lock, flags);
			if (!got)
				break;
			wake_up_interruptible(&h->wq_client);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* the display side: provider "mvc3dhw" for amvideo                   */
/* ------------------------------------------------------------------ */

/* (h->lock held) in amvideo's vsync interrupt: the vsync period */
static void vs_tick_locked(struct sc2_dev *h, u64 now)
{
	u64 dt;

	if (!in_irq())
		return;
	dt = now - h->vs_last_ns;
	if (h->vs_last_ns && dt > 3 * NSEC_PER_MSEC && dt < 100 * NSEC_PER_MSEC) {
		u64 p = h->vs_period_ns;

		if (!p) {
			h->vs_period_ns = dt;
		} else if (dt > p - p / 8 && dt < p + p / 8) {
			h->vs_period_ns = (p * 7 + dt) / 8;
			h->vs_odd = 0;
		} else if (h->vs_odd && dt > h->vs_odd_ns - h->vs_odd_ns / 8 && dt < h->vs_odd_ns + h->vs_odd_ns / 8) {
			/* another refresh rate (the mode changed): 6 like intervals in a row; a missed vsync is one */
			if (++h->vs_odd >= 6) {
				h->vs_period_ns = dt;
				h->vs_odd = 0;
			}
		} else {
			h->vs_odd_ns = dt;
			h->vs_odd = 1;
		}
		vsync_us = (int)div_u64(h->vs_period_ns, 1000);
	}
	if (!h->vs_last_ns || dt > 3 * NSEC_PER_MSEC) {
		int i;

		h->vs_last_ns = now;
		h->vs_seq++;
		/* pictures back from the display 2 vsyncs ago: the VPP is done with them */
		for (i = 0; i < NSLOT; i++)
			if (h->slots[i].state == S_RETIRED && h->vs_seq - h->slots[i].retire_seq >= 2)
				slot_put_locked(h, i);
	}
}

static u64 lead_ns_locked(struct sc2_dev *h)
{
	int l = READ_ONCE(lead_us);

	if (l >= 0)
		return (u64)l * 1000;
	return h->vs_period_ns ? h->vs_period_ns / 2 : 8 * NSEC_PER_MSEC;
}

/*
 * (h->lock held) the picture to show now: the head of the display queue once due; when the next one is
 * due too, the head is late: back to the decoder
 */
static struct slot *due_locked(struct sc2_dev *h, u64 now)
{
	u64 lead = lead_ns_locked(h);

	while (h->disp_cnt) {
		struct slot *s = &h->slots[h->dispq[h->disp_head]];

		if (s->due_ns > now + lead)
			return NULL;
		if (h->disp_cnt > 1) {
			struct slot *n = &h->slots[h->dispq[(h->disp_head + 1) % NSLOT]];

			if (n->due_ns <= now + lead) {
				slot_put_locked(h, h->dispq[h->disp_head]);
				h->disp_head = (h->disp_head + 1) % NSLOT;
				h->disp_cnt--;
				h->dropped_late++;
				continue;
			}
		}
		return s;
	}
	return NULL;
}

static struct vframe_s *sc2_peek(void *op_arg)
{
	struct sc2_dev *h = op_arg;
	struct vframe_s *vf = NULL;
	unsigned long flags;
	struct slot *s;
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&h->lock, flags);
	vs_tick_locked(h, now);
	s = h->prov_reg ? due_locked(h, now) : NULL;
	if (s)
		vf = s->vf;
	spin_unlock_irqrestore(&h->lock, flags);
	return vf;
}

static struct vframe_s *sc2_get(void *op_arg)
{
	struct sc2_dev *h = op_arg;
	struct vframe_s *vf = NULL;
	unsigned long flags;
	struct slot *s;
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&h->lock, flags);
	vs_tick_locked(h, now);
	s = h->prov_reg ? due_locked(h, now) : NULL;
	if (s) {
		s64 late = (s64)(now - s->due_ns) / 1000;

		vf = s->vf;
		s->state = S_SHOWN;
		if (READ_ONCE(fp_on))
			fp_conv_vf(vf);
		h->disp_head = (h->disp_head + 1) % NSLOT;
		h->disp_cnt--;
		h->shown++;
		h->late_us_sum += late;
		h->late_cnt++;
		if (late > h->late_max_us)
			h->late_max_us = late;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	return vf;
}

static void sc2_put(struct vframe_s *vf, void *op_arg)
{
	struct sc2_dev *h = op_arg;
	unsigned long flags;
	int i;

	if (!vf)
		return;
	spin_lock_irqsave(&h->lock, flags);
	i = slot_of_locked(h, vf);
	if (i >= 0 && h->slots[i].state == S_SHOWN && h->prov_reg) {
		/*
		 * amvideo gives it back when it switches to the next picture, which the VPP takes from the next
		 * vsync on: the decoder (short of free buffers while the player holds some) would write into it
		 * at once - a torn picture. It goes back 2 vsyncs later (vs_tick_locked).
		 */
		h->slots[i].state = S_RETIRED;
		h->slots[i].retire_seq = h->vs_seq;
	} else if (i >= 0) {
		slot_put_locked(h, i);
	} else {
		/* not ours (any more): to the decoder if it is still there */
		h->stale_put++;
		if (h->prov_reg)
			vf_put(vf, HW_NAME);
	}
	spin_unlock_irqrestore(&h->lock, flags);
	kick(h);
}

static int sc2_prov_event(int type, void *data, void *op_arg)
{
	vf_notify_provider(HW_NAME, type, data);
	return 0;
}

static int sc2_vf_states(struct vframe_states *st, void *op_arg)
{
	struct sc2_dev *h = op_arg;
	unsigned long flags;
	int i, nfree = 0;

	spin_lock_irqsave(&h->lock, flags);
	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state == S_FREE)
			nfree++;
	st->vf_pool_size = NSLOT;
	st->buf_free_num = nfree;
	st->buf_recycle_num = 0;
	st->buf_avail_num = h->disp_cnt;
	spin_unlock_irqrestore(&h->lock, flags);
	return 0;
}

static const struct vframe_operations_s sc2_vf_ops = {
	.peek = sc2_peek,
	.get = sc2_get,
	.put = sc2_put,
	.event_cb = sc2_prov_event,
	.vf_states = sc2_vf_states,
};

/* ------------------------------------------------------------------ */
/* receiver side (decoder events)                                     */
/* ------------------------------------------------------------------ */

static int sc2_recv_event(int type, void *data, void *op_arg)
{
	struct sc2_dev *h = op_arg;
	unsigned long flags;

	switch (type) {
	case VFRAME_EVENT_PROVIDER_REG:
		spin_lock_irqsave(&h->lock, flags);
		forget_all_locked(h);
		h->prov_reg = true;
		h->seq = 0;
		spin_unlock_irqrestore(&h->lock, flags);
		pr_info(DRV_NAME ": decoder registered\n");
		vf_reg_provider(&h->prov);
		kick(h);
		break;
	case VFRAME_EVENT_PROVIDER_UNREG:
		spin_lock_irqsave(&h->lock, flags);
		h->prov_reg = false;
		forget_all_locked(h);
		spin_unlock_irqrestore(&h->lock, flags);
		/* amvideo keeps a copy of what it shows */
		vf_unreg_provider(&h->prov);
		pool_decoder_gone();
		pr_info(DRV_NAME ": decoder unregistered after %u pictures\n", h->seq);
		wake_up_interruptible(&h->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_LIGHT_UNREG:
		spin_lock_irqsave(&h->lock, flags);
		drop_all_locked(h);
		spin_unlock_irqrestore(&h->lock, flags);
		vf_light_unreg_provider(&h->prov);
		wake_up_interruptible(&h->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_RESET:
		spin_lock_irqsave(&h->lock, flags);
		drop_all_locked(h);
		spin_unlock_irqrestore(&h->lock, flags);
		vf_notify_receiver(HW_NAME, type, data);
		wake_up_interruptible(&h->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_VFRAME_READY:
		kick(h);
		break;
	case VFRAME_EVENT_PROVIDER_QUREY_STATE:
		return RECEIVER_ACTIVE;
	default:
		return vf_notify_receiver(HW_NAME, type, data);
	}
	return 0;
}

static const struct vframe_receiver_op_s sc2_recv_ops = {
	.event_cb = sc2_recv_event,
};

/* ------------------------------------------------------------------ */
/* /dev/mvc3dhw                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* the decoder's 31 bit DMA: its work space and reference buffers     */
/* ------------------------------------------------------------------ */

/*
 * The firmware's amvdec_h264mvc drops bit 31 of some of its DMA write addresses: its work space and its two
 * reference (co-located motion vector) buffers get part of their data 2 GiB lower, in whatever owns that
 * memory (measured 11-05 with hwmvc/sc2/memcanary + the allocation log: 0xd0a20000 -> 0x50a20000; other
 * processes' pages, the page cache: crashes, a kernel oops). The codec CMA lies at 0xcc000000-0xe6ffffff,
 * so it always happens. Its pictures are written right (bit exact).
 *
 * So these three buffers are moved below 2 GiB, where the address is the same with or without bit 31:
 * at MVC3D_HW_START a pool is taken there with alloc_contig_range() (movable pages are migrated away),
 * and the decoder's decoder_bmmu_box_alloc_buf_phy() for them (the work space: index 0; a reference
 * buffer: the first allocation in init_canvas()) returns an address in it. The decoder's own buffer stays
 * allocated, unused. The pool is freed once no decoder is left. No pool: no hardware MVC.
 */
#define POOL_SIZE	(28 << 20)	/* work space + both views' reference buffers: up to 16 frames each (avatar.mk3d: 8 frames, 13.1 MB) */
#define POOL_TOP	0x80000000UL
#define POOL_BOTTOM	0x31000000UL	/* above the low reserved areas (dsp firmware at 0x30000000) */

static unsigned long pool_pfn;		/* 0: none */
static unsigned long pool_used;		/* bytes handed out to the current decoder */
static atomic_t pool_redirects = ATOMIC_INIT(0), pool_misses = ATOMIC_INIT(0);
#define POOL_SLOTS	16
static struct { int idx; unsigned long off, len; } pool_slot[POOL_SLOTS];
static int pool_slots;
static bool pool_same_decoder;	/* the current decoder's work space is in */
static atomic_t ref_next = ATOMIC_INIT(0);	/* init_canvas entered: its next allocation is the reference buffer */
static DEFINE_SPINLOCK(pool_lock);

static int keep_pool;
module_param(keep_pool, int, 0444);
MODULE_PARM_DESC(keep_pool, "tests (other receivers of the MVC decoder, tools/hwmvctest): the low pool taken at load and kept");

static int log_alloc;
module_param(log_alloc, int, 0644);
MODULE_PARM_DESC(log_alloc, "debug: log the MVC decoder's buffers (index, size, physical address)");

static int pool_get(struct device *dev)
{
	unsigned long base, pfn, n = POOL_SIZE >> PAGE_SHIFT;
	int tries = 0;

	if (pool_pfn)
		return 0;
	for (base = POOL_TOP - POOL_SIZE; base >= POOL_BOTTOM && tries < 48; base -= 4 << 20, tries++) {
		pfn = base >> PAGE_SHIFT;
		if (alloc_contig_range(pfn, pfn + n, MIGRATE_MOVABLE, GFP_KERNEL))
			continue;
		/* clean: no dirty cache lines over what the decoder writes */
		memset(phys_to_virt(base), 0, POOL_SIZE);
		dma_sync_single_for_device(dev, base, POOL_SIZE, DMA_TO_DEVICE);
		pool_pfn = pfn;
		pool_used = 0;
		pr_info("mvc3d: low DMA pool %#lx..%#lx (%d tries)\n", base, base + POOL_SIZE, tries + 1);
		return 0;
	}
	pr_warn("mvc3d: no %d MiB below 2 GiB for the MVC decoder (%d tries): hardware MVC refused\n",
		POOL_SIZE >> 20, tries);
	return -ENOMEM;
}

static void pool_put(void)
{
	if (!pool_pfn)
		return;
	free_contig_range(pool_pfn, POOL_SIZE >> PAGE_SHIFT);
	pr_info("mvc3d: low DMA pool freed (%d buffers moved, %d not)\n", atomic_read(&pool_redirects),
		atomic_read(&pool_misses));
	pool_pfn = 0;
	pool_used = 0;
	pool_slots = 0;
	pool_same_decoder = false;
}

static int ref_pre(struct kprobe *p, struct pt_regs *regs)
{
	atomic_set(&ref_next, 1);
	return 0;
}

static struct kprobe ref_probe = {
	.symbol_name = "amvdec_h264mvc:init_canvas",
	.pre_handler = ref_pre,
};
static bool ref_probe_on;

/* decoder_bmmu_box_alloc_buf_phy(handle, idx, size, driver_name, &phys) */
struct alloc_ctx {
	int idx, size, move;
	unsigned long *addr;
};

static int alloc_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct alloc_ctx *c = (struct alloc_ctx *)ri->data;
	const char *name = (const char *)regs->regs[3];
	char nm[16] = "";

	if (!name || probe_kernel_read(nm, name, sizeof(nm) - 1) || strncmp(nm, "amvdec_h264mvc", 14))
		return 1;	/* another decoder: nothing to do on return */
	c->idx = regs->regs[1];
	c->size = regs->regs[2];
	c->addr = (unsigned long *)regs->regs[4];
	c->move = c->idx == 0 || atomic_xchg(&ref_next, 0);
	return 0;
}

static int alloc_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct alloc_ctx *c = (struct alloc_ctx *)ri->data;
	unsigned long flags, orig = c->addr ? *c->addr : 0, to = 0;

	if (c->move && !regs_return_value(regs) && c->addr && c->size > 0) {
		unsigned long need = PAGE_ALIGN(c->size);

		int i;

		spin_lock_irqsave(&pool_lock, flags);
		/* the work space comes first: a new decoder (the decoder is single instance) */
		if (c->idx == 0 && !pool_same_decoder) {
			pool_used = 0;
			pool_slots = 0;
			pool_same_decoder = true;
		}
		/*
		 * the decoder asks again for what it has, at every new SPS (avatar.mk3d, 11-06): the box gives the same
		 * buffer, so the same place here too - a new one each time ran the pool out, and a buffer stayed above
		 * 2 GiB (kernel and user memory overwritten)
		 */
		for (i = 0; i < pool_slots; i++) {
			if (pool_slot[i].idx == c->idx && need <= pool_slot[i].len) {
				to = (pool_pfn << PAGE_SHIFT) + pool_slot[i].off;
				break;
			}
		}
		if (!to && pool_pfn && pool_used + need <= POOL_SIZE && pool_slots < POOL_SLOTS) {
			to = (pool_pfn << PAGE_SHIFT) + pool_used;
			pool_slot[pool_slots].idx = c->idx;
			pool_slot[pool_slots].off = pool_used;
			pool_slot[pool_slots].len = need;
			pool_slots++;
			pool_used += need;
		}
		spin_unlock_irqrestore(&pool_lock, flags);
		if (to) {
			*c->addr = to;
			atomic_inc(&pool_redirects);
		} else if (orig >= POOL_TOP) {
			/* never into high memory: the decoder's init fails instead */
			atomic_inc(&pool_misses);
			pr_err("mvc3d: MVC decoder buffer %d (%#x bytes) at %#lx cannot go below 2 GiB: refused\n", c->idx,
			       c->size, orig);
			regs_set_return_value(regs, -ENOMEM);
		}
	}
	if (READ_ONCE(log_alloc))
		pr_info("mvc3d: alloc idx %d size %#x at %#lx%s%s\n", c->idx, c->size, orig,
			to ? " -> " : "", to ? "low pool" : "");
	return 0;
}

static struct kretprobe alloc_probe = {
	.kp.symbol_name = "decoder_bmmu_box_alloc_buf_phy",
	.entry_handler = alloc_entry,
	.handler = alloc_ret,
	.data_size = sizeof(struct alloc_ctx),
	.maxactive = 16,
};
static bool alloc_probe_on;

/* the pool goes when no decoder can write into it any more (h->mlock held) */
static void pool_release_if_idle(struct sc2_dev *h)
{
	if (!h->started && !READ_ONCE(h->prov_reg) && !keep_pool)
		pool_put();
}

static void pool_work_fn(struct work_struct *w)
{
	mutex_lock(&gd->mlock);
	pool_release_if_idle(gd);
	mutex_unlock(&gd->mlock);
}
static DECLARE_WORK(pool_work, pool_work_fn);

/* the decoder unregistered (its buffers are free): the pool is free for the next one, or goes */
static void pool_decoder_gone(void)
{
	unsigned long flags;

	spin_lock_irqsave(&pool_lock, flags);
	pool_used = 0;
	pool_slots = 0;
	pool_same_decoder = false;
	spin_unlock_irqrestore(&pool_lock, flags);
	schedule_work(&pool_work);
}

/* ------------------------------------------------------------------ */
/* HDMI frame packing on the S905X4 (hdmitx20)                         */
/* ------------------------------------------------------------------ */

/*
 * The hdmitx20 driver has an old 3D frame packing path (config "3dfp": flag_3dfp; the mode set then takes
 * the 3D encoder tables (2250 lines), the 3D clock table, the frame packing VSIF), measured right on the
 * Optoma projector 11-05 once these are given too:
 *  - the PHY: with a 3D flag the mode set leaves the TMDS PHY off ("For 3D, enable phy by SystemControl at
 *    last step") -> amhdmitx0/phy 1 afterwards;
 *  - 8 bit 4:4:4 (the 3D clock table is for 24 bit colour);
 *  - the vinfo of the mode with the frame packed height (2 * 1080 + 45; CoreELEC does the same), else the
 *    VPP puts out 1080 lines of 2205;
 *  - no Dolby Vision output processing (it moves the colour depth back to 10 bit) and the HPD held (the
 *    projector pulses it when it switches to 3D) while a client plays frame packed.
 * Setting it through sysfs alone does not last: Android's next DRM commit sets its own mode again, with the
 * 3D flag still on (2160p60: no 3D encoder table -> vblank time outs, a crtc_commit hang, a hung task panic).
 * So, as on the S928X (mvc3d.c), the mode sets themselves are probed: while a client plays frame packed,
 * the firmware's 1080p24/25/30 and 2160p24/25/30 (frame rate switch on a 2160p desktop; Android keeps its
 * mode) become 1080p frame packing, every other mode clears the flag first.
 */
#define FP_FLAGS_OFF	0x2834	/* struct hdmitx_dev: flag_3dfp bit 0, 3dtb bit 1, 3dss bit 2 (r25 hdmitx_set_enc_hw) */
#define FP_ATTR_OFF	0x26b4	/* char fmt_attr[] (r25 set_disp_mode_auto) */
#define FP_ATTR_LEN	16
#define FP_CURPARAM_OFF	0x2648	/* struct hdmitx_vidpara *cur_video_param */
/* hdmitx20 set_disp_mode_auto relative to hdmitx_set_enc_hw in the r25 aml_media .text (both static) */
#define SDMA_FROM_ENC	(0xd62a4L - 0xe5058L)

void *get_hdmitx_device(void);
struct vinfo_s *get_current_vinfo(void);

static bool fp_ok;		/* layout checked, probes on */
static int fp_win_h = 2205, fp_win_w = 1920;	/* the frame packed frame (video window) */
static bool fp_armed;		/* a client plays frame packed */
static bool fp_on;		/* the hdmitx has our 3D flag (the last mode set was frame packed) */
static char fp_attr_saved[FP_ATTR_LEN];
static bool fp_attr_forced;
static u64 fp_modesets, fp_maps;
static char fp_dv_policy[8], fp_dv_mode[8];
static bool fp_dv_saved;

#define FP_VINFO_SIZE	512
static u8 fp_vinfo[5][FP_VINFO_SIZE];
static bool fp_vinfo_ok[5];

static const char *const fp_modes[] = { "1080p24hz", "1080p25hz", "1080p30hz", "720p50hz", "720p60hz" };

static int fp_mode_index(const char *m)
{
	static const char *const from2160[] = { "2160p24hz", "2160p25hz", "2160p30hz" };
	int i;

	for (i = 0; i < ARRAY_SIZE(fp_modes); i++)
		if (!strcmp(m, fp_modes[i]))
			return i;
	for (i = 0; i < ARRAY_SIZE(from2160); i++)
		if (!strcmp(m, from2160[i]))
			return 16 + i;	/* -> 1080p of the same rate */
	return -1;
}

static bool is_fp_vinfo(const void *v)
{
	return v && (const u8 *)v >= fp_vinfo[0] && (const u8 *)v < fp_vinfo[0] + sizeof(fp_vinfo);
}

/* the frame packed copy of a mode's vinfo: width w, then height, field_height, ... htotal (+13), vtotal (+14) */
static bool fp_make_vinfo(int idx, const void *src)
{
	u8 *v = fp_vinfo[idx];
	u32 w = idx < 3 ? 1920 : 1280, h = idx < 3 ? 1080 : 720, gap = idx < 3 ? 45 : 30;
	int off;

	if (probe_kernel_read(v, src, FP_VINFO_SIZE))
		return false;
	for (off = 8; off < 128; off += 4) {
		u32 *p = (u32 *)(v + off);

		if (p[0] == w && p[1] == h && p[2] == h) {
			p[1] = 2 * h + gap;
			p[2] = 2 * h + gap;
			if (p[14] == (h == 1080 ? 1125 : 750))
				p[14] *= 2;
			return true;
		}
	}
	return false;
}

/* hdmi_get_valid_vinfo(char *mode): while armed, the frame packing modes get the frame packed vinfo */
struct vinfo_call {
	int idx;
};

static int vinfo_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct vinfo_call *c = (struct vinfo_call *)ri->data;
	char m[16] = "";

	c->idx = -1;
	if (!READ_ONCE(fp_armed) || probe_kernel_read(m, (void *)regs->regs[0], sizeof(m) - 1))
		return 1;
	c->idx = fp_mode_index(m);
	if (c->idx < 0)
		return 1;
	if (c->idx >= 16)
		c->idx -= 16;	/* 2160p -> 1080p of the same rate */
	regs->regs[0] = (unsigned long)fp_modes[c->idx];
	return 0;
}

static int vinfo_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct vinfo_call *c = (struct vinfo_call *)ri->data;
	void *v = (void *)regs_return_value(regs);

	if (c->idx < 0 || !v)
		return 0;
	if (!fp_vinfo_ok[c->idx])
		fp_vinfo_ok[c->idx] = fp_make_vinfo(c->idx, v);
	if (fp_vinfo_ok[c->idx]) {
		regs->regs[0] = (unsigned long)fp_vinfo[c->idx];
		fp_maps++;
	}
	return 0;
}

static struct kretprobe vinfo_probe = {
	.kp.symbol_name = "hdmi_get_valid_vinfo",
	.entry_handler = vinfo_entry,
	.handler = vinfo_ret,
	.data_size = sizeof(struct vinfo_call),
	.maxactive = 8,
};
static bool vinfo_probe_on;

static void fp_attr(bool force)
{
	char *a = (char *)get_hdmitx_device() + FP_ATTR_OFF;

	if (force) {
		if (!fp_attr_forced) {
			memcpy(fp_attr_saved, a, FP_ATTR_LEN);
			fp_attr_forced = true;
		}
		memset(a, 0, FP_ATTR_LEN);
		memcpy(a, "444,8bit", 8);
	} else if (fp_attr_forced) {
		memcpy(a, fp_attr_saved, FP_ATTR_LEN);
		fp_attr_forced = false;
	}
}

static void fp_flag(bool on)
{
	u8 *f = (u8 *)get_hdmitx_device() + FP_FLAGS_OFF;

	*f = on ? (*f & ~7) | 1 : *f & ~7;
}

static struct delayed_work fp_work;
static bool fp_work_on;

/*
 * set_disp_mode_auto() of the hdmitx20 (the mode set, after the vinfo of the mode was taken): a frame packed
 * vinfo -> the 3D flag and 8 bit 4:4:4 for this mode set; any other mode -> back to 2D first
 */
static int sdma_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct vinfo_s *v = get_current_vinfo();
	bool fp = READ_ONCE(fp_armed) && is_fp_vinfo(v);

	if (fp) {
		bool p720 = (const u8 *)v >= fp_vinfo[3];

		WRITE_ONCE(fp_win_w, p720 ? 1280 : 1920);
		WRITE_ONCE(fp_win_h, p720 ? 1470 : 2205);
	}

	fp_modesets++;
	if (fp) {
		fp_attr(true);
		fp_flag(true);
	} else if (fp_on || fp_attr_forced) {
		fp_flag(false);
		fp_attr(false);
	}
	if (fp != fp_on || fp) {
		fp_on = fp;
		fp_work_on = fp;
		mod_delayed_work(system_wq, &fp_work, msecs_to_jiffies(fp ? 600 : 50));
	}
	return 0;
}

static struct kprobe sdma_probe = {
	.pre_handler = sdma_pre,
};
static bool sdma_probe_on;

static int kwrite(const char *path, const char *val)
{
	struct file *f = filp_open(path, O_WRONLY, 0);
	loff_t pos = 0;
	ssize_t n;

	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, val, strlen(val), &pos);
	filp_close(f, NULL);
	return n < 0 ? n : 0;
}

struct kread_args {
	const char *path;
	char *buf;
	size_t size;
};

static void kread_now(const char *path, char *buf, size_t size);

static void kread_fn(void *a)
{
	struct kread_args *r = a;

	kread_now(r->path, r->buf, r->size);
}

static void kread(const char *path, char *buf, size_t size)
{
	struct kread_args r = { path, buf, size };

	in_worker(kread_fn, &r);
}

static void kread_now(const char *path, char *buf, size_t size)
{
	struct file *f = filp_open(path, O_RDONLY, 0);
	loff_t pos = 0;
	ssize_t n = 0;

	buf[0] = 0;
	if (IS_ERR(f))
		return;
	n = kernel_read(f, buf, size - 1, &pos);
	filp_close(f, NULL);
	buf[n > 0 ? n : 0] = 0;
	buf[strcspn(buf, "\n")] = 0;
}

#define HDMITX_SYS	"/sys/class/amhdmitx/amhdmitx0/"
#define DV_PARAMS	"/sys/module/aml_media/parameters/"

/* after a frame packed mode set: the VSIF (and the flag) by "3dfp", then the PHY on; after leaving it: "3doff" */
static void fp_work_fn(struct work_struct *w)
{
	if (READ_ONCE(fp_work_on)) {
		kwrite(HDMITX_SYS "config", "3dfp");
		kwrite(HDMITX_SYS "phy", "1");
		/* the video window now (the probe keeps it so) */
		kwrite("/sys/class/video/axis", fp_win_h == 1470 ? "0 0 1279 1469" : "0 0 1919 2204");
		pr_info(DRV_NAME ": HDMI frame packing on (%s)\n",
			(char *)*(void **)get_current_vinfo() ?: "?");
	} else {
		kwrite(HDMITX_SYS "config", "3doff");
		pr_info(DRV_NAME ": HDMI frame packing off\n");
	}
}

/* (process context) a client starts / stops frame packed playback */
static void fp_arm(bool on)
{
	if (on == fp_armed)
		return;
	if (on) {
		kread(DV_PARAMS "dolby_vision_policy", fp_dv_policy, sizeof(fp_dv_policy));
		kread(DV_PARAMS "dolby_vision_mode", fp_dv_mode, sizeof(fp_dv_mode));
		fp_dv_saved = fp_dv_policy[0] && fp_dv_mode[0];
		sysfs_write(DV_PARAMS "dolby_vision_policy", "2");
		sysfs_write(DV_PARAMS "dolby_vision_mode", "5");
		sysfs_write(HDMITX_SYS "debug", "hpd_lock1");
		WRITE_ONCE(fp_armed, true);
		pr_info(DRV_NAME ": frame packing armed (Dolby Vision %s/%s -> 2/5, HPD held)\n", fp_dv_policy,
			fp_dv_mode);
	} else {
		WRITE_ONCE(fp_armed, false);
		sysfs_write(HDMITX_SYS "debug", "hpd_lock0");
		if (fp_dv_saved) {
			sysfs_write(DV_PARAMS "dolby_vision_policy", fp_dv_policy);
			sysfs_write(DV_PARAMS "dolby_vision_mode", fp_dv_mode);
		}
		pr_info(DRV_NAME ": frame packing disarmed (the next mode set is 2D)\n");
	}
}

/* the GUI on a 2160p Android mode with 1080p frame packing out: its destination halved (left view lines) */
static int plane_check_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct drm_plane_state *state = (struct drm_plane_state *)regs->regs[1];

	if (!READ_ONCE(fp_on) || !state || !state->crtc)
		return 0;
	if (state->crtc_w > 1920 || state->crtc_h > 1080 || state->crtc_x >= 1920 || state->crtc_y >= 1080) {
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



/* the video window while frame packed: the whole frame (Android's composer gives it in its 2160p mode) */

static int fp_axis_h;
module_param(fp_axis_h, int, 0644);
MODULE_PARM_DESC(fp_axis_h, "tests: the video window height while frame packed (0: the frame packed frame)");

static int win_pre(struct kprobe *p, struct pt_regs *regs)
{
	int *a = (int *)regs->regs[1];

	if (!READ_ONCE(fp_on) || !a)
		return 0;
	a[0] = 0;
	a[1] = 0;
	a[2] = READ_ONCE(fp_win_w) - 1;
	a[3] = (READ_ONCE(fp_axis_h) > 0 ? READ_ONCE(fp_axis_h) : READ_ONCE(fp_win_h)) - 1;
	return 0;
}

static struct kprobe win_probe = {
	.symbol_name = "_set_video_window",
	.pre_handler = win_pre,
};
static bool win_probe_on;

/*
 * The VPP's source size of an MVC picture in 3D frame alternative mode (MODE_3D_FA, no 2D, no LR): the Dune
 * 5.4 vpp_get_video_source_size() (inlined in vpp_set_filters) takes frame_height << 1 = 4410 for the 2205
 * line frame packed picture -> each view stretched to twice its height (projector 11-05). CoreELEC's 5.15
 * kernel has the fix: "else if (process_3d_type & MODE_3D_MVC) src = vf->width x vf->height". Done here at
 * the instruction: r25 vpp_set_filters+0x3120, the branch right after "lsl w0, w0, #1" (w0 = src_height,
 * w8 = process_3d_type); the code around it is checked first.
 */
#define VSF_FIX_OFF	0x3120
static const u32 vsf_code[6] = { 0xb9409fe0, 0xb85d43a1, 0x52800051, 0xb900be71, 0x531f7800, 0x17fff4c5 };
static u64 vsf_fixed;
static int vsf_fix = 1;
module_param(vsf_fix, int, 0644);
MODULE_PARM_DESC(vsf_fix, "frame packing: the VPP source height of the 3D frame alternative MVC picture as CoreELEC 5.15 has it");

static int vsf_pre(struct kprobe *p, struct pt_regs *regs)
{
	/* only our MVC pictures reach the 3D path while frame packing is on */
	if (READ_ONCE(vsf_fix) && READ_ONCE(fp_on) && (regs->regs[8] & 0x21) == 0x21) {
		regs->regs[0] = (u32)regs->regs[0] >> 1;
		vsf_fixed++;
	}
	return 0;
}

static struct kprobe vsf_probe = {
	.symbol_name = "vpp_set_filters",
	.offset = VSF_FIX_OFF,
	.pre_handler = vsf_pre,
};
static bool vsf_probe_on;

/*
 * The VD1/VD2 MIF picture select of a frame packed MVC picture. CoreELEC's 5.15 vdx_set_dcu() has a first
 * branch "if (framepacking_support) psel = 0" (VD1 = the left eye, 0..1079, VD2 = the right eye, measured on
 * the same box 11-06); the Dune 5.4 vd_set_dcu() does not: vpp_2pic_mode 2 of the 3D frame alternative mode
 * takes "progressive frame in two pictures", PSEL 0x0a000201 - lines of both views in turn, the transparent
 * curtain on the projector. Done at the instruction: r25 vd_set_dcu+0x18b0 "and w9, w8, #3" (w8 =
 * vpp_2pic_mode), so the code goes on to the branch writing PSEL 0; the code around it is checked first.
 */
#define PSEL_FIX_OFF	0x18b0
static const u32 psel_code[6] = { 0xb940bf08, 0x52820229, 0x6a0902df, 0x54000440, 0x12000509, 0x35000409 };
static u64 psel_fixed;
static int psel_fix = 1;
module_param(psel_fix, int, 0644);
MODULE_PARM_DESC(psel_fix, "frame packing: no two picture mode in the VD MIF (as CoreELEC 5.15 has it)");

static int psel_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(psel_fix) && READ_ONCE(fp_on) && (regs->regs[8] & 3)) {
		regs->regs[8] = 0;
		psel_fixed++;
	}
	return 0;
}

static struct kprobe psel_probe = {
	.symbol_name = "vd_set_dcu",
	.offset = PSEL_FIX_OFF,
	.pre_handler = psel_pre,
};
static bool psel_probe_on;

/*
 * The picture type of an MVC picture in 3D mode. The Dune 5.4 video_vf_get() (pre_process_for_3d inlined)
 * makes it VIDTYPE_VIU_FIELD ("str w10, [x0, #0xc]", w10 = 0x1000) as soon as process_3d_type has
 * MODE_3D_ENABLE and vf->trans_fmt is set; CoreELEC's 5.15 only sets MODE_3D_MVC and keeps VIDTYPE_MVC - all
 * of its frame packing depends on that bit (VD2 for the right eye, zoom_end_y 1079 with the blank lines). So
 * the store writes the type the picture already has; the code around it is checked first (r25
 * video_vf_get+0x154).
 */
#define VFTYPE_FIX_OFF	0x154
static const u32 vftype_code[6] = { 0x5282000a, 0x321501ab, 0, 0x52800030, 0x6a1101bf, 0xb9000c0a }; /* [2]: an adrp, relocated */
static u64 vftype_fixed;
static int vftype_fix = 1;
module_param(vftype_fix, int, 0644);
MODULE_PARM_DESC(vftype_fix, "frame packing: an MVC picture stays VIDTYPE_MVC in 3D mode (as CoreELEC 5.15 has it)");

static int vftype_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 type = *(u32 *)((u8 *)regs->regs[0] + 12);

	if (READ_ONCE(vftype_fix) && READ_ONCE(fp_on) && (type & VIDTYPE_MVC)) {
		regs->regs[10] = type;
		vftype_fixed++;
	}
	return 0;
}

static struct kprobe vftype_probe = {
	.symbol_name = "video_vf_get",
	.offset = VFTYPE_FIX_OFF,
	.pre_handler = vftype_pre,
};
static bool vftype_probe_on;

/*
 * Two more places where CoreELEC's 5.15 knows MVC and the Dune 5.4 does not:
 * - vpp_set_filters: "if (!(vf->type & VIDTYPE_MVC) && h_in > src_height_max ...) vscale_skip_count++": the
 *   Dune code skips every second line of the 2205 line picture (h_in > 2160). r25 +0xfec "cmp w10, w9".
 * - config_vd_position (get_3d_vert_pos): frame alternative with MODE_3D_MVC keeps the whole view, the Dune
 *   mask 0x32f600 lacks MODE_3D_MVC (0x800) and halves it. r25 +0x408 "tst w0, w15".
 */
#define VSKIP_FIX_OFF	0xfec
static const u32 vskip_code[6] = { 0xb9409fe9, 0xd3441108, 0xf81b03a8, 0x6b09015f, 0x54000082, 0xb940ae68 };
#define VDPOS_FIX_OFF	0x408
static const u32 vdpos_code[5] = { 0xb945868a, 0x529ec00f, 0x72a0064f, 0x6a0f001f, 0x54000600 };
static u64 vskip_fixed, vdpos_fixed;
static int vskip_fix = 1, vdpos_fix = 1;
module_param(vskip_fix, int, 0644);
module_param(vdpos_fix, int, 0644);

static int vskip_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(vskip_fix) && READ_ONCE(fp_on)) {
		if ((u32)regs->regs[10] < (u32)regs->regs[9]) {
			regs->regs[10] = regs->regs[9];
			vskip_fixed++;
		}
	}
	return 0;
}

static int vdpos_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(vdpos_fix) && READ_ONCE(fp_on) && (regs->regs[0] & 0x821) == 0x821) {
		regs->regs[15] |= 0x800;
		vdpos_fixed++;
	}
	return 0;
}

static struct kprobe vskip_probe = {
	.symbol_name = "vpp_set_filters",
	.offset = VSKIP_FIX_OFF,
	.pre_handler = vskip_pre,
};
static struct kprobe vdpos_probe = {
	.symbol_name = "config_vd_position",
	.offset = VDPOS_FIX_OFF,
	.pre_handler = vdpos_pre,
};
static bool vskip_probe_on, vdpos_probe_on;

/*
 * amvideo's pre_process_for_3d() takes an MVC picture as the 3D frame alternative one (no type change: vftype_fix,
 * process_3d_type |= MODE_3D_MVC) only if vf->trans_fmt is set - CoreELEC's amvdec_h264mvc sets
 * TVIN_TFMT_3D_TB and both eye windows in view_mode 3, the Dune decoder leaves them 0 (process_3d_type lost
 * MODE_3D_MVC on every picture, 11-05). They are set here (struct vframe_s: type +12, width +308, height
 * +312, trans_fmt +360, left_eye +376, right_eye +392 {start_x, start_y, width, height} - the 5.4.125
 * layout, checked by the values first).
 */
#define VF_TYPE_OFF	12
#define VF_WIDTH_OFF	308
#define VF_HEIGHT_OFF	312
#define VF_TRANS_OFF	360
#define VF_LEFT_OFF	376
#define VF_RIGHT_OFF	392
#define TVIN_TFMT_3D_TB_	5

static int fp_conv = 1;
module_param(fp_conv, int, 0644);
MODULE_PARM_DESC(fp_conv, "frame packing: the MVC pictures get trans_fmt top-bottom and the eye windows (as CoreELEC's decoder)");
static u64 fp_conv_done, fp_conv_skip;

static void fp_conv_vf(struct vframe_s *vf)
{
	u8 *p = (u8 *)vf;
	u32 *type = (u32 *)(p + VF_TYPE_OFF), w = *(u32 *)(p + VF_WIDTH_OFF), h = *(u32 *)(p + VF_HEIGHT_OFF);
	u32 *trans = (u32 *)(p + VF_TRANS_OFF), *le = (u32 *)(p + VF_LEFT_OFF), *re = (u32 *)(p + VF_RIGHT_OFF);
	static bool logged;

	if (!READ_ONCE(fp_conv) || !(*type & VIDTYPE_MVC))
		return;
	if (!logged) {
		logged = true;
		pr_info(DRV_NAME ": MVC picture %ux%u trans_fmt %u left %u %u %u %u right %u %u %u %u\n", w, h, *trans,
			le[0], le[1], le[2], le[3], re[0], re[1], re[2], re[3]);
	}
	if (!((w == 1920 && h == 1080) || (w == 1280 && h == 720)) || (*trans && *trans != TVIN_TFMT_3D_TB_) ||
	    (le[2] && le[2] != w) || (re[2] && re[2] != w)) {
		fp_conv_skip++;
		return;
	}
	*trans = TVIN_TFMT_3D_TB_;
	le[0] = 0; le[1] = 0; le[2] = w; le[3] = h;
	re[0] = 0; re[1] = 0; re[2] = w; re[3] = h;
	fp_conv_done++;
}

static bool vsf_code_ok(void)
{
	struct kprobe kp = { .symbol_name = "vpp_set_filters" };
	u32 w[6];
	u8 *a;

	if (register_kprobe(&kp))
		return false;
	a = (u8 *)kp.addr;
	unregister_kprobe(&kp);
	return !probe_kernel_read(w, a + VSF_FIX_OFF - 0x14, sizeof(w)) && !memcmp(w, vsf_code, sizeof(w));
}

static bool code_ok(const char *sym, unsigned int off, const u32 *code, size_t n)
{
	struct kprobe kp = { .symbol_name = sym };
	u32 w[8];
	u8 *a;

	if (n > sizeof(w) || register_kprobe(&kp))
		return false;
	a = (u8 *)kp.addr;
	unregister_kprobe(&kp);
	return !probe_kernel_read(w, a + off, n) && !memcmp(w, code, n);
}

static bool vftype_code_ok(void)
{
	struct kprobe kp = { .symbol_name = "video_vf_get" };
	u32 w[6];
	u8 *a;

	if (register_kprobe(&kp))
		return false;
	a = (u8 *)kp.addr;
	unregister_kprobe(&kp);
	if (probe_kernel_read(w, a + VFTYPE_FIX_OFF - 0x14, sizeof(w)))
		return false;
	w[2] = 0;
	return !memcmp(w, vftype_code, sizeof(w));
}

static bool psel_code_ok(void)
{
	struct kprobe kp = { .symbol_name = "vd_set_dcu" };
	u32 w[6];
	u8 *a;

	if (register_kprobe(&kp))
		return false;
	a = (u8 *)kp.addr;
	unregister_kprobe(&kp);
	return !probe_kernel_read(w, a + PSEL_FIX_OFF - 0x10, sizeof(w)) && !memcmp(w, psel_code, sizeof(w));
}

/* the r25 hdmitx20 set_disp_mode_auto: its first instructions (sub sp; str x30,[x18]; stp x29,x30) */
static bool sdma_looks_right(const u32 *a)
{
	u32 w[3];

	if (probe_kernel_read(w, a, sizeof(w)))
		return false;
	return (w[0] & 0xffc003ff) == 0xd10003ff &&	/* sub sp, sp, #imm */
	       w[1] == 0xf800865e &&			/* str x30, [x18], #8 */
	       (w[2] & 0xffc07fff) == 0xa9007bfd;	/* stp x29, x30, [sp, #imm] */
}

static void fp_init(void)
{
	u8 *hd = get_hdmitx_device();
	struct kprobe kp = { .symbol_name = "hdmitx_set_enc_hw" };
	char attr[FP_ATTR_LEN + 1];
	void *enc;

	/* the struct offsets of the r25 binary, checked by their values */
	memcpy(attr, hd + FP_ATTR_OFF, FP_ATTR_LEN);
	attr[FP_ATTR_LEN] = 0;
	if ((hd[FP_FLAGS_OFF] & 7) || !strstr(attr, "bit") ||
	    (*(unsigned long *)(hd + FP_CURPARAM_OFF) >> 48) != 0xffff) {
		pr_warn(DRV_NAME ": hdmitx layout not the r25 one (flags %#x, attr \"%s\"): no frame packing\n",
			hd[FP_FLAGS_OFF], attr);
		return;
	}
	if (register_kprobe(&kp)) {
		pr_warn(DRV_NAME ": no hdmitx_set_enc_hw: no frame packing\n");
		return;
	}
	enc = kp.addr;
	unregister_kprobe(&kp);
	sdma_probe.addr = (kprobe_opcode_t *)((u8 *)enc + SDMA_FROM_ENC);
	if (!sdma_looks_right((u32 *)sdma_probe.addr)) {
		pr_warn(DRV_NAME ": set_disp_mode_auto not where the r25 binary has it: no frame packing\n");
		return;
	}
	INIT_DELAYED_WORK(&fp_work, fp_work_fn);
	vinfo_probe_on = !register_kretprobe(&vinfo_probe);
	sdma_probe_on = vinfo_probe_on && !register_kprobe(&sdma_probe);
	if (!sdma_probe_on) {
		if (vinfo_probe_on)
			unregister_kretprobe(&vinfo_probe);
		vinfo_probe_on = false;
		pr_warn(DRV_NAME ": no mode set probes: no frame packing\n");
		return;
	}
	plane_probe_on = !register_kprobe(&plane_check_probe);
	win_probe_on = !register_kprobe(&win_probe);
	if (vsf_code_ok())
		vsf_probe_on = !register_kprobe(&vsf_probe);
	if (!vsf_probe_on)
		pr_warn(DRV_NAME ": vpp_set_filters not the r25 code: 3D picture height not fixed\n");
	if (psel_code_ok())
		psel_probe_on = !register_kprobe(&psel_probe);
	if (!psel_probe_on)
		pr_warn(DRV_NAME ": vd_set_dcu not the r25 code: VD MIF two picture mode stays\n");
	if (vftype_code_ok())
		vftype_probe_on = !register_kprobe(&vftype_probe);
	if (!vftype_probe_on)
		pr_warn(DRV_NAME ": video_vf_get not the r25 code: MVC pictures lose their type in 3D mode\n");
	if (code_ok("vpp_set_filters", VSKIP_FIX_OFF - 0xc, vskip_code, sizeof(vskip_code)))
		vskip_probe_on = !register_kprobe(&vskip_probe);
	if (code_ok("config_vd_position", VDPOS_FIX_OFF - 0xc, vdpos_code, sizeof(vdpos_code)))
		vdpos_probe_on = !register_kprobe(&vdpos_probe);
	if (!vskip_probe_on || !vdpos_probe_on)
		pr_warn(DRV_NAME ": VPP not the r25 code: vskip fix %d, position fix %d\n", vskip_probe_on, vdpos_probe_on);
	fp_ok = true;
	pr_info(DRV_NAME ": HDMI frame packing ready (attr \"%s\", GUI probe %s)\n", attr,
		plane_probe_on ? "on" : "off");
}

static void fp_exit(void)
{
	if (plane_probe_on)
		unregister_kprobe(&plane_check_probe);
	if (win_probe_on)
		unregister_kprobe(&win_probe);
	if (vsf_probe_on)
		unregister_kprobe(&vsf_probe);
	if (psel_probe_on)
		unregister_kprobe(&psel_probe);
	if (vftype_probe_on)
		unregister_kprobe(&vftype_probe);
	if (vskip_probe_on)
		unregister_kprobe(&vskip_probe);
	if (vdpos_probe_on)
		unregister_kprobe(&vdpos_probe);
	if (sdma_probe_on)
		unregister_kprobe(&sdma_probe);
	if (vinfo_probe_on)
		unregister_kretprobe(&vinfo_probe);
	if (fp_ok)
		cancel_delayed_work_sync(&fp_work);
	if (fp_on || fp_attr_forced) {
		fp_flag(false);
		fp_attr(false);
	}
}

/*
 * (process context) the VPP's 3D type, as AMSTREAM_IOC_SET_3D_TYPE on /dev/amvideo does it for a player
 * (CoreELEC's Kodi: MODE_3D_ENABLE | MODE_3D_FA with framepacking_support for MVC frame packing): VD1 reads the
 * left eye, VD2 the right eye, lines 0-1079 and 1125-2204 of the 2205 line frame. 0: off.
 */
#ifndef MODE_3D_ENABLE
#define MODE_3D_ENABLE	0x00000001
#define MODE_3D_FA	0x00000020
#endif
#define AMVIDEO_IOC_SET_3D_TYPE	_IOW('S', 0x3c, u32)

struct type3d_args {
	u32 type;
	int ret;
};

static int fp_3d_type_now(u32 type);

static void type3d_fn(void *a)
{
	struct type3d_args *t = a;

	t->ret = fp_3d_type_now(t->type);
}

static int fp_3d_type(u32 type)
{
	struct type3d_args t = { type, 0 };

	in_worker(type3d_fn, &t);
	return t.ret;
}

static int fp_3d_type_now(u32 type)
{
	struct file *f = filp_open("/dev/amvideo", O_RDWR, 0);
	long r = -ENOTTY;

	if (IS_ERR(f)) {
		pr_warn(DRV_NAME ": /dev/amvideo: %ld\n", PTR_ERR(f));
		return PTR_ERR(f);
	}
	if (f->f_op && f->f_op->unlocked_ioctl)
		r = f->f_op->unlocked_ioctl(f, AMVIDEO_IOC_SET_3D_TYPE, type);
	filp_close(f, NULL);
	pr_info(DRV_NAME ": 3D type %#x: %ld\n", type, r);
	return r;
}

/*
 * A client gone before its decoder (killed: this file closes before its amstream): the decoder's end comes through
 * our route only, so the route stays a moment for it - without it the "registered" decoder stayed for ever and every
 * later start was refused (11-06). A decoder still there after that is forgotten.
 */
static void route_off_fn(struct work_struct *w)
{
	struct sc2_dev *h = gd;
	unsigned long flags;
	bool stale;

	mutex_lock(&h->mlock);
	if (!h->started) {
		spin_lock_irqsave(&h->lock, flags);
		stale = h->prov_reg;
		h->prov_reg = false;
		if (stale)
			forget_all_locked(h);
		spin_unlock_irqrestore(&h->lock, flags);
		if (stale) {
			pr_warn(DRV_NAME ": a decoder without a client: forgotten\n");
			vf_unreg_provider(&h->prov);
			pool_decoder_gone();
		}
		route(h, false);
	}
	mutex_unlock(&h->mlock);
}
static DECLARE_DELAYED_WORK(route_work, route_off_fn);

static void hw_stop(struct sc2_dev *h)
{
	unsigned long flags;
	bool decoder;

	spin_lock_irqsave(&h->lock, flags);
	h->started = false;
	drop_all_locked(h);
	decoder = h->prov_reg;
	spin_unlock_irqrestore(&h->lock, flags);
	if (decoder)
		schedule_delayed_work(&route_work, HZ);
	else
		route(h, false);
	set_freerun_mode(0);
	sysfs_write(MVC_PARAMS "view_mode", "3");
	if (h->fp) {
		sysfs_write(FP_PARAM, "0");
		fp_3d_type(0);
		fp_arm(false);
	}
	h->fp = false;
	pr_info(DRV_NAME ": stop (in %llu, shown %llu, late drops %llu, client drops %llu)\n", h->pics_in,
		h->shown, h->dropped_late, h->dropped_client);
	/* a decoder still open (the client died before its amstream) keeps the pool until it goes */
	pool_release_if_idle(h);
}

static long ioc_start(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_start s;
	unsigned long flags;
	int r;

	if (copy_from_user(&s, p, sizeof(s)))
		return -EFAULT;
	if (h->started)
		return -EBUSY;
	if (!s.width || s.width > 1920 || !s.height || s.height > 1088 || s.mode > MVC3D_HW_FP)
		return -EINVAL;
	if (!ref_probe_on || !alloc_probe_on) {
		pr_warn(DRV_NAME ": no probes on the MVC decoder's allocations: hardware MVC refused (its 31 bit DMA corrupts memory)\n");
		return -ENODEV;
	}
	if (s.mode == MVC3D_HW_FP && !fp_ok)
		return -ENODEV;
	/* (the last client's decoder, its process going: a moment for it) */
	for (r = 0; h->routed && r < 75 && READ_ONCE(h->prov_reg); r++)
		msleep(20);
	cancel_delayed_work(&route_work);
	if (READ_ONCE(h->prov_reg))
		return -EBUSY;	/* a decoder of another player is open */
	r = pool_get(h->misc.this_device);
	if (r)
		return r;
	pool_used = 0;
	pool_slots = 0;
	pool_same_decoder = false;
	r = route(h, true);
	if (r) {
		pool_put();
		return r;
	}
	h->fp = s.mode == MVC3D_HW_FP;
	sysfs_write(MVC_PARAMS "view_mode", h->fp ? "3" : READ_ONCE(hw_2d_view) ? "1" : "0");
	sysfs_write(MVC_PARAMS "drop_thread_hold", "0");
	if (h->fp) {
		sysfs_write(FP_PARAM, "1");
		/* the VPP's frame packing of the views (as CoreELEC's Kodi: 3D on, frame alternative) */
		fp_3d_type(MODE_3D_ENABLE | MODE_3D_FA);
		fp_arm(true);
	}
	/* amvideo shows what it peeks: the time is ours */
	set_freerun_mode(1);
	spin_lock_irqsave(&h->lock, flags);
	drop_all_locked(h);
	h->started = true;
	h->gen++;
	h->pics_in = h->shown = h->dropped_late = h->dropped_client = h->released = 0;
	h->put_back = h->stale_put = h->late_us_sum = h->late_cnt = 0;
	h->late_max_us = 0;
	s.gen = h->gen;
	spin_unlock_irqrestore(&h->lock, flags);
	s.out_height = h->fp ? 2 * s.height + (s.height == 720 ? 30 : 45) : s.height;
	s.stride = 0;
	s.reserved[0] = MVC3D_HW_F_DIRECT;
	s.reserved[1] = 0;
	pr_info(DRV_NAME ": start %ux%u %s (direct)\n", s.width, s.height, h->fp ? "frame packed" : "2D");
	kick(h);
	return copy_to_user(p, &s, sizeof(s)) ? -EFAULT : 0;
}

static long ioc_wait(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_pic w;
	unsigned long flags;
	long r;
	int i = -1;

	if (copy_from_user(&w, p, sizeof(w)))
		return -EFAULT;
	r = wait_event_interruptible_timeout(h->wq_client, READ_ONCE(h->ready_cnt) || !h->started,
					     msecs_to_jiffies(w.timeout_ms));
	if (r < 0)
		return r;
	spin_lock_irqsave(&h->lock, flags);
	if (h->ready_cnt) {
		struct slot *s;

		i = h->ready[h->ready_head];
		h->ready_head = (h->ready_head + 1) % NSLOT;
		h->ready_cnt--;
		s = &h->slots[i];
		s->state = S_CLIENT;
		w.index = i;
		w.seq = s->seq;
		w.gen = s->gen;
		w.decoded_ns = s->decoded_ns;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	if (i < 0)
		return -EAGAIN;
	return copy_to_user(p, &w, sizeof(w)) ? -EFAULT : 0;
}

static long ioc_present(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_present q;
	unsigned long flags;
	long r = 0;

	if (copy_from_user(&q, p, sizeof(q)))
		return -EFAULT;
	spin_lock_irqsave(&h->lock, flags);
	if (q.index < 0 || q.index >= NSLOT || h->slots[q.index].state != S_CLIENT) {
		r = -EINVAL;
	} else {
		struct slot *s = &h->slots[q.index];
		int k = h->disp_cnt;

		/* in due order (the player presents in display order: almost always at the end) */
		s->due_ns = q.due_ns;
		s->state = S_QUEUED;
		while (k > 0 && h->slots[h->dispq[(h->disp_head + k - 1) % NSLOT]].due_ns > q.due_ns) {
			h->dispq[(h->disp_head + k) % NSLOT] = h->dispq[(h->disp_head + k - 1) % NSLOT];
			k--;
		}
		h->dispq[(h->disp_head + k) % NSLOT] = q.index;
		h->disp_cnt++;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	if (!r)
		vf_notify_receiver(HW_NAME, VFRAME_EVENT_PROVIDER_VFRAME_READY, NULL);
	return r;
}

static long ioc_release(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_buf a;
	unsigned long flags;
	long r = 0;

	if (copy_from_user(&a, p, sizeof(a)))
		return -EFAULT;
	spin_lock_irqsave(&h->lock, flags);
	if (a.index < 0 || a.index >= NSLOT || h->slots[a.index].state != S_CLIENT) {
		r = -EINVAL;
	} else {
		slot_put_locked(h, a.index);
		h->dropped_client++;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	kick(h);
	return r;
}

static long hw_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct sc2_dev *h = gd;
	void __user *p = (void __user *)arg;
	unsigned long flags;
	long r;

	if (cmd == MVC3D_HW_START) {
		mutex_lock(&h->mlock);
		if (!h->owner)
			h->owner = f;
		mutex_unlock(&h->mlock);
	}
	if (h->owner != f)
		return -EBUSY;
	switch (cmd) {
	case MVC3D_HW_START:
		mutex_lock(&h->mlock);
		r = ioc_start(h, p);
		mutex_unlock(&h->mlock);
		return r;
	case MVC3D_HW_ADD_BUF:
		return -EOPNOTSUPP;	/* MVC3D_HW_F_DIRECT */
	case MVC3D_HW_WAIT:
		return ioc_wait(h, p);
	case MVC3D_HW_PRESENT:
		return ioc_present(h, p);
	case MVC3D_HW_RELEASE:
		return ioc_release(h, p);
	case MVC3D_HW_FLUSH:
		spin_lock_irqsave(&h->lock, flags);
		drop_all_locked(h);
		h->gen++;
		r = h->gen;
		spin_unlock_irqrestore(&h->lock, flags);
		kick(h);
		return put_user((u32)r, (u32 __user *)p);
	case MVC3D_HW_SET_FP: {
		u32 on;

		if (copy_from_user(&on, p, sizeof(on)))
			return -EFAULT;
		/* frame packing is armed by MVC3D_HW_START in MVC3D_HW_FP mode */
		return on && !fp_ok ? -ENODEV : 0;
	}
	}
	return -ENOTTY;
}

static int hw_open(struct inode *inode, struct file *f)
{
	return 0;	/* the first MVC3D_HW_START makes this file the owner */
}

/*
 * The player, before its film (no ioctl from Java): write "arm" on a file of this device = frame packing wanted
 * until the film's MVC3D_HW_STOP or the file's close, so the mode set the player does now (before the decoder
 * opens) is already the frame packed one. read: "armed=N on=N started=N\n", on = the HDMI output is frame packed,
 * started = the hardware decoder runs (not: the player has no 3D, it gives the mode back).
 */
static struct file *fp_armer;

static ssize_t hw_write(struct file *f, const char __user *u, size_t n, loff_t *pos)
{
	struct sc2_dev *h = gd;
	char c[8] = "";
	int r = n;

	if (copy_from_user(c, u, min(n, sizeof(c) - 1)))
		return -EFAULT;
	if (strncmp(c, "arm", 3))
		return -EINVAL;
	if (!fp_ok)
		return -ENODEV;
	mutex_lock(&h->mlock);
	if (fp_armer && fp_armer != f) {
		r = -EBUSY;
	} else {
		fp_armer = f;
		fp_arm(true);
	}
	mutex_unlock(&h->mlock);
	return r;
}

static ssize_t hw_read(struct file *f, char __user *u, size_t n, loff_t *pos)
{
	char c[48];
	loff_t at = 0;	/* always the whole line: a status */
	int len = scnprintf(c, sizeof(c), "armed=%d on=%d started=%d\n", READ_ONCE(fp_armed), READ_ONCE(fp_on),
			    READ_ONCE(gd->started));

	return simple_read_from_buffer(u, n, &at, c, len);
}

static int hw_release(struct inode *inode, struct file *f)
{
	struct sc2_dev *h = gd;

	mutex_lock(&h->mlock);
	if (h->owner == f) {
		if (h->started)
			hw_stop(h);
		h->owner = NULL;
	}
	if (fp_armer == f) {
		fp_armer = NULL;
		if (!h->started)
			fp_arm(false);
	}
	mutex_unlock(&h->mlock);
	return 0;
}

static const struct file_operations hw_fops = {
	.owner = THIS_MODULE,
	.open = hw_open,
	.release = hw_release,
	.read = hw_read,
	.write = hw_write,
	.llseek = noop_llseek,
	.unlocked_ioctl = hw_ioctl,
	.compat_ioctl = hw_ioctl,
};

static int stats_show(struct seq_file *m, void *v)
{
	struct sc2_dev *h = gd;
	unsigned long flags;
	char st[NSLOT + 1];
	int i;

	spin_lock_irqsave(&h->lock, flags);
	for (i = 0; i < NSLOT; i++)
		st[i] = "-RCQSX"[h->slots[i].state];
	st[i] = 0;
	seq_printf(m, "mvc3d %s: client %s, %s, decoder %s, route %s\n", SC2_VERSION, h->owner ? "yes" : "no",
		   h->started ? (h->fp ? "frame packed" : "2D") : "stopped", h->prov_reg ? "registered" : "none",
		   h->routed ? "here" : "default");
	seq_printf(m, "slots %s (Ready Client Queued Shown X-retired), ready %d, queued %d, vsync %u us, lead %llu us\n", st,
		   h->ready_cnt, h->disp_cnt, (u32)div_u64(h->vs_period_ns, 1000), div_u64(lead_ns_locked(h), 1000));
	seq_printf(m, "pictures in %llu (seq %u), shown %llu, late drops %llu, client drops %llu, back %llu, stale puts %llu\n",
		   h->pics_in, h->seq, h->shown, h->dropped_late, h->dropped_client, h->put_back, h->stale_put);
	seq_printf(m, "decoder probes %s, low pool %#lx (%lu KiB used), buffers moved %d, not moved %d\n",
		   ref_probe_on && alloc_probe_on ? "on" : "OFF", pool_pfn << PAGE_SHIFT, pool_used >> 10,
		   atomic_read(&pool_redirects), atomic_read(&pool_misses));
	seq_printf(m, "frame packing %s, armed %d, on %d, mode sets %llu, vinfo maps %llu, VPP height fixes %llu, PSEL fixes %llu, type fixes %llu, vskip fixes %llu, position fixes %llu, MVC pictures marked %llu (skipped %llu)\n",
		   fp_ok ? "ready" : "OFF", fp_armed, fp_on, fp_modesets, fp_maps, vsf_fixed, psel_fixed, vftype_fixed, vskip_fixed, vdpos_fixed, fp_conv_done, fp_conv_skip);
	seq_printf(m, "shown after due: avg %lld us, max %lld us; default map \"%s\"\n",
		   h->late_cnt ? (s64)div64_s64((s64)h->late_us_sum, h->late_cnt) : 0, h->late_max_us,
		   h->default_chain);
	spin_unlock_irqrestore(&h->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int __init sc2_init(void)
{
	struct sc2_dev *h = kzalloc(sizeof(*h), GFP_KERNEL);
	int r;

	if (!h)
		return -ENOMEM;
	mutex_init(&h->mlock);
	spin_lock_init(&h->lock);
	init_waitqueue_head(&h->wq_client);
	init_waitqueue_head(&h->wq_thread);
	gd = h;
	vf_receiver_init(&h->recv, HW_NAME, &sc2_recv_ops, h);
	vf_provider_init(&h->prov, HW_NAME, &sc2_vf_ops, h);
	r = vf_reg_receiver(&h->recv);
	if (r)
		goto fail;
	h->thread = kthread_run(pull_thread, h, HW_NAME);
	if (IS_ERR(h->thread)) {
		r = PTR_ERR(h->thread);
		goto fail_recv;
	}
	h->misc.minor = MISC_DYNAMIC_MINOR;
	h->misc.name = HW_NAME;
	h->misc.fops = &hw_fops;
	h->misc.mode = 0666;
	r = misc_register(&h->misc);
	if (r)
		goto fail_thread;
	/* the pool's cache maintenance */
	h->misc.this_device->dma_mask = &sc2_dma_mask;
	h->misc.this_device->coherent_dma_mask = DMA_BIT_MASK(32);
	h->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("hw_stats", 0444, h->dbg, NULL, &stats_fops);
	r = register_kprobe(&ref_probe);
	ref_probe_on = !r;
	if (r)
		pr_warn(DRV_NAME ": no probe on amvdec_h264mvc init_canvas (%d): hardware MVC refused\n", r);
	alloc_probe_on = !register_kretprobe(&alloc_probe);
	fp_init();
	if (keep_pool && ref_probe_on && alloc_probe_on)
		pool_get(h->misc.this_device);
	pr_info(DRV_NAME ": %s ready (hardware MVC through amvideo)\n", SC2_VERSION);
	return 0;

fail_thread:
	kthread_stop(h->thread);
fail_recv:
	vf_unreg_receiver(&h->recv);
fail:
	gd = NULL;
	kfree(h);
	return r;
}

static void __exit sc2_exit(void)
{
	struct sc2_dev *h = gd;

	fp_exit();
	if (ref_probe_on)
		unregister_kprobe(&ref_probe);
	if (alloc_probe_on)
		unregister_kretprobe(&alloc_probe);
	cancel_work_sync(&pool_work);
	debugfs_remove_recursive(h->dbg);
	misc_deregister(&h->misc);
	mutex_lock(&h->mlock);
	if (h->started)
		hw_stop(h);
	mutex_unlock(&h->mlock);
	flush_delayed_work(&route_work);
	kthread_stop(h->thread);
	if (h->prov_reg)
		vf_unreg_provider(&h->prov);
	vf_unreg_receiver(&h->recv);
	/* with a decoder still open the pool stays (it may write there) */
	if (!h->prov_reg)
		pool_put();
	kfree(h);
	gd = NULL;
}

module_init(sc2_init);
module_exit(sc2_exit);
MODULE_DESCRIPTION("hardware 3D MVC through the legacy video layer (Amlogic S905X4)");
MODULE_LICENSE("GPL");
MODULE_VERSION(SC2_VERSION);
