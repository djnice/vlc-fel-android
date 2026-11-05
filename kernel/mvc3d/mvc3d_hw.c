// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d hardware MVC path (/dev/mvc3dhw), see mvc3d_uapi.h. Included by mvc3d.c.
 *
 * The legacy Amlogic MVC decoder (amvdec_h264mvc, provider "decoder.h264mvc", view_mode 3) hands out
 * one vframe per picture pair: canvas0_config = base view, canvas1_config = dependent view, 3 planes
 * each (Y, U, V; 4:2:0) in 32x32 blocks, row-major, linear inside a block, the bytes of every 64 bit
 * word reversed (measured on the S928X, libmvc3dplay tools/hwmvctest). The thread here composes it into a
 * client buffer (packed 4:2:2, UYVY) and gives the decoder frame back at once.
 */
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/swab.h>
#include <linux/amlogic/meson_uvm_core.h>
#include <linux/amlogic/media/meson_uvm_allocator.h>

#define HW_NAME		"mvc3dhw"
#define HW_PROVIDER	"decoder.h264mvc"
#define HW_MAP_ID	"mvc3dhw"
#define HW_MAP_CHAIN	HW_PROVIDER " " HW_NAME
#define HW_MAX_W	1920

int dmabuf_set_vframe(struct dma_buf *dmabuf, struct vframe_s *vf, enum uvm_hook_mod_type type);

enum hw_buf_state { HB_NONE, HB_FREE, HB_READY, HB_CLIENT };

struct hw_buf {
	struct dma_buf *db;
	phys_addr_t phys;
	size_t size;
	u8 *va;
	enum hw_buf_state state;
	u32 seq, gen;
	u64 decoded_ns;
};

struct hw_dev {
	struct miscdevice misc;
	struct mutex mlock;		/* client ioctls, start/stop */
	struct mutex plock;		/* the thread's decoder frame vs provider unreg */
	spinlock_t lock;		/* buffer states, ready queue */
	wait_queue_head_t wq_client, wq_thread;
	bool kick;
	struct task_struct *thread;
	struct vframe_receiver_s recv;

	struct file *owner;
	bool started, prov_reg, routed;
	u32 mode, w, h, out_h, stride, gap;
	u32 seq, gen;
	struct hw_buf bufs[MVC3D_HW_MAX_BUFS];
	int nbufs;
	int ready[MVC3D_HW_MAX_BUFS];
	int ready_head, ready_cnt;
	char default_chain[160];

	u64 frames_in, composed, waits_nobuf, bad_frames;
	u64 us_sum, us_max, us_cnt;
	u8 yl[HW_MAX_W], ul[HW_MAX_W / 2], vl[HW_MAX_W / 2];
};

static struct hw_dev *ghw;
static u64 hw_dma_mask = ~0ULL;

/* tests on a display without 3D: the 2D picture is the dependent (right) view */
static int hw_2d_view;
module_param(hw_2d_view, int, 0644);
MODULE_PARM_DESC(hw_2d_view, "hardware MVC in 2D: 0 the base view, 1 the dependent view (tests)");

/* ------------------------------------------------------------------ */
/* vfm route and decoder parameters                                   */
/* ------------------------------------------------------------------ */

static int hw_sysfs_write(const char *path, const char *val)
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

/* the chain of the map "default" from /sys/class/vfm/map: "[00]  default { decoder(0) ppmgr(0) amvideo}" */
static void hw_read_default(struct hw_dev *h)
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

		/* names without the "(0)" state */
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
			strscpy(h->default_chain, "decoder ppmgr deinterlace amvideo",
				sizeof(h->default_chain));
	}
	kfree(buf);
}

/*
 * "decoder.h264mvc" matches the map "default" by its prefix and the first map wins: while playing,
 * "default" is switched off (a removed map keeps its slot and comes back there)
 */
