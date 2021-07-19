// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 Fuzhou Rockchip Electronics Co., Ltd. */

#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>
#include <linux/dma-iommu.h>
#include <linux/rk-camera-module.h>
#include "dev.h"
#include "regs.h"

static inline
struct rkisp_bridge_buf *to_bridge_buf(struct rkisp_ispp_buf *dbufs)
{
	return container_of(dbufs, struct rkisp_bridge_buf, dbufs);
}

static void update_mi(struct rkisp_bridge_device *dev)
{
	struct rkisp_hw_dev *hw = dev->ispdev->hw_dev;
	struct rkisp_bridge_buf *buf;
	u32 val;

	if (hw->nxt_buf) {
		buf = to_bridge_buf(hw->nxt_buf);
		val = buf->dummy[GROUP_BUF_PIC].dma_addr;
		rkisp_write(dev->ispdev,
			    dev->cfg->reg.y0_base, val, true);
		val += dev->cfg->offset;
		rkisp_write(dev->ispdev,
			    dev->cfg->reg.uv0_base, val, true);
		val = buf->dummy[GROUP_BUF_GAIN].dma_addr;
		rkisp_write(dev->ispdev,
			    dev->cfg->reg.g0_base, val, true);
	}

	v4l2_dbg(3, rkisp_debug, &dev->sd,
		 "%s pic(shd:0x%x base:0x%x) gain(shd:0x%x base:0x%x)\n",
		 __func__,
		 rkisp_read(dev->ispdev, dev->cfg->reg.y0_base_shd, true),
		 rkisp_read(dev->ispdev, dev->cfg->reg.y0_base, true),
		 rkisp_read(dev->ispdev, dev->cfg->reg.g0_base_shd, true),
		 rkisp_read(dev->ispdev, dev->cfg->reg.g0_base, true));
}

static int frame_end(struct rkisp_bridge_device *dev, bool en)
{
	struct rkisp_hw_dev *hw = dev->ispdev->hw_dev;
	struct v4l2_subdev *sd = v4l2_get_subdev_hostdata(&dev->sd);
	unsigned long lock_flags = 0;
	u64 ns = 0;

	if (hw->cur_buf && hw->nxt_buf) {
		if (!en) {
			spin_lock_irqsave(&hw->buf_lock, lock_flags);
			list_add_tail(&hw->cur_buf->list, &hw->list);
			spin_unlock_irqrestore(&hw->buf_lock, lock_flags);
		} else {
			rkisp_dmarx_get_frame(dev->ispdev,
				&hw->cur_buf->frame_id, &ns, true);
			hw->cur_buf->frame_id++;
			if (!ns)
				ns = ktime_get_ns();
			hw->cur_buf->frame_timestamp = ns;
			hw->cur_buf->index = dev->ispdev->dev_id;
			v4l2_subdev_call(sd, video, s_rx_buffer, hw->cur_buf, NULL);
		}
		hw->cur_buf = NULL;
	}

	if (hw->nxt_buf) {
		hw->cur_buf = hw->nxt_buf;
		hw->nxt_buf = NULL;
	}

	spin_lock_irqsave(&hw->buf_lock, lock_flags);
	if (!list_empty(&hw->list)) {
		hw->nxt_buf = list_first_entry(&hw->list,
				struct rkisp_ispp_buf, list);
		list_del(&hw->nxt_buf->list);
	}
	spin_unlock_irqrestore(&hw->buf_lock, lock_flags);

	update_mi(dev);

	return 0;
}

static int config_gain(struct rkisp_bridge_device *dev)
{
	u32 w = dev->crop.width;
	u32 h = dev->crop.height;
	u32 val;

	val = ALIGN(w, 64) * ALIGN(h, 128) >> 4;
	rkisp_write(dev->ispdev, MI_GAIN_WR_SIZE, val, false);
	val = ALIGN((w + 3) >> 2, 16);
	rkisp_write(dev->ispdev, MI_GAIN_WR_LENGTH, val, false);
	rkisp_set_bits(dev->ispdev, MI_WR_CTRL2,
		       0, SW_GAIN_WR_AUTOUPD, true);
	return 0;
}

