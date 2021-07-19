// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2020 Rockchip Electronics Co., Ltd */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>

#include "common.h"
#include "dev.h"
#include "hw.h"
#include "regs.h"

/*
 * rkispp_hw share hardware resource with rkispp virtual device
 * rkispp_device rkispp_device rkispp_device rkispp_device
 *       |             |             |             |
 *       \             |             |             /
 *        -----------------------------------------
 *                           |
 *                       rkispp_hw
 */

struct irqs_data {
	const char *name;
	irqreturn_t (*irq_hdl)(int irq, void *ctx);
};

struct match_data {
	int clks_num;
	const char * const *clks;
	enum rkispp_ver ispp_ver;
	struct irqs_data *irqs;
	int num_irqs;
};

/* using default value if reg no write for multi device */
static void default_sw_reg_flag(struct rkispp_device *dev)
{
	u32 reg[] = {
		RKISPP_TNR_CTRL,
		RKISPP_TNR_CORE_CTRL,
		RKISPP_NR_CTRL,
		RKISPP_NR_UVNR_CTRL_PARA,
		RKISPP_SHARP_CTRL,
		RKISPP_SHARP_CORE_CTRL,
		RKISPP_SCL0_CTRL,
		RKISPP_SCL1_CTRL,
		RKISPP_SCL2_CTRL,
		RKISPP_ORB_CORE_CTRL,
		RKISPP_FEC_CTRL,
		RKISPP_FEC_CORE_CTRL
	};
	u32 i, *flag;

	for (i = 0; i < ARRAY_SIZE(reg); i++) {
		flag = dev->sw_base_addr + reg[i] + ISPP_SW_REG_SIZE;
		*flag = 0xffffffff;
	}
}

static inline bool is_iommu_enable(struct device *dev)
{
	struct device_node *iommu;

	iommu = of_parse_phandle(dev->of_node, "iommus", 0);
	if (!iommu) {
		dev_info(dev, "no iommu attached, using non-iommu buffers\n");
		return false;
	} else if (!of_device_is_available(iommu)) {
		dev_info(dev, "iommu is disabled, using non-iommu buffers\n");
		of_node_put(iommu);
		return false;
	}
	of_node_put(iommu);

	return true;
}

static void disable_sys_clk(struct rkispp_hw_dev *dev)
{
	int i;

	for (i = 0; i < dev->clks_num; i++)
		clk_disable_unprepare(dev->clks[i]);
}

static int enable_sys_clk(struct rkispp_hw_dev *dev)
{
	int i, ret = -EINVAL;

	for (i = 0; i < dev->clks_num; i++) {
		ret = clk_prepare_enable(dev->clks[i]);
		if (ret < 0)
			goto err;
	}

	return 0;
err:
	for (--i; i >= 0; --i)
		clk_disable_unprepare(dev->clks[i]);
	return ret;
}

static irqreturn_t irq_hdl(int irq, void *ctx)
{
	struct device *dev = ctx;
	struct rkispp_hw_dev *hw_dev = dev_get_drvdata(dev);
	struct rkispp_device *ispp = hw_dev->ispp[hw_dev->cur_dev_id];
	void __iomem *base = hw_dev->base_addr;
	unsigned int mis_val;

	spin_lock(&hw_dev->irq_lock);
	mis_val = readl(base + RKISPP_CTRL_INT_STA);
	writel(mis_val, base + RKISPP_CTRL_INT_CLR);
	spin_unlock(&hw_dev->irq_lock);

	if (mis_val)
		ispp->irq_hdl(mis_val, ispp);

	return IRQ_HANDLED;
}

static const char * const rv1126_ispp_clks[] = {
	"aclk_ispp",
	"hclk_ispp",
	"clk_ispp",
};

static struct irqs_data rv1126_ispp_irqs[] = {
	{"ispp_irq", irq_hdl},
	{"fec_irq", irq_hdl},
};

static const struct match_data rv1126_ispp_match_data = {
	.clks = rv1126_ispp_clks,
	.clks_num = ARRAY_SIZE(rv1126_ispp_clks),
	.irqs = rv1126_ispp_irqs,
	.num_irqs = ARRAY_SIZE(rv1126_ispp_irqs),
	.ispp_ver = ISPP_V10,
};

static const struct of_device_id rkispp_hw_of_match[] = {
	{
		.compatible = "rockchip,rv1126-rkispp",
		.data = &rv1126_ispp_match_data,
	},
	{},
};