static int hw_route(struct hw_dev *h, bool on)
{
	char id[32], chain[160];

	if (on == h->routed)
		return 0;
	if (on) {
		hw_read_default(h);
		strscpy(id, "default", sizeof(id));
		vfm_map_remove(id);
		strscpy(id, HW_MAP_ID, sizeof(id));
		strscpy(chain, HW_MAP_CHAIN, sizeof(chain));
		vfm_map_remove(id);
		if (vfm_map_add(id, chain) < 0) {
			pr_err(HW_NAME ": cannot add vfm map %s\n", id);
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
			pr_err(HW_NAME ": cannot restore vfm map default (%s)\n", chain);
	}
	h->routed = on;
	pr_info(HW_NAME ": %s routed %s\n", HW_PROVIDER, on ? "here" : "back to default");
	return 0;
}

/* ------------------------------------------------------------------ */
/* composition                                                        */
/* ------------------------------------------------------------------ */

/* one line of a 32x32 block plane (bytes of each 64 bit word reversed) into linear bytes */
static void untile_line(u8 *dst, const u8 *plane, u32 cw, u32 y, u32 w)
{
	const u8 *row = plane + (size_t)(y >> 5) * (cw >> 5) * 1024 + ((y & 31) << 5);
	u32 bx;

	for (bx = 0; bx < (w + 31) >> 5; bx++) {
		const u64 *s = (const u64 *)(row + ((size_t)bx << 10));
		u64 *d = (u64 *)(dst + (bx << 5));

		d[0] = swab64(s[0]);
		d[1] = swab64(s[1]);
		d[2] = swab64(s[2]);
		d[3] = swab64(s[3]);
	}
}

struct hw_planes {
	u8 *p[3];
	u32 cw[3], size[3];
};

static int hw_map_planes(struct hw_planes *m, const struct canvas_config_s *c)
{
	int i;

	memset(m, 0, sizeof(*m));
	for (i = 0; i < 3; i++) {
		m->cw[i] = c[i].width;
		m->size[i] = c[i].width * c[i].height;
		if (!c[i].phy_addr || !m->size[i] || c[i].block_mode != CANVAS_BLKMODE_32X32)
			return -EINVAL;
		m->p[i] = codec_mm_vmap(c[i].phy_addr, m->size[i]);
		if (!m->p[i])
			return -ENOMEM;
		codec_mm_dma_flush(m->p[i], m->size[i], DMA_FROM_DEVICE);
	}
	return 0;
}

static void hw_unmap_planes(struct hw_planes *m)
{
	int i;

	for (i = 0; i < 3; i++)
		if (m->p[i])
			codec_mm_unmap_phyaddr(m->p[i]);
}

static void hw_compose_view(struct hw_dev *h, u8 *dst, const struct hw_planes *m)
{
	u32 y, x;

	for (y = 0; y < h->h; y++) {
		u8 *o = dst + (size_t)y * h->stride;

		untile_line(h->yl, m->p[0], m->cw[0], y, h->w);
		if (!(y & 1)) {
			untile_line(h->ul, m->p[1], m->cw[1], y >> 1, h->w >> 1);
			untile_line(h->vl, m->p[2], m->cw[2], y >> 1, h->w >> 1);
		}
		for (x = 0; x < h->w >> 1; x++) {
			o[4 * x] = h->ul[x];
			o[4 * x + 1] = h->yl[2 * x];
			o[4 * x + 2] = h->vl[x];
			o[4 * x + 3] = h->yl[2 * x + 1];
		}
	}
}

static int hw_compose(struct hw_dev *h, struct hw_buf *b, struct vframe_s *src)
{
	struct hw_planes m;
	int r;

	r = hw_map_planes(&m, h->mode != MVC3D_HW_FP && READ_ONCE(hw_2d_view) && (src->type & VIDTYPE_MVC) ?
			  src->canvas1_config : src->canvas0_config);
	if (!r) {
		hw_compose_view(h, b->va, &m);
		hw_unmap_planes(&m);
	}
	if (r) {
		hw_unmap_planes(&m);
		return r;
	}
	if (h->mode == MVC3D_HW_FP) {
		if (!(src->type & VIDTYPE_MVC))
			return -EINVAL;
		r = hw_map_planes(&m, src->canvas1_config);
		if (!r)
			hw_compose_view(h, b->va + (size_t)(h->h + h->gap) * h->stride, &m);
		hw_unmap_planes(&m);
	}
	codec_mm_dma_flush(b->va, h->stride * h->out_h, DMA_TO_DEVICE);
	return r;
}

static int hw_attach_vf(struct hw_dev *h, struct hw_buf *b, const struct vframe_s *src)
{
	struct vframe_s vf;

	memset(&vf, 0, sizeof(vf));
	INIT_LIST_HEAD(&vf.list);
	vf.type = VIDTYPE_PROGRESSIVE | VIDTYPE_VIU_FIELD | VIDTYPE_VIU_422 | VIDTYPE_VIU_SINGLE_PLANE;
	vf.type_backup = vf.type;
	vf.type_original = vf.type;
	vf.flag = VFRAME_FLAG_VIDEO_LINEAR;
	vf.bitdepth = BITDEPTH_Y8 | BITDEPTH_U8 | BITDEPTH_V8;
	vf.signal_type = src->signal_type ? src->signal_type : SIGNAL_BT709;
	vf.width = h->w;
	vf.height = h->out_h;
	vf.duration = src->duration;
	vf.ratio_control = src->ratio_control;
	vf.canvas0Addr = (u32)-1;
	vf.canvas1Addr = (u32)-1;
	vf.plane_num = 1;
	vf.canvas0_config[0].phy_addr = b->phys;
	vf.canvas0_config[0].width = h->stride;
	vf.canvas0_config[0].height = h->out_h;
	vf.canvas0_config[0].block_mode = CANVAS_BLKMODE_LINEAR;
	memcpy(vf.canvas1_config, vf.canvas0_config, sizeof(vf.canvas1_config));
	return dmabuf_set_vframe(b->db, &vf, VF_SRC_DECODER);
}

static int hw_free_buf_locked(struct hw_dev *h)
{
	int i;

	for (i = 0; i < h->nbufs; i++)
		if (h->bufs[i].state == HB_FREE)
			return i;
	return -1;
}

static void hw_kick(struct hw_dev *h)
{
	h->kick = true;
	wake_up_interruptible(&h->wq_thread);
}

static int hw_thread(void *data)
{
	struct hw_dev *h = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible_timeout(h->wq_thread, h->kick || kthread_should_stop(),
						 msecs_to_jiffies(100));
		h->kick = false;
		for (;;) {
			struct vframe_s *vf;
			unsigned long flags;
			struct hw_buf *b;
			ktime_t t0;
			u32 seq, gen, type;
			int bi, r;
			s64 us;

			mutex_lock(&h->plock);
			if (!h->prov_reg || !h->started || !vf_peek(HW_NAME)) {
				mutex_unlock(&h->plock);
				break;
			}
			spin_lock_irqsave(&h->lock, flags);
			bi = hw_free_buf_locked(h);
			if (bi >= 0)
				h->bufs[bi].state = HB_CLIENT;	/* ours while composing */
			spin_unlock_irqrestore(&h->lock, flags);
			if (bi < 0) {
				/* the decoder waits for the display */
				h->waits_nobuf++;
				mutex_unlock(&h->plock);
				break;
			}
			vf = vf_get(HW_NAME);
			if (!vf) {
				spin_lock_irqsave(&h->lock, flags);
				h->bufs[bi].state = HB_FREE;
				spin_unlock_irqrestore(&h->lock, flags);
				mutex_unlock(&h->plock);
				break;
			}
			b = &h->bufs[bi];
			h->frames_in++;
			seq = h->seq++;
			gen = h->gen;
			t0 = ktime_get();
			type = vf->type;
			r = hw_compose(h, b, vf);
			if (!r)
				r = hw_attach_vf(h, b, vf);
			vf_put(vf, HW_NAME);
			vf_notify_provider(HW_NAME, VFRAME_EVENT_RECEIVER_PUT, NULL);
			mutex_unlock(&h->plock);

			us = ktime_us_delta(ktime_get(), t0);
			spin_lock_irqsave(&h->lock, flags);
			if (r) {
				h->bad_frames++;
				b->state = HB_FREE;
			} else if (b->state == HB_CLIENT) {
				b->state = HB_READY;
				b->seq = seq;
				b->gen = gen;
				b->decoded_ns = ktime_get_ns();
				h->ready[(h->ready_head + h->ready_cnt) % MVC3D_HW_MAX_BUFS] = bi;
				h->ready_cnt++;
				h->composed++;
				h->us_sum += us;
				h->us_cnt++;
				if (us > h->us_max)
					h->us_max = us;
			}
			spin_unlock_irqrestore(&h->lock, flags);
			if (r)
				pr_warn_ratelimited(HW_NAME ": picture %u: %d (type 0x%x)\n", seq, r, type);
			wake_up_interruptible(&h->wq_client);
		}
	}
	return 0;
}