static int config_mpfbc(struct rkisp_bridge_device *dev)
{
	struct rkisp_hw_dev *hw = dev->ispdev->hw_dev;
	u32 h = hw->max_in.h ? hw->max_in.h : dev->crop.height;
	u32 ctrl = 0;

	if (dev->work_mode & ISP_ISPP_QUICK) {
		rkisp_set_bits(dev->ispdev, CTRL_SWS_CFG,
			       0, SW_ISP2PP_PIPE_EN, true);
		ctrl = SW_MPFBC_MAINISP_MODE;
		if (dev->ispdev->hw_dev->nxt_buf)
			ctrl |= SW_MPFBC_PINGPONG_EN;
	}

	rkisp_write(dev->ispdev, ISP_MPFBC_VIR_WIDTH, 0, true);
	rkisp_write(dev->ispdev, ISP_MPFBC_VIR_HEIGHT, ALIGN(h, 16), true);

	ctrl |= (dev->work_mode & ISP_ISPP_422) | SW_MPFBC_EN;
	rkisp_write(dev->ispdev, ISP_MPFBC_BASE, ctrl, true);
	return 0;
}

static void disable_mpfbc(struct rkisp_bridge_device *dev)
{
	if (dev->ispdev->hw_dev->is_single)
		rkisp_clear_bits(dev->ispdev, ISP_MPFBC_BASE,
				 SW_MPFBC_EN, true);
}

static bool is_stopped_mpfbc(struct rkisp_bridge_device *dev)
{
	bool en = true;

	if (dev->ispdev->hw_dev->is_single)
		en = is_mpfbc_stopped(dev->ispdev->base_addr);
	return en;
}

static struct rkisp_bridge_ops mpfbc_ops = {
	.config = config_mpfbc,
	.disable = disable_mpfbc,
	.is_stopped = is_stopped_mpfbc,
};

static struct rkisp_bridge_config mpfbc_cfg = {
	.frame_end_id = MI_MPFBC_FRAME,
	.reg = {
		.y0_base = ISP_MPFBC_HEAD_PTR,
		.uv0_base = ISP_MPFBC_PAYL_PTR,
		.y1_base = ISP_MPFBC_HEAD_PTR2,
		.uv1_base = ISP_MPFBC_PAYL_PTR2,
		.g0_base = MI_GAIN_WR_BASE,
		.g1_base = MI_GAIN_WR_BASE2,

		.y0_base_shd = ISP_MPFBC_HEAD_PTR,
		.uv0_base_shd = ISP_MPFBC_PAYL_PTR,
		.g0_base_shd = MI_GAIN_WR_BASE_SHD,
	},
};

static int config_mp(struct rkisp_bridge_device *dev)
{
	u32 w = dev->crop.width;
	u32 h = dev->crop.height;
	u32 val;

	if (dev->work_mode & ISP_ISPP_QUICK) {
		rkisp_set_bits(dev->ispdev, CTRL_SWS_CFG, 0,
			       SW_ISP2PP_PIPE_EN, true);
		if (dev->ispdev->hw_dev->nxt_buf)
			rkisp_set_bits(dev->ispdev, CIF_MI_CTRL, 0,
				       CIF_MI_MP_PINGPONG_ENABLE, true);
	}

	val = w * h;
	rkisp_write(dev->ispdev, CIF_MI_MP_Y_SIZE_INIT, val, false);
	val = (dev->work_mode & ISP_ISPP_422) ? val : val / 2;
	rkisp_write(dev->ispdev, CIF_MI_MP_CB_SIZE_INIT, val, false);
	rkisp_write(dev->ispdev, CIF_MI_MP_CR_SIZE_INIT, 0, false);
	rkisp_write(dev->ispdev, CIF_MI_MP_Y_OFFS_CNT_INIT, 0, false);
	rkisp_write(dev->ispdev, CIF_MI_MP_CB_OFFS_CNT_INIT, 0, false);
	rkisp_write(dev->ispdev, CIF_MI_MP_CR_OFFS_CNT_INIT, 0, false);

	rkisp_write(dev->ispdev, ISP_MPFBC_BASE,
		    dev->work_mode & ISP_ISPP_422, true);
	rkisp_set_bits(dev->ispdev, CIF_MI_CTRL, MI_CTRL_MP_FMT_MASK,
		       MI_CTRL_MP_WRITE_YUV_SPLA | CIF_MI_CTRL_MP_ENABLE |
		       CIF_MI_MP_AUTOUPDATE_ENABLE, true);
	return 0;
}

