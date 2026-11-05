// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hwmvct - test receiver for the Amlogic h264 MVC decoder (amvdec_h264mvc, provider "decoder.h264mvc").
 *
 * Takes the decoded 3D vframes (both views, view_mode 2/3) and hands them raw to user space:
 * every read() of /dev/hwmvct returns one record = struct hwmvct_rec + the 6 planes (view 0 Y U V,
 * view 1 Y U V) exactly as the decoder wrote them (canvas size, 32x32 block mode). The frame is given
 * back to the decoder after the copy, so the reader paces the decoder (nothing is shown).
 *
 * vfm map (as root, while testing): rm default; add hwmvct decoder hwmvct (see hwmvctest.sh).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/dma-direction.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/amlogic/media/codec_mm/codec_mm.h>

#include "hwmvct_uapi.h"

#define DRV_NAME "hwmvct"

void codec_mm_dma_flush(void *vaddr, int size, enum dma_data_direction dir);

static DEFINE_MUTEX(lock);		/* read() vs provider unreg */
static DECLARE_WAIT_QUEUE_HEAD(wq);
static bool prov_active;
static u32 frames, starts;
static struct vframe_receiver_s recv;

static int recv_event(int type, void *data, void *op_arg)
{
	switch (type) {
	case VFRAME_EVENT_PROVIDER_REG:
		mutex_lock(&lock);
		prov_active = true;
		frames = 0;
		starts++;
		mutex_unlock(&lock);
		pr_info(DRV_NAME ": provider %s registered\n", data ? (char *)data : "?");
		wake_up_interruptible(&wq);
		break;
	case VFRAME_EVENT_PROVIDER_UNREG:
		/* waits for a read() in progress: the decoder frees its buffers after this */
		mutex_lock(&lock);
		prov_active = false;
		mutex_unlock(&lock);
		pr_info(DRV_NAME ": provider unregistered after %u frames\n", frames);
		wake_up_interruptible(&wq);
		break;
	case VFRAME_EVENT_PROVIDER_VFRAME_READY:
		wake_up_interruptible(&wq);
		break;
	case VFRAME_EVENT_PROVIDER_QUREY_STATE:
		return RECEIVER_ACTIVE;
	default:
		break;
	}
	return 0;
}

static const struct vframe_receiver_op_s recv_ops = {
	.event_cb = recv_event,
};

static int copy_plane(char __user **ubuf, const struct canvas_config_s *c, struct hwmvct_plane *p)
{
	u32 size = c->width * c->height;
	u8 *v;
	int r = 0;

	p->phys = c->phy_addr;
	p->width = c->width;
	p->height = c->height;
	p->block_mode = c->block_mode;
	p->size = size;
	if (!c->phy_addr || !size || size > HWMVCT_MAX_PLANE)
		return -EINVAL;
	v = codec_mm_vmap(c->phy_addr, size);
	if (!v)
		return -ENOMEM;
	codec_mm_dma_flush(v, size, DMA_FROM_DEVICE);
	if (copy_to_user(*ubuf, v, size)) {
		pr_err(DRV_NAME ": copy of %u bytes from %llx (va %px) to %px failed\n", size,
		       (u64)c->phy_addr, v, *ubuf);
		r = -EFAULT;
	}
	codec_mm_unmap_phyaddr(v);
	*ubuf += size;
	return r;
}

static ssize_t hwmvct_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct hwmvct_rec rec;
	struct vframe_s *vf;
	char __user *data = buf + sizeof(rec);
	ssize_t r;
	int i;

	if (count < HWMVCT_MAX_REC)
		return -EINVAL;
	/* up to 3 s for the next picture, 0 bytes = none (end of stream or decoder gone) */
	r = wait_event_interruptible_timeout(wq, !prov_active || vf_peek(DRV_NAME), 3 * HZ);
	if (r < 0)
		return r;
	mutex_lock(&lock);
	if (!prov_active || !(vf = vf_get(DRV_NAME))) {
		mutex_unlock(&lock);
		return 0;
	}
	memset(&rec, 0, sizeof(rec));
	rec.magic = HWMVCT_MAGIC;
	rec.index = frames++;
	rec.starts = starts;
	rec.pts = vf->pts;
	rec.pts_us64 = vf->pts_us64;
	rec.type = vf->type;
	rec.width = vf->width;
	rec.height = vf->height;
	rec.dirty = vf->frame_dirty;
	rec.canvas0 = vf->canvas0Addr;
	rec.canvas1 = vf->canvas1Addr;
	rec.plane_num = vf->plane_num;
	r = 0;
	for (i = 0; i < 3 && !r; i++)
		r = copy_plane(&data, &vf->canvas0_config[i], &rec.plane[i]);
	for (i = 0; i < 3 && !r && (vf->type & VIDTYPE_MVC); i++)
		r = copy_plane(&data, &vf->canvas1_config[i], &rec.plane[3 + i]);
	vf_put(vf, DRV_NAME);
	vf_notify_provider(DRV_NAME, VFRAME_EVENT_RECEIVER_PUT, NULL);
	mutex_unlock(&lock);
	if (r)
		rec.error = -r;
	if (copy_to_user(buf, &rec, sizeof(rec)))
		return -EFAULT;
	return data - buf;
}

static const struct file_operations hwmvct_fops = {
	.owner = THIS_MODULE,
	.read = hwmvct_read,
	.llseek = noop_llseek,
};

static struct miscdevice hwmvct_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRV_NAME,
	.fops = &hwmvct_fops,
	.mode = 0666,
};

static int __init hwmvct_init(void)
{
	int r = misc_register(&hwmvct_misc);

	if (r)
		return r;
	vf_receiver_init(&recv, DRV_NAME, &recv_ops, NULL);
	r = vf_reg_receiver(&recv);
	if (r) {
		misc_deregister(&hwmvct_misc);
		return r;
	}
	pr_info(DRV_NAME ": ready\n");
	return 0;
}

static void __exit hwmvct_exit(void)
{
	vf_unreg_receiver(&recv);
	misc_deregister(&hwmvct_misc);
}

module_init(hwmvct_init);
module_exit(hwmvct_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("test receiver for the Amlogic h264 MVC decoder");
