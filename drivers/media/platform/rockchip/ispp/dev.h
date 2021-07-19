/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2019 Fuzhou Rockchip Electronics Co., Ltd. */

#ifndef _RKISPP_DEV_H
#define _RKISPP_DEV_H

#include "ispp.h"
#include "params.h"
#include "stream.h"
#include "stats.h"
#include "hw.h"

#define DRIVER_NAME			"rkispp"
#define II_VDEV_NAME DRIVER_NAME	"_input_image"
#define MB_VDEV_NAME DRIVER_NAME	"_m_bypass"
#define S0_VDEV_NAME DRIVER_NAME	"_scale0"
#define S1_VDEV_NAME DRIVER_NAME	"_scale1"
#define S2_VDEV_NAME DRIVER_NAME	"_scale2"

enum rkispp_input {
	INP_INVAL = 0,
	INP_ISP,
	INP_DDR,
};

struct rkispp_device {
	char name[128];
	struct device *dev;
	void *sw_base_addr;
	struct media_device media_dev;
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;

	struct rkispp_hw_dev *hw_dev;
	struct rkispp_subdev ispp_sdev;
	struct rkispp_stream_vdev stream_vdev;
	struct rkispp_params_vdev params_vdev;
	struct rkispp_stats_vdev stats_vdev;

	enum rkispp_ver	ispp_ver;
	/* mutex to serialize the calls from user */
	struct mutex apilock;
	/* mutex to serialize the calls of iq */
	struct mutex iqlock;
	enum rkispp_input inp;
	u32 dev_id;
	u32 isp_mode;
	wait_queue_head_t sync_onoff;
	bool stream_sync;

	void (*irq_hdl)(u32 mis, struct rkispp_device *dev);
};
#endif