static void disable_mp(struct rkisp_bridge_device *dev)
{
	if (dev->ispdev->hw_dev->is_single)
		rkisp_clear_bits(dev->ispdev, CIF_MI_CTRL,
				 CIF_MI_CTRL_MP_ENABLE |
				 CIF_MI_CTRL_RAW_ENABLE, true);
}

static bool is_stopped_mp(struct rkisp_bridge_device *dev)
{
	bool en = true;

	if (dev->ispdev->hw_dev->is_single)
		en = mp_is_stream_stopped(dev->ispdev->base_addr);
	return en;
}

static struct rkisp_bridge_ops mp_ops = {
	.config = config_mp,
	.disable = disable_mp,
	.is_stopped = is_stopped_mp,
};

static struct rkisp_bridge_config mp_cfg = {
	.frame_end_id = MI_MP_FRAME,
	.reg = {
		.y0_base = MI_MP_WR_Y_BASE,
		.uv0_base = MI_MP_WR_CB_BASE,
		.y1_base = MI_MP_WR_Y_BASE2,
		.uv1_base = MI_MP_WR_CB_BASE2,
		.g0_base = MI_GAIN_WR_BASE,
		.g1_base = MI_GAIN_WR_BASE2,

		.y0_base_shd = MI_MP_WR_Y_BASE_SHD,
		.uv0_base_shd = MI_MP_WR_CB_BASE_SHD,
		.g0_base_shd = MI_GAIN_WR_BASE_SHD,
	},
};

static void free_bridge_buf(struct rkisp_bridge_device *dev)
{
	struct rkisp_hw_dev *hw = dev->ispdev->hw_dev;
	struct rkisp_bridge_buf *buf;
	struct rkisp_ispp_buf *dbufs;
	unsigned long lock_flags = 0;
	int i, j;

	if (atomic_dec_return(&hw->refcnt))
		return;

	v4l2_dbg(1, rkisp_debug, &dev->ispdev->v4l2_dev,
		 "%s\n", __func__);

	spin_lock_irqsave(&hw->buf_lock, lock_flags);
	if (hw->cur_buf) {
		list_add_tail(&hw->cur_buf->list, &hw->list);
		if (hw->cur_buf == hw->nxt_buf)
			hw->nxt_buf = NULL;
		hw->cur_buf = NULL;
	}

	if (hw->nxt_buf) {
		list_add_tail(&hw->nxt_buf->list, &hw->list);
		hw->nxt_buf = NULL;
	}

	while (!list_empty(&hw->list)) {
		dbufs = list_first_entry(&hw->list,
				struct rkisp_ispp_buf, list);
		list_del(&dbufs->list);
	}
	spin_unlock_irqrestore(&hw->buf_lock, lock_flags);
	for (i = 0; i < BRIDGE_BUF_MAX; i++) {
		buf = &hw->bufs[i];
		for (j = 0; j < GROUP_BUF_MAX; j++)
			rkisp_free_buffer(dev->ispdev, &buf->dummy[j]);
	}
}

