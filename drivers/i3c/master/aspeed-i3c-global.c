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

#define ASPEED_I3CG_CTRL(x)		(0x10 + (x * 0x10))
#define ASPEED_I3CG_SET(x)		(0x14 + (x * 0x10))

#define DEF_SLV_INST_ID			0x4
#define DEF_SLV_STATIC_ADDR		0x74

union i3c_set_reg {
	uint32_t value;
	struct {
		unsigned int i2c_mode : 1;	/* bit[0] */
		unsigned int test_mode : 1;	/* bit[1] */
		unsigned int act_mode : 2;	/* bit[ 3: 2] */
		unsigned int pending_int : 4;	/* bit[ 7: 4] */
		unsigned int sa : 7;		/* bit[14: 8] */
		unsigned int sa_en : 1;		/* bit[15] */
		unsigned int inst_id : 4;	/* bit[19:16] */
		unsigned int rsvd : 12;		/* bit[31:20] */
	} fields;
};


struct aspeed_i3c_global {
	void __iomem		*base;
	struct reset_control	*rst;
};

static const struct of_device_id aspeed_i3c_global_of_table[] = {
	{ .compatible = "aspeed,ast2600-i3c-global", },
	{},
};
MODULE_DEVICE_TABLE(of, aspeed_i3c_global_of_table);

static int aspeed_i3c_global_probe(struct platform_device *pdev)
{
	struct aspeed_i3c_global *i3c_global;
	union i3c_set_reg reg;
	int i, ret;
	u32 num_of_i3cs;

	i3c_global = devm_kzalloc(&pdev->dev, sizeof(*i3c_global), GFP_KERNEL);
	if (!i3c_global)
		return -ENOMEM;

	i3c_global->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i3c_global->base))
		return PTR_ERR(i3c_global->base);

	i3c_global->rst = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(i3c_global->rst)) {
		dev_err(&pdev->dev,
			"missing or invalid reset controller device tree entry");
		return PTR_ERR(i3c_global->rst);
	}

	reset_control_assert(i3c_global->rst);
	udelay(3);
	reset_control_deassert(i3c_global->rst);

	ret = of_property_read_u32(pdev->dev.of_node, "ni3cs", &num_of_i3cs);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to get number of i3c devices");
		return -EINVAL;
	}

	reg.value = 0;
	reg.fields.inst_id = DEF_SLV_INST_ID;
	reg.fields.sa = DEF_SLV_STATIC_ADDR;
	reg.fields.pending_int = 0xc;
	reg.fields.act_mode = 0x1;
	for (i = 0; i < num_of_i3cs; i++)
		writel(reg.value, i3c_global->base + ASPEED_I3CG_SET(i));

	dev_info(&pdev->dev, "i3c global control registered\n");

	return 0;
}

static struct platform_driver aspeed_i3c_driver = {
	.probe  = aspeed_i3c_global_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_match_ptr(aspeed_i3c_global_of_table),
	},
};
module_platform_driver(aspeed_i3c_driver);

MODULE_AUTHOR("Ryan Chen");
MODULE_DESCRIPTION("ASPEED I3C Global Driver");
MODULE_LICENSE("GPL v2");