static int hw_recv_event(int type, void *data, void *op_arg)
{
	struct hw_dev *h = op_arg;

	switch (type) {
	case VFRAME_EVENT_PROVIDER_REG:
		mutex_lock(&h->plock);
		h->prov_reg = true;
		h->seq = 0;
		mutex_unlock(&h->plock);
		pr_info(HW_NAME ": decoder registered\n");
		hw_kick(h);
		break;
	case VFRAME_EVENT_PROVIDER_UNREG:
		/* waits for a picture in composition: the decoder frees its buffers after this */
		mutex_lock(&h->plock);
		h->prov_reg = false;
		mutex_unlock(&h->plock);
		pr_info(HW_NAME ": decoder unregistered after %u pictures\n", h->seq);
		wake_up_interruptible(&h->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_VFRAME_READY:
		hw_kick(h);
		break;
	case VFRAME_EVENT_PROVIDER_QUREY_STATE:
		return RECEIVER_ACTIVE;
	default:
		break;
	}
	return 0;
}

static const struct vframe_receiver_op_s hw_recv_ops = {
	.event_cb = hw_recv_event,
};

/* ------------------------------------------------------------------ */
/* /dev/mvc3dhw                                                       */
/* ------------------------------------------------------------------ */

static void hw_free_bufs(struct hw_dev *h)
{
	int i;

	for (i = 0; i < h->nbufs; i++) {
		struct hw_buf *b = &h->bufs[i];

		if (b->va)
			codec_mm_unmap_phyaddr(b->va);
		if (b->db)
			dma_buf_put(b->db);
		memset(b, 0, sizeof(*b));
	}
	h->nbufs = 0;
	h->ready_cnt = 0;
}

static void hw_stop(struct hw_dev *h)
{
	unsigned long flags;

	mutex_lock(&h->plock);
	h->started = false;
	mutex_unlock(&h->plock);
	spin_lock_irqsave(&h->lock, flags);
	h->ready_cnt = 0;
	spin_unlock_irqrestore(&h->lock, flags);
	hw_route(h, false);
	m3_hw_attach(false);
	hw_free_bufs(h);
}

static phys_addr_t hw_dmabuf_phys(struct hw_dev *h, struct dma_buf *db)
{
	struct uvm_buf_obj *obj = dmabuf_get_uvm_buf_obj(db);
	struct dma_buf_attachment *at;
	struct sg_table *sgt;
	phys_addr_t phys = 0;

	if (!IS_ERR_OR_NULL(obj)) {
		struct mua_buffer *mua = container_of(obj, struct mua_buffer, base);

		phys = mua->paddr;
		if (!phys && mua->sg_table)
			phys = sg_phys(mua->sg_table->sgl);
	}
	if (phys)
		return phys;
	at = dma_buf_attach(db, h->misc.this_device);
	if (IS_ERR(at))
		return 0;
	sgt = dma_buf_map_attachment(at, DMA_BIDIRECTIONAL);
	if (!IS_ERR(sgt)) {
		/* the uvm allocator gives contiguous memory: one entry */
		if (sgt->nents == 1)
			phys = sg_phys(sgt->sgl);
		dma_buf_unmap_attachment(at, sgt, DMA_BIDIRECTIONAL);
	}
	dma_buf_detach(db, at);
	return phys;
}

static long hw_ioc_start(struct hw_dev *h, void __user *p)
{
	struct mvc3d_hw_start s;
	int r;

	if (copy_from_user(&s, p, sizeof(s)))
		return -EFAULT;
	if (h->started)
		return -EBUSY;
	if (!s.width || s.width > HW_MAX_W || (s.width & 63) || !s.height || s.height > 1088 ||
	    s.mode > MVC3D_HW_FP)
		return -EINVAL;
	r = m3_hw_attach(true);
	if (r)
		return r;
	h->w = s.width;
	h->h = s.height;
	h->mode = s.mode;
	h->gap = s.height == 720 ? 30 : 45;
	h->out_h = s.mode == MVC3D_HW_FP ? 2 * s.height + h->gap : s.height;
	h->stride = s.width * 2;
	r = hw_route(h, true);
	if (r) {
		m3_hw_attach(false);
		return r;
	}
	/* both views out of the decoder; no dropping when the display is ahead of it */
	hw_sysfs_write("/sys/module/amvdec_h264mvc/parameters/view_mode", "3");
	hw_sysfs_write("/sys/module/amvdec_h264mvc/parameters/drop_thread_hold", "0");
	h->started = true;
	h->gen++;
	s.out_height = h->out_h;
	s.stride = h->stride;
	s.gen = h->gen;
	pr_info(HW_NAME ": start %ux%u %s, pictures %ux%u\n", h->w, h->h,
		h->mode == MVC3D_HW_FP ? "frame packed" : "2D", h->w, h->out_h);
	return copy_to_user(p, &s, sizeof(s)) ? -EFAULT : 0;
}

static long hw_ioc_add_buf(struct hw_dev *h, void __user *p)
{
	struct mvc3d_hw_buf a;
	struct hw_buf *b;
	struct dma_buf *db;
	unsigned long flags;
	size_t need;
	u32 i;

	if (copy_from_user(&a, p, sizeof(a)))
		return -EFAULT;
	if (!h->started)
		return -EINVAL;
	if (h->nbufs >= MVC3D_HW_MAX_BUFS)
		return -ENOSPC;
	db = dma_buf_get(a.fd);
	if (IS_ERR(db))
		return PTR_ERR(db);
	need = (size_t)h->stride * h->out_h;
	if (!dmabuf_is_uvm(db) || db->size < need) {
		dma_buf_put(db);
		return -EINVAL;
	}
	b = &h->bufs[h->nbufs];
	b->db = db;
	b->size = need;
	b->phys = hw_dmabuf_phys(h, db);
	b->va = b->phys ? codec_mm_vmap(b->phys, need) : NULL;
	if (!b->va) {
		dma_buf_put(db);
		memset(b, 0, sizeof(*b));
		return -ENOMEM;
	}
	/* black, the active space between the views stays so */
	for (i = 0; i < need; i += 4) {
		b->va[i] = 0x80;
		b->va[i + 1] = 0x10;
		b->va[i + 2] = 0x80;
		b->va[i + 3] = 0x10;
	}
	codec_mm_dma_flush(b->va, need, DMA_TO_DEVICE);
	spin_lock_irqsave(&h->lock, flags);
	b->state = HB_FREE;
	a.index = h->nbufs++;
	spin_unlock_irqrestore(&h->lock, flags);
	hw_kick(h);
	return copy_to_user(p, &a, sizeof(a)) ? -EFAULT : 0;
}

static long hw_ioc_wait(struct hw_dev *h, void __user *p)
{
	struct mvc3d_hw_pic w;
	unsigned long flags;
	long r;
	int bi = -1;

	if (copy_from_user(&w, p, sizeof(w)))
		return -EFAULT;
	r = wait_event_interruptible_timeout(h->wq_client, READ_ONCE(h->ready_cnt) || !h->started,
					     msecs_to_jiffies(w.timeout_ms));
	if (r < 0)
		return r;
	spin_lock_irqsave(&h->lock, flags);
	if (h->ready_cnt) {
		struct hw_buf *b;

		bi = h->ready[h->ready_head];
		h->ready_head = (h->ready_head + 1) % MVC3D_HW_MAX_BUFS;
		h->ready_cnt--;
		b = &h->bufs[bi];
		b->state = HB_CLIENT;
		w.index = bi;
		w.seq = b->seq;
		w.gen = b->gen;
		w.decoded_ns = b->decoded_ns;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	if (bi < 0)
		return -EAGAIN;
	return copy_to_user(p, &w, sizeof(w)) ? -EFAULT : 0;
}

static long hw_ioc_release(struct hw_dev *h, void __user *p)
{
	struct mvc3d_hw_buf a;
	unsigned long flags;
	long r = 0;

	if (copy_from_user(&a, p, sizeof(a)))
		return -EFAULT;
	spin_lock_irqsave(&h->lock, flags);
	if (a.index < 0 || a.index >= h->nbufs || h->bufs[a.index].state != HB_CLIENT)
		r = -EINVAL;
	else
		h->bufs[a.index].state = HB_FREE;
	spin_unlock_irqrestore(&h->lock, flags);
	hw_kick(h);
	return r;
}

static long hw_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct hw_dev *h = ghw;
	void __user *p = (void __user *)arg;
	unsigned long flags;
	long r;

	if (h->owner != f)
		return -EBUSY;
	switch (cmd) {
	case MVC3D_HW_START:
		mutex_lock(&h->mlock);
		r = hw_ioc_start(h, p);
		mutex_unlock(&h->mlock);
		return r;
	case MVC3D_HW_ADD_BUF:
		mutex_lock(&h->mlock);
		r = hw_ioc_add_buf(h, p);
		mutex_unlock(&h->mlock);
		return r;
	case MVC3D_HW_WAIT:
		return hw_ioc_wait(h, p);
	case MVC3D_HW_RELEASE:
		return hw_ioc_release(h, p);
	case MVC3D_HW_FLUSH:
		spin_lock_irqsave(&h->lock, flags);
		while (h->ready_cnt) {
			h->bufs[h->ready[h->ready_head]].state = HB_FREE;
			h->ready_head = (h->ready_head + 1) % MVC3D_HW_MAX_BUFS;
			h->ready_cnt--;
		}
		h->gen++;
		r = h->gen;
		spin_unlock_irqrestore(&h->lock, flags);
		hw_kick(h);
		return put_user((u32)r, (u32 __user *)p);
	case MVC3D_HW_SET_FP: {
		u32 on;

		if (copy_from_user(&on, p, sizeof(on)))
			return -EFAULT;
		if (on && !fp_probe_on)
			return -ENODEV;
		WRITE_ONCE(fp_client, !!on);
		pr_info(HW_NAME ": frame packing %s\n", on ? "wanted" : "not wanted");
		return 0;
	}
	}
	return -ENOTTY;
}

static int hw_open(struct inode *inode, struct file *f)
{
	struct hw_dev *h = ghw;
	int r = 0;

	mutex_lock(&h->mlock);
	if (h->owner)
		r = -EBUSY;
	else
		h->owner = f;
	mutex_unlock(&h->mlock);
	return r;
}

static int hw_release(struct inode *inode, struct file *f)
{
	struct hw_dev *h = ghw;

	mutex_lock(&h->mlock);
	if (h->owner == f) {
		if (h->started || h->nbufs)
			hw_stop(h);
		WRITE_ONCE(fp_client, false);
		h->owner = NULL;
	}
	mutex_unlock(&h->mlock);
	return 0;
}

static const struct file_operations hw_fops = {
	.owner = THIS_MODULE,
	.open = hw_open,
	.release = hw_release,
	.unlocked_ioctl = hw_ioctl,
	.compat_ioctl = hw_ioctl,
};

static int hw_stats_show(struct seq_file *m, void *v)
{
	struct hw_dev *h = ghw;
	unsigned long flags;
	char st[MVC3D_HW_MAX_BUFS + 1];
	int i;

	spin_lock_irqsave(&h->lock, flags);
	for (i = 0; i < h->nbufs; i++)
		st[i] = "-FRC"[h->bufs[i].state];
	st[i] = 0;
	seq_printf(m, "client %s, %s, decoder %s, route %s\n", h->owner ? "yes" : "no",
		   h->started ? (h->mode == MVC3D_HW_FP ? "frame packed" : "2D") : "stopped",
		   h->prov_reg ? "registered" : "none", h->routed ? "here" : "default");
	seq_printf(m, "picture %ux%u stride %u, buffers %d: %s (Free Ready Client), ready %d\n", h->w,
		   h->out_h, h->stride, h->nbufs, st, h->ready_cnt);
	seq_printf(m, "decoder pictures %llu (seq %u), composed %llu, bad %llu, waits for a buffer %llu\n",
		   h->frames_in, h->seq, h->composed, h->bad_frames, h->waits_nobuf);
	seq_printf(m, "compose avg %llu us max %llu us, default map \"%s\"\n",
		   h->us_cnt ? div64_u64(h->us_sum, h->us_cnt) : 0, h->us_max, h->default_chain);
	spin_unlock_irqrestore(&h->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hw_stats);

static int mvc3d_hw_init(struct dentry *dbg)
{
	struct hw_dev *h = kzalloc(sizeof(*h), GFP_KERNEL);
	int r;

	if (!h)
		return -ENOMEM;
	mutex_init(&h->mlock);
	mutex_init(&h->plock);
	spin_lock_init(&h->lock);
	init_waitqueue_head(&h->wq_client);
	init_waitqueue_head(&h->wq_thread);
	ghw = h;
	vf_receiver_init(&h->recv, HW_NAME, &hw_recv_ops, h);
	r = vf_reg_receiver(&h->recv);
	if (r)
		goto fail;
	h->thread = kthread_run(hw_thread, h, HW_NAME);
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
	/* dma_buf_map_attachment needs a DMA capable device */
	h->misc.this_device->dma_mask = &hw_dma_mask;
	h->misc.this_device->coherent_dma_mask = ~0ULL;
	debugfs_create_file("hw_stats", 0444, dbg, NULL, &hw_stats_fops);
	return 0;

fail_thread:
	kthread_stop(h->thread);
fail_recv:
	vf_unreg_receiver(&h->recv);
fail:
	ghw = NULL;
	kfree(h);
	return r;
}

static void mvc3d_hw_exit(void)
{
	struct hw_dev *h = ghw;

	if (!h)
		return;
	misc_deregister(&h->misc);
	mutex_lock(&h->mlock);
	if (h->started || h->nbufs)
		hw_stop(h);
	mutex_unlock(&h->mlock);
	kthread_stop(h->thread);
	vf_unreg_receiver(&h->recv);
	kfree(h);
	ghw = NULL;
}
