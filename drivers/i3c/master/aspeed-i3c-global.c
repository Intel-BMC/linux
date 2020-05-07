// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2019 ASPEED Technology Inc.

#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/delay.h>

#define ASPEED_I3CG_CTRL(x)		(0x10 + (x*0x10))
#define ASPEED_I3CG_SET(x)		(0x14 + (x*0x10))

struct aspeed_i3c_global {
	void __iomem		*base;
	struct reset_control	*rst;
};

static int aspeed_i3c_global_probe(struct platform_device *pdev)
{
	struct aspeed_i3c_global *i3c_global;
	struct device_node *node = pdev->dev.of_node;
	int i = 0;

	i3c_global = kzalloc(sizeof(*i3c_global), GFP_KERNEL);
	if (!i3c_global)
		return -ENOMEM;

	i3c_global->base = of_iomap(node, 0);
	if (!i3c_global->base) {
		return -ENOMEM;
	}

	i3c_global->rst = devm_reset_control_get_exclusive(&pdev->dev, NULL);

	if (IS_ERR(i3c_global->rst)) {
		if (PTR_ERR(i3c_global->rst) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "missing or invalid reset controller device tree entry\n");
		return PTR_ERR(i3c_global->rst);
	}

	reset_control_assert(i3c_global->rst);
	udelay(3);
	reset_control_deassert(i3c_global->rst);

	/* init */
	for(i = 0; i < 5; i++)
		writel(0x000474c4, i3c_global->base + ASPEED_I3CG_SET(i));

	return 0;
}

static const struct of_device_id aspeed_i3c_of_match[] = {
	{ .compatible = "aspeed,ast2600-i3c-global", },
	{ },
};
MODULE_DEVICE_TABLE(of, aspeed_i3c_of_match);

static struct platform_driver aspeed_i3c_driver = {
	.probe  = aspeed_i3c_global_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = aspeed_i3c_of_match,
	},
};
module_platform_driver(aspeed_i3c_driver);

MODULE_AUTHOR("Ryan Chen");
MODULE_DESCRIPTION("ASPEED I3C Global Driver");
MODULE_LICENSE("GPL v2");