static int init_buf(struct rkisp_bridge_device *dev, u32 pic_size, u32 gain_size)
{
	struct v4l2_subdev *sd = v4l2_get_subdev_hostdata(&dev->sd);
	struct rkisp_hw_dev *hw = dev->ispdev->hw_dev;
	struct rkisp_bridge_buf *buf;
	struct rkisp_dummy_buffer *dummy;
	int i, j, val, ret = 0;

	if (atomic_inc_return(&hw->refcnt) > 1)
		return 0;

	v4l2_dbg(1, rkisp_debug, &dev->ispdev->v4l2_dev,
		 "%s pic size:%d gain size:%d\n",
		 __func__, pic_size, gain_size);
	for (i = 0; i < dev->buf_num; i++) {
		buf = &hw->bufs[i];
		for (j = 0; j < GROUP_BUF_MAX; j++) {
			dummy = &buf->dummy[j];
			dummy->is_need_dbuf = true;
			dummy->size = !j ? pic_size : gain_size;
			ret = rkisp_alloc_buffer(dev->ispdev, dummy);
			if (ret)
				goto err;
			buf->dbufs.dbuf[j] = dummy->dbuf;
		}
		list_add_tail(&buf->dbufs.list, &hw->list);
		ret = v4l2_subdev_call(sd, video, s_rx_buffer, &buf->dbufs, NULL);
		if (ret)
			goto err;
	}

	hw->cur_buf = list_first_entry(&hw->list, struct rkisp_ispp_buf, list);
	list_del(&hw->cur_buf->list);
	buf = to_bridge_buf(hw->cur_buf);
	val = buf->dummy[GROUP_BUF_PIC].dma_addr;
	rkisp_write(dev->ispdev, dev->cfg->reg.y0_base, val, true);
	val += dev->cfg->offset;
	rkisp_write(dev->ispdev, dev->cfg->reg.uv0_base, val, true);
	val = buf->dummy[GROUP_BUF_GAIN].dma_addr;
	rkisp_write(dev->ispdev, dev->cfg->reg.g0_base, val, true);

	if (!list_empty(&hw->list)) {
		hw->nxt_buf = list_first_entry(&hw->list,
				struct rkisp_ispp_buf, list);
		list_del(&hw->nxt_buf->list);
	}
	if (hw->nxt_buf && (dev->work_mode & ISP_ISPP_QUICK)) {
		buf = to_bridge_buf(hw->nxt_buf);
		val = buf->dummy[GROUP_BUF_PIC].dma_addr;
		rkisp_write(dev->ispdev, dev->cfg->reg.y1_base, val, true);
		val += dev->cfg->offset;
		rkisp_write(dev->ispdev, dev->cfg->reg.uv1_base, val, true);
		val = buf->dummy[GROUP_BUF_GAIN].dma_addr;
		rkisp_write(dev->ispdev, dev->cfg->reg.g1_base, val, true);
		rkisp_set_bits(dev->ispdev, MI_WR_CTRL2,
			       0, SW_GAIN_WR_PINGPONG, true);
	}

	rkisp_set_bits(dev->ispdev, CIF_VI_DPCL, 0,
		       CIF_VI_DPCL_CHAN_MODE_MP |
		       CIF_VI_DPCL_MP_MUX_MRSZ_MI, true);
	rkisp_set_bits(dev->ispdev, MI_WR_CTRL, 0,
		       CIF_MI_CTRL_INIT_BASE_EN |
		       CIF_MI_CTRL_INIT_OFFSET_EN, true);
	rkisp_set_bits(dev->ispdev, MI_IMSC, 0,
		       dev->cfg->frame_end_id, true);
	return 0;
err:
	free_bridge_buf(dev);
	v4l2_err(&dev->sd, "%s fail:%d\n", __func__, ret);
	return ret;
}

