/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2020 Rockchip Electronics Co., Ltd. */

#ifndef _RKISP_HW_H
#define _RKISP_HW_H

#include "bridge.h"

#define RKISP_MAX_BUS_CLK 8

struct rkisp_hw_dev {
	const struct isp_match_data *match_data;
	struct platform_device *pdev;
	struct device *dev;
	struct regmap *grf;
	void __iomem *base_addr;
	struct clk *clks[RKISP_MAX_BUS_CLK];
	int num_clks;
	const unsigned int *clk_rate_tbl;
	int num_clk_rate_tbl;
	int mipi_irq;
	enum rkisp_isp_ver isp_ver;
	struct rkisp_device *isp[DEV_MAX];
	int dev_num;
	int cur_dev_id;
	int mipi_dev_id;
	struct max_input max_in;
	/* lock for multi dev */
	struct mutex dev_lock;
	spinlock_t rdbk_lock;
	atomic_t power_cnt;
	atomic_t refcnt;

	/* share buf for multi dev */
	spinlock_t buf_lock;
	struct rkisp_bridge_buf bufs[BRIDGE_BUF_MAX];
	struct rkisp_ispp_buf *cur_buf;
	struct rkisp_ispp_buf *nxt_buf;
	struct list_head list;

	bool is_idle;
	bool is_single;
	bool is_mi_update;
	bool is_thunderboot;
};

int rkisp_register_irq(struct rkisp_hw_dev *dev);
#endif