static int rkispp_hw_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	const struct match_data *match_data;
	struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct rkispp_hw_dev *hw_dev;
	struct resource *res;
	int i, ret, irq;

	match = of_match_node(rkispp_hw_of_match, node);
	if (IS_ERR(match))
		return PTR_ERR(match);

	hw_dev = devm_kzalloc(dev, sizeof(*hw_dev), GFP_KERNEL);
	if (!hw_dev)
		return -ENOMEM;

	dev_set_drvdata(dev, hw_dev);
	hw_dev->dev = dev;
	match_data = match->data;
	hw_dev->max_in.w = 0;
	hw_dev->max_in.h = 0;
	hw_dev->max_in.fps = 0;
	of_property_read_u32_array(node, "max-input", &hw_dev->max_in.w, 3);
	dev_info(dev, "max input:%dx%d@%dfps\n",
		 hw_dev->max_in.w, hw_dev->max_in.h, hw_dev->max_in.fps);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "get resource failed\n");
		ret = -EINVAL;
		goto err;
	}
	hw_dev->base_addr = devm_ioremap_resource(dev, res);
	if (PTR_ERR(hw_dev->base_addr) == -EBUSY) {
		resource_size_t offset = res->start;
		resource_size_t size = resource_size(res);

		hw_dev->base_addr = devm_ioremap(dev, offset, size);
	}
	if (IS_ERR(hw_dev->base_addr)) {
		dev_err(dev, "ioremap failed\n");
		ret = PTR_ERR(hw_dev->base_addr);
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
					   match_data->irqs[0].name);
	if (res) {
		/* there are irq names in dts */
		for (i = 0; i < match_data->num_irqs; i++) {
			irq = platform_get_irq_byname(pdev,
						      match_data->irqs[i].name);
			if (irq < 0) {
				dev_err(dev, "no irq %s in dts\n",
					match_data->irqs[i].name);
				ret = irq;
				goto err;
			}
			ret = devm_request_irq(dev, irq,
					       match_data->irqs[i].irq_hdl,
					       IRQF_SHARED,
					       dev_driver_string(dev),
					       dev);
			if (ret < 0) {
				dev_err(dev, "request %s failed: %d\n",
					match_data->irqs[i].name, ret);
				goto err;
			}
		}
	}

	for (i = 0; i < match_data->clks_num; i++) {
		struct clk *clk = devm_clk_get(dev, match_data->clks[i]);

		if (IS_ERR(clk)) {
			dev_err(dev, "failed to get %s\n",
				match_data->clks[i]);
			ret = PTR_ERR(clk);
			goto err;
		}
		hw_dev->clks[i] = clk;
	}
	hw_dev->clks_num = match_data->clks_num;

	hw_dev->dev_num = 0;
	hw_dev->cur_dev_id = 0;
	hw_dev->ispp_ver = match_data->ispp_ver;
	mutex_init(&hw_dev->dev_lock);
	spin_lock_init(&hw_dev->irq_lock);
	spin_lock_init(&hw_dev->buf_lock);
	atomic_set(&hw_dev->refcnt, 0);
	atomic_set(&hw_dev->power_cnt, 0);
	INIT_LIST_HEAD(&hw_dev->list);
	hw_dev->is_idle = true;
	hw_dev->is_single = true;
	if (!is_iommu_enable(dev)) {
		ret = of_reserved_mem_device_init(dev);
		if (ret)
			dev_warn(dev, "No reserved memory region assign to ispp\n");
	}

	pm_runtime_enable(&pdev->dev);

	return platform_driver_register(&rkispp_plat_drv);
err:
	return ret;
}

static int rkispp_hw_remove(struct platform_device *pdev)
{
	struct rkispp_hw_dev *hw_dev = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);
	mutex_destroy(&hw_dev->dev_lock);
	return 0;
}

static int __maybe_unused rkispp_runtime_suspend(struct device *dev)
{
	struct rkispp_hw_dev *hw_dev = dev_get_drvdata(dev);

	writel(0, hw_dev->base_addr + RKISPP_CTRL_INT_MSK);
	writel(GLB_SOFT_RST_ALL, hw_dev->base_addr + RKISPP_CTRL_RESET);
	disable_sys_clk(hw_dev);
	return 0;
}

static int __maybe_unused rkispp_runtime_resume(struct device *dev)
{
	struct rkispp_hw_dev *hw_dev = dev_get_drvdata(dev);
	void __iomem *base = hw_dev->base_addr;
	int i;

	enable_sys_clk(hw_dev);

	writel(SW_SCL_BYPASS, base + RKISPP_SCL0_CTRL);
	writel(SW_SCL_BYPASS, base + RKISPP_SCL1_CTRL);
	writel(SW_SCL_BYPASS, base + RKISPP_SCL2_CTRL);
	writel(OTHER_FORCE_UPD, base + RKISPP_CTRL_UPDATE);
	writel(GATE_DIS_ALL, base + RKISPP_CTRL_CLKGATE);
	writel(SW_SHP_DMA_DIS, base + RKISPP_SHARP_CORE_CTRL);
	writel(SW_FEC2DDR_DIS, base + RKISPP_FEC_CORE_CTRL);
	writel(0xfffffff, base + RKISPP_CTRL_INT_MSK);
	writel(GATE_DIS_NR, base + RKISPP_CTRL_CLKGATE);

	for (i = 0; i < hw_dev->dev_num; i++) {
		void *buf = hw_dev->ispp[i]->sw_base_addr;

		memset(buf, 0, ISPP_SW_MAX_SIZE);
		memcpy_fromio(buf, base, ISPP_SW_REG_SIZE);
		default_sw_reg_flag(hw_dev->ispp[i]);
	}
	return 0;
}

static const struct dev_pm_ops rkispp_hw_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rkispp_runtime_suspend,
			   rkispp_runtime_resume, NULL)
};

static struct platform_driver rkispp_hw_drv = {
	.driver = {
		.name = "rkispp_hw",
		.of_match_table = of_match_ptr(rkispp_hw_of_match),
		.pm = &rkispp_hw_pm_ops,
	},
	.probe = rkispp_hw_probe,
	.remove = rkispp_hw_remove,
};

#if IS_BUILTIN(CONFIG_VIDEO_ROCKCHIP_ISP) && IS_BUILTIN(CONFIG_VIDEO_ROCKCHIP_ISPP)
int __init rkispp_hw_drv_init(void)
{
	return platform_driver_register(&rkispp_hw_drv);
}
#else
module_platform_driver(rkispp_hw_drv);
#endif