static int config_mode(struct rkisp_bridge_device *dev)
{
	struct rkisp_hw_dev *hw = dev->ispdev->hw_dev;
	u32 w = hw->max_in.w ? hw->max_in.w : dev->crop.width;
	u32 h = hw->max_in.h ? hw->max_in.h : dev->crop.height;
	u32 offs = w * h;
	u32 pic_size = 0, gain_size;

	if (dev->work_mode == ISP_ISPP_INIT_FAIL) {
		free_bridge_buf(dev);
		return 0;
	}

	if (!dev->linked || !dev->ispdev->isp_inp) {
		v4l2_err(&dev->sd,
			 "invalid: link:%d or isp input:0x%x\n",
			 dev->linked,
			 dev->ispdev->isp_inp);
		return -EINVAL;
	}

	v4l2_dbg(1, rkisp_debug, &dev->sd,
		 "work mode:0x%x buf num:%d\n",
		 dev->work_mode, dev->buf_num);

	if (dev->work_mode & ISP_ISPP_FBC) {
		dev->ops = &mpfbc_ops;
		dev->cfg = &mpfbc_cfg;
	} else {
		dev->ops = &mp_ops;
		dev->cfg = &mp_cfg;
	}

	gain_size = ALIGN(w, 64) * ALIGN(h, 128) >> 4;
	if (dev->work_mode & ISP_ISPP_FBC) {
		w = ALIGN(w, 16);
		h = ALIGN(h, 16);
		offs = w * h >> 4;
		pic_size = offs;
	}
	if (dev->work_mode & ISP_ISPP_422)
		pic_size += w * h * 2;
	else
		pic_size += w * h * 3 >> 1;
	dev->cfg->offset = offs;
	return init_buf(dev, pic_size, gain_size);
}

static void crop_on(struct rkisp_bridge_device *dev)
{
	struct rkisp_device *ispdev = dev->ispdev;
	u32 src_w = ispdev->isp_sdev.out_crop.width;
	u32 src_h = ispdev->isp_sdev.out_crop.height;
	u32 dest_w = dev->crop.width;
	u32 dest_h = dev->crop.height;
	u32 left = dev->crop.left;
	u32 top = dev->crop.top;
	u32 ctrl;

	if (src_w == dest_w && src_h == dest_h)
		return;

	rkisp_write(ispdev, CIF_DUAL_CROP_M_H_OFFS, left, true);
	rkisp_write(ispdev, CIF_DUAL_CROP_M_V_OFFS, top, true);
	rkisp_write(ispdev, CIF_DUAL_CROP_M_H_SIZE, dest_w, true);
	rkisp_write(ispdev, CIF_DUAL_CROP_M_V_SIZE, dest_h, true);
	ctrl = rkisp_read(ispdev, CIF_DUAL_CROP_CTRL, true);
	ctrl |= CIF_DUAL_CROP_MP_MODE_YUV | CIF_DUAL_CROP_CFG_UPD;
	rkisp_write(ispdev, CIF_DUAL_CROP_CTRL, ctrl, true);
}

static void crop_off(struct rkisp_bridge_device *dev)
{
	struct rkisp_device *ispdev = dev->ispdev;
	u32 src_w = ispdev->isp_sdev.out_crop.width;
	u32 src_h = ispdev->isp_sdev.out_crop.height;
	u32 dest_w = dev->crop.width;
	u32 dest_h = dev->crop.height;
	u32 ctrl;

	if (src_w == dest_w && src_h == dest_h)
		return;

	ctrl = rkisp_read(ispdev, CIF_DUAL_CROP_CTRL, true);
	ctrl &= ~(CIF_DUAL_CROP_MP_MODE_YUV |
		  CIF_DUAL_CROP_MP_MODE_RAW);
	ctrl |= CIF_DUAL_CROP_GEN_CFG_UPD;
	rkisp_write(ispdev, CIF_DUAL_CROP_CTRL, ctrl, true);
}

static int bridge_start(struct rkisp_bridge_device *dev)
{
	crop_on(dev);
	config_gain(dev);
	dev->ops->config(dev);

	if (!dev->ispdev->hw_dev->is_mi_update) {
		force_cfg_update(dev->ispdev);

		if (!(dev->work_mode & ISP_ISPP_QUICK))
			update_mi(dev);
	}
	dev->en = true;
	return 0;
}

static int bridge_stop(struct rkisp_bridge_device *dev)
{
	int ret;

	dev->stopping = true;
	dev->ops->disable(dev);
	hdr_stop_dmatx(dev->ispdev);
	ret = wait_event_timeout(dev->done, !dev->en,
				 msecs_to_jiffies(1000));
	if (!ret)
		v4l2_warn(&dev->sd,
			  "%s timeout ret:%d\n", __func__, ret);
	crop_off(dev);
	dev->stopping = false;
	dev->en = false;

	/* make sure ispp last frame done */
	if (dev->work_mode & ISP_ISPP_QUICK) {
		rkisp_clear_bits(dev->ispdev, MI_IMSC,
				 dev->cfg->frame_end_id, true);
		usleep_range(20000, 25000);
	}
	return 0;
}

static int bridge_start_stream(struct v4l2_subdev *sd)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);
	int ret;

	if (WARN_ON(dev->en))
		return -EBUSY;

	if (dev->ispdev->isp_inp & INP_CSI ||
	    dev->ispdev->isp_inp & INP_DVP ||
	    dev->ispdev->isp_inp & INP_LVDS) {
		/* Always update sensor info in case media topology changed */
		ret = rkisp_update_sensor_info(dev->ispdev);
		if (ret < 0) {
			v4l2_err(sd, "update sensor info failed %d\n", ret);
			goto free_buf;
		}
	}

	/* enable clocks/power-domains */
	ret = dev->ispdev->pipe.open(&dev->ispdev->pipe, &sd->entity, true);
	if (ret < 0)
		goto free_buf;

	hdr_config_dmatx(dev->ispdev);
	ret = bridge_start(dev);
	if (ret)
		goto close_pipe;
	hdr_update_dmatx_buf(dev->ispdev);

	/* start sub-devices */
	ret = dev->ispdev->pipe.set_stream(&dev->ispdev->pipe, true);
	if (ret < 0)
		goto stop_bridge;

	ret = media_pipeline_start(&sd->entity, &dev->ispdev->pipe.pipe);
	if (ret < 0)
		goto pipe_stream_off;

	return 0;
pipe_stream_off:
	dev->ispdev->pipe.set_stream(&dev->ispdev->pipe, false);
stop_bridge:
	bridge_stop(dev);
close_pipe:
	dev->ispdev->pipe.close(&dev->ispdev->pipe);
	hdr_destroy_buf(dev->ispdev);
free_buf:
	free_bridge_buf(dev);
	v4l2_err(&dev->sd, "%s fail:%d\n", __func__, ret);
	return ret;
}

static void bridge_destroy_buf(struct rkisp_bridge_device *dev)
{
	free_bridge_buf(dev);
	hdr_destroy_buf(dev->ispdev);
}

static int bridge_stop_stream(struct v4l2_subdev *sd)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);

	bridge_stop(dev);
	media_pipeline_stop(&sd->entity);
	dev->ispdev->pipe.set_stream(&dev->ispdev->pipe, false);
	dev->ispdev->pipe.close(&dev->ispdev->pipe);
	bridge_destroy_buf(dev);
	return 0;
}

static int bridge_get_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_format *fmt)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);

	if (!fmt)
		return -EINVAL;

	/* get isp out format */
	fmt->pad = RKISP_ISP_PAD_SOURCE_PATH;
	fmt->which = V4L2_SUBDEV_FORMAT_ACTIVE;
	return v4l2_subdev_call(&dev->ispdev->isp_sdev.sd,
				pad, get_fmt, NULL, fmt);
}

static int bridge_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);
	struct rkisp_isp_subdev *isp_sd = &dev->ispdev->isp_sdev;
	u32 src_w = isp_sd->out_crop.width;
	u32 src_h = isp_sd->out_crop.height;
	struct v4l2_rect *crop;

	if (!sel)
		return -EINVAL;
	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	crop = &sel->r;
	crop->left = clamp_t(u32, crop->left, 0, src_w);
	crop->top = clamp_t(u32, crop->top, 0, src_h);
	crop->width = clamp_t(u32, crop->width,
		CIF_ISP_OUTPUT_W_MIN, src_w - crop->left);
	crop->height = clamp_t(u32, crop->height,
		CIF_ISP_OUTPUT_H_MIN, src_h - crop->top);

	dev->crop = *crop;
	return 0;
}

static int bridge_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);
	struct rkisp_isp_subdev *isp_sd = &dev->ispdev->isp_sdev;
	struct v4l2_rect *crop;

	if (!sel)
		return -EINVAL;

	crop = &sel->r;
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
		*crop = isp_sd->out_crop;
		break;
	case V4L2_SEL_TGT_CROP:
		*crop = dev->crop;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int bridge_s_rx_buffer(struct v4l2_subdev *sd,
			      void *buf, unsigned int *size)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);
	struct rkisp_hw_dev *hw = dev->ispdev->hw_dev;
	struct rkisp_ispp_buf *dbufs = buf;
	unsigned long lock_flags = 0;

	/* size isn't using now */
	if (!dbufs || !atomic_read(&hw->refcnt))
		return -EINVAL;

	spin_lock_irqsave(&hw->buf_lock, lock_flags);
	list_add_tail(&dbufs->list, &hw->list);
	spin_unlock_irqrestore(&hw->buf_lock, lock_flags);
	return 0;
}

static int bridge_s_stream(struct v4l2_subdev *sd, int on)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);
	int ret = 0;

	v4l2_dbg(1, rkisp_debug, sd,
		 "%s %d\n", __func__, on);

	if (on) {
		atomic_inc(&dev->ispdev->cap_dev.refcnt);
		ret = bridge_start_stream(sd);
	} else {
		if (dev->en)
			ret = bridge_stop_stream(sd);
		atomic_dec(&dev->ispdev->cap_dev.refcnt);
	}

	return ret;
}

static int bridge_s_power(struct v4l2_subdev *sd, int on)
{
	int ret = 0;

	v4l2_dbg(1, rkisp_debug, sd,
		 "%s %d\n", __func__, on);

	if (on)
		ret = v4l2_pipeline_pm_use(&sd->entity, 1);
	else
		ret = v4l2_pipeline_pm_use(&sd->entity, 0);

	return ret;
}

static long bridge_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct rkisp_bridge_device *dev = v4l2_get_subdevdata(sd);
	struct rkisp_ispp_mode *mode;
	long ret = 0;

	switch (cmd) {
	case RKISP_ISPP_CMD_SET_MODE:
		mode = arg;
		dev->ispdev->hw_dev->max_in = mode->max_in;
		dev->work_mode = mode->work_mode;
		dev->buf_num = mode->buf_num;
		ret = config_mode(dev);
		rkisp_chk_tb_over(dev->ispdev);
		break;
	default:
		ret = -ENOIOCTLCMD;
	}

	return ret;
}

static const struct v4l2_subdev_pad_ops bridge_pad_ops = {
	.set_fmt = bridge_get_set_fmt,
	.get_fmt = bridge_get_set_fmt,
	.get_selection = bridge_get_selection,
	.set_selection = bridge_set_selection,
};

static const struct v4l2_subdev_video_ops bridge_video_ops = {
	.s_rx_buffer = bridge_s_rx_buffer,
	.s_stream = bridge_s_stream,
};

static const struct v4l2_subdev_core_ops bridge_core_ops = {
	.s_power = bridge_s_power,
	.ioctl = bridge_ioctl,
};

static struct v4l2_subdev_ops bridge_v4l2_ops = {
	.core = &bridge_core_ops,
	.video = &bridge_video_ops,
	.pad = &bridge_pad_ops,
};

void rkisp_bridge_isr(u32 *mis_val, struct rkisp_device *dev)
{
	struct rkisp_bridge_device *bridge = &dev->br_dev;
	void __iomem *base = dev->base_addr;
	u32 val = 0;

	/* dmarx isr is unreliable, MI frame end to replace it */
	if (*mis_val & (MI_MP_FRAME | MI_MPFBC_FRAME) &&
	    IS_HDR_RDBK(dev->hdr.op_mode)) {
		switch (dev->hdr.op_mode) {
		case HDR_RDBK_FRAME3://for rd1 rd0 rd2
			val |= RAW1_RD_FRAME;
			/* FALLTHROUGH */
		case HDR_RDBK_FRAME2://for rd0 rd2
			val |= RAW0_RD_FRAME;
			/* FALLTHROUGH */
		default:// for rd2
			val |= RAW2_RD_FRAME;
			/* FALLTHROUGH */
		}
		rkisp2_rawrd_isr(val, dev);
	}

	if (!bridge->cfg ||
	    (bridge->cfg &&
	     !(*mis_val & bridge->cfg->frame_end_id)))
		return;

	*mis_val &= ~bridge->cfg->frame_end_id;
	writel(bridge->cfg->frame_end_id, base + CIF_MI_ICR);

	if (bridge->stopping) {
		if (bridge->ops->is_stopped(bridge)) {
			bridge->en = false;
			bridge->stopping = false;
			wake_up(&bridge->done);
		}
	}

	if (!(bridge->work_mode & ISP_ISPP_QUICK)) {
		frame_end(bridge, bridge->en);
		if (!bridge->en)
			dev->isp_state = ISP_STOP;
	}
	if (dev->dmarx_dev.trigger == T_MANUAL)
		rkisp_csi_trigger_event(dev, T_CMD_END, NULL);
}

int rkisp_register_bridge_subdev(struct rkisp_device *dev,
				 struct v4l2_device *v4l2_dev)
{
	struct rkisp_bridge_device *bridge = &dev->br_dev;
	struct v4l2_subdev *sd;
	struct media_entity *source, *sink;
	int ret;

	memset(bridge, 0, sizeof(*bridge));
	if (dev->isp_ver != ISP_V20)
		return 0;

	bridge->ispdev = dev;
	sd = &bridge->sd;
	v4l2_subdev_init(sd, &bridge_v4l2_ops);
	//sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.obj_type = 0;
	snprintf(sd->name, sizeof(sd->name), BRIDGE_DEV_NAME);
	bridge->pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&sd->entity, 1, &bridge->pad);
	if (ret < 0)
		return ret;
	sd->owner = THIS_MODULE;
	v4l2_set_subdevdata(sd, bridge);
	sd->grp_id = GRP_ID_ISP_BRIDGE;
	ret = v4l2_device_register_subdev(v4l2_dev, sd);
	if (ret < 0) {
		v4l2_err(sd, "Failed to register subdev\n");
		goto free_media;
	}
	bridge->crop = dev->isp_sdev.out_crop;
	/* bridge links */
	bridge->linked = true;
	source = &dev->isp_sdev.sd.entity;
	sink = &sd->entity;
	ret = media_create_pad_link(source, RKISP_ISP_PAD_SOURCE_PATH,
				    sink, 0, bridge->linked);
	init_waitqueue_head(&bridge->done);
	return ret;

free_media:
	media_entity_cleanup(&sd->entity);
	return ret;
}

void rkisp_unregister_bridge_subdev(struct rkisp_device *dev)
{
	struct v4l2_subdev *sd = &dev->br_dev.sd;

	if (dev->isp_ver != ISP_V20)
		return;
	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
}

void rkisp_get_bridge_sd(struct platform_device *dev,
			 struct v4l2_subdev **sd)
{
	struct rkisp_device *isp_dev = platform_get_drvdata(dev);

	if (isp_dev)
		*sd = &isp_dev->br_dev.sd;
	else
		*sd = NULL;
}
EXPORT_SYMBOL(rkisp_get_bridge_sd);
