// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2012-2017 ASPEED Technology Inc.
// Copyright (c) 2017-2020 Intel Corporation

#include <linux/aspeed-lpc-sio.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>

#define SOC_NAME			"aspeed"
#define DEVICE_NAME			"lpc-sio"

#define AST_LPC_SWCR0300		0x00
#define  LPC_PWRGD_STS			BIT(30)
#define  LPC_PWRGD_RISING_EVT_STS	BIT(29)
#define  LPC_PWRGD_FALLING_EVT_STS	BIT(28)
#define  LPC_PWRBTN_STS			BIT(27)
#define  LPC_PWRBTN_RISING_EVT_STS	BIT(26)
#define  LPC_PWRBTN_FALLING_EVT_STS	BIT(25)
#define  LPC_S5N_STS			BIT(21)
#define  LPC_S5N_RISING_EVT_STS		BIT(20)
#define  LPC_S5N_FALLING_EVT_STS	BIT(19)
#define  LPC_S3N_STS			BIT(18)
#define  LPC_S3N_RISING_EVT_STS		BIT(17)
#define  LPC_S3N_FALLING_EVT_STS	BIT(16)
#define  LPC_PWBTO_RAW_STS		BIT(15)
#define  LPC_LAST_ONCTL_STS		BIT(14)
#define  LPC_WAS_PFAIL_STS		BIT(13)
#define  LPC_POWER_UP_FAIL_STS		BIT(12) /* Crowbar */
#define  LPC_PWRBTN_OVERRIDE_STS	BIT(11)
#define  LPC_BMC_TRIG_WAKEUP_EVT_STS	BIT(8)

#define AST_LPC_SWCR0704		0x04
#define  LPC_BMC_TRIG_WAKEUP_EVT_EN	BIT(8)

#define AST_LPC_SWCR0B08		0x08
#define  LPC_PWREQ_OUTPUT_LEVEL		BIT(25)
#define  LPC_PWBTO_OUTPUT_LEVEL		BIT(24)
#define  LPC_ONCTL_STS			BIT(15)
#define  LPC_ONCTL_GPIO_LEVEL		BIT(14)
#define  LPC_ONCTL_EN_GPIO_OUTPUT	BIT(13)
#define  LPC_ONCTL_EN_GPIO_MODE		BIT(12)
#define  LPC_BMC_TRIG_WAKEUP_EVT	BIT(6)
#define  LPC_BMC_TRIG_SMI_EVT_EN	BIT(0)

#define AST_LPC_SWCR0F0C		0x0C
#define AST_LPC_SWCR1310		0x10
#define AST_LPC_SWCR1714		0x14
#define AST_LPC_SWCR1B18		0x18
#define AST_LPC_SWCR1F1C		0x1C
#define AST_LPC_ACPIE3E0		0x20
#define AST_LPC_ACPIC1C0		0x24

#define AST_LPC_ACPIB3B0		0x28
#define  LPC_BMC_TRIG_SCI_EVT_STS	BIT(8)

#define AST_LPC_ACPIB7B4		0x2C
#define  LPC_BMC_TRIG_SCI_EVT_EN	BIT(8)

struct aspeed_lpc_sio {
	struct miscdevice		miscdev;
	struct regmap			*regmap;
	struct clk			*clk;
	struct semaphore		lock;
	unsigned int			reg_base;
};

static struct aspeed_lpc_sio *file_aspeed_lpc_sio(struct file *file)
{
	return container_of(file->private_data, struct aspeed_lpc_sio,
			    miscdev);
}

static int aspeed_lpc_sio_open(struct inode *inode, struct file *filp)
{
	return 0;
}

#define LPC_SLP3N5N_EVENT_STATUS	(\
		LPC_S5N_RISING_EVT_STS  |    \
		LPC_S5N_FALLING_EVT_STS |    \
		LPC_S3N_RISING_EVT_STS  |    \
		LPC_S3N_FALLING_EVT_STS)

/*
 *  SLPS3n SLPS5n State
 *  ---------------------------------
 *   1      1      S12
 *   0      1      S3I
 *   x      0      S45
 *************************************
 */

static void sio_get_acpi_state(struct aspeed_lpc_sio *lpc_sio,
			       struct sio_ioctl_data *sio_data)
{
	u32 reg, val;

	reg = lpc_sio->reg_base + AST_LPC_SWCR0300;
	regmap_read(lpc_sio->regmap, reg, &val);

	/* update the ACPI state event status */
	if (sio_data->param != 0) {
		if (val & LPC_SLP3N5N_EVENT_STATUS) {
			sio_data->param = 1;
			regmap_write(lpc_sio->regmap, reg,
				     LPC_SLP3N5N_EVENT_STATUS);
		} else {
			sio_data->param = 0;
		}
	}

	if ((val & LPC_S3N_STS) && (val & LPC_S5N_STS))
		sio_data->data = ACPI_STATE_S12;
	else if ((val & LPC_S3N_STS) == 0 && (val & LPC_S5N_STS))
		sio_data->data = ACPI_STATE_S3I;
	else
		sio_data->data = ACPI_STATE_S45;
}

#define LPC_PWRGD_EVENT_STATUS  (   \
		LPC_PWRGD_RISING_EVT_STS  | \
		LPC_PWRGD_FALLING_EVT_STS)

static void sio_get_pwrgd_status(struct aspeed_lpc_sio *lpc_sio,
				 struct sio_ioctl_data *sio_data)
{
	u32 reg, val;

	reg = lpc_sio->reg_base + AST_LPC_SWCR0300;
	regmap_read(lpc_sio->regmap, reg, &val);

	/* update the PWRGD event status */
	if (sio_data->param != 0) {
		if (val & LPC_PWRGD_EVENT_STATUS) {
			sio_data->param = 1;
			regmap_write(lpc_sio->regmap, reg,
				     LPC_PWRGD_EVENT_STATUS);
		} else {
			sio_data->param = 0;
		}
	}

	sio_data->data = (val & LPC_PWRGD_STS) != 0 ? 1 : 0;
}

static void sio_get_onctl_status(struct aspeed_lpc_sio *lpc_sio,
				 struct sio_ioctl_data *sio_data)
{
	u32 reg, val;

	reg = lpc_sio->reg_base + AST_LPC_SWCR0B08;
	regmap_read(lpc_sio->regmap, reg, &val);

	sio_data->data = (val & LPC_ONCTL_STS) != 0 ? 1 : 0;
}

static void sio_set_onctl_gpio(struct aspeed_lpc_sio *lpc_sio,
			       struct sio_ioctl_data *sio_data)
{
	u32 reg, val;

	reg = lpc_sio->reg_base + AST_LPC_SWCR0B08;
	regmap_read(lpc_sio->regmap, reg, &val);

	/* Enable ONCTL GPIO mode */
	if (sio_data->param != 0) {
		val |= LPC_ONCTL_EN_GPIO_MODE;
		val |= LPC_ONCTL_EN_GPIO_OUTPUT;

		if (sio_data->data != 0)
			val |=  LPC_ONCTL_GPIO_LEVEL;
		else
			val &= ~LPC_ONCTL_GPIO_LEVEL;

		regmap_write(lpc_sio->regmap, reg, val);
	} else {
		val &= ~LPC_ONCTL_EN_GPIO_MODE;
		regmap_write(lpc_sio->regmap, reg, val);
	}
}

static void sio_get_pwrbtn_override(struct aspeed_lpc_sio *lpc_sio,
				    struct sio_ioctl_data *sio_data)
{
	u32 reg, val;

	reg = lpc_sio->reg_base + AST_LPC_SWCR0300;
	regmap_read(lpc_sio->regmap, reg, &val);

	/* clear the PWRBTN OVERRIDE status */
	if (sio_data->param != 0 && val & LPC_PWRBTN_OVERRIDE_STS)
		regmap_write(lpc_sio->regmap, reg, LPC_PWRBTN_OVERRIDE_STS);

	sio_data->data = (val & LPC_PWRBTN_OVERRIDE_STS) != 0 ? 1 : 0;
}

static void sio_get_pfail_status(struct aspeed_lpc_sio *lpc_sio,
				 struct sio_ioctl_data *sio_data)
{
	u32 reg, val;

	reg = lpc_sio->reg_base + AST_LPC_SWCR0300;
	regmap_read(lpc_sio->regmap, reg, &val);

	/* [ASPEED]: SWCR_03_00[13] (Was_pfail: default 1) is used to identify
	 * this current booting is from AC loss (not DC loss) if FW cleans this
	 * bit after booting successfully every time.
	 **********************************************************************/
	if (val & LPC_WAS_PFAIL_STS) {
		regmap_write(lpc_sio->regmap, reg, 0);  /* W0C */
		sio_data->data = 1;
	} else {
		sio_data->data = 0;
	}
}

static void sio_set_bmc_sci_event(struct aspeed_lpc_sio *lpc_sio,
				  struct sio_ioctl_data *sio_data)
{
	u32 reg;

	if (sio_data->param) {
		reg = lpc_sio->reg_base + AST_LPC_ACPIB7B4;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_SCI_EVT_EN,
				  LPC_BMC_TRIG_SCI_EVT_EN);

		reg = lpc_sio->reg_base + AST_LPC_SWCR0704;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_WAKEUP_EVT_EN,
				  LPC_BMC_TRIG_WAKEUP_EVT_EN);

		reg = lpc_sio->reg_base + AST_LPC_SWCR0B08;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_WAKEUP_EVT,
				  LPC_BMC_TRIG_WAKEUP_EVT);
	} else {
		reg = lpc_sio->reg_base + AST_LPC_SWCR0300;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_WAKEUP_EVT_STS,
				  LPC_BMC_TRIG_WAKEUP_EVT_STS);

		reg = lpc_sio->reg_base + AST_LPC_ACPIB3B0;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_SCI_EVT_STS,
				  LPC_BMC_TRIG_SCI_EVT_STS);
	}

	sio_data->data = sio_data->param;
}

static void sio_set_bmc_smi_event(struct aspeed_lpc_sio *lpc_sio,
				  struct sio_ioctl_data *sio_data)
{
	u32 reg;

	if (sio_data->param) {
		reg = lpc_sio->reg_base + AST_LPC_SWCR0704;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_WAKEUP_EVT_EN,
				  LPC_BMC_TRIG_WAKEUP_EVT_EN);

		reg = lpc_sio->reg_base + AST_LPC_SWCR0B08;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_SMI_EVT_EN,
				  LPC_BMC_TRIG_SMI_EVT_EN);
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_WAKEUP_EVT,
				  LPC_BMC_TRIG_WAKEUP_EVT);
	} else {
		reg = lpc_sio->reg_base + AST_LPC_SWCR0300;
		regmap_write_bits(lpc_sio->regmap, reg,
				  LPC_BMC_TRIG_WAKEUP_EVT_STS,
				  LPC_BMC_TRIG_WAKEUP_EVT_STS);
	}

	sio_data->data = sio_data->param;
}

typedef void (*sio_cmd_fn) (struct aspeed_lpc_sio *sio_dev,
			    struct sio_ioctl_data *sio_data);

static sio_cmd_fn sio_cmd_handle[SIO_MAX_CMD] = {
	[SIO_GET_ACPI_STATE]		= sio_get_acpi_state,
	[SIO_GET_PWRGD_STATUS]		= sio_get_pwrgd_status,
	[SIO_GET_ONCTL_STATUS]		= sio_get_onctl_status,
	[SIO_SET_ONCTL_GPIO]		= sio_set_onctl_gpio,
	[SIO_GET_PWRBTN_OVERRIDE]	= sio_get_pwrbtn_override,
	[SIO_GET_PFAIL_STATUS]		= sio_get_pfail_status,
	[SIO_SET_BMC_SCI_EVENT]		= sio_set_bmc_sci_event,
	[SIO_SET_BMC_SMI_EVENT]		= sio_set_bmc_smi_event,
};

static long aspeed_lpc_sio_ioctl(struct file *file, unsigned int cmd,
				 unsigned long param)
{
	struct aspeed_lpc_sio *lpc_sio = file_aspeed_lpc_sio(file);
	struct sio_ioctl_data sio_data;
	sio_cmd_fn cmd_fn;
	long ret;

	if (copy_from_user(&sio_data, (void __user *)param, sizeof(sio_data)))
		return -EFAULT;

	if (cmd != SIO_IOC_COMMAND || sio_data.sio_cmd >= SIO_MAX_CMD)
		return -EINVAL;

	cmd_fn = sio_cmd_handle[sio_data.sio_cmd];
	if (!cmd_fn)
		return -EINVAL;

	if (down_interruptible(&lpc_sio->lock) != 0)
		return -ERESTARTSYS;

	cmd_fn(lpc_sio, &sio_data);
	ret = copy_to_user((void __user *)param, &sio_data, sizeof(sio_data));

	up(&lpc_sio->lock);

	return ret;
}

static const struct file_operations aspeed_lpc_sio_fops = {
	.owner		= THIS_MODULE,
	.open		= aspeed_lpc_sio_open,
	.unlocked_ioctl	= aspeed_lpc_sio_ioctl,
};

static int aspeed_lpc_sio_probe(struct platform_device *pdev)
{
	struct aspeed_lpc_sio *lpc_sio;
	struct device *dev;
	u32 val;
	int ret;

	dev = &pdev->dev;

	lpc_sio = devm_kzalloc(dev, sizeof(*lpc_sio), GFP_KERNEL);
	if (!lpc_sio)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, lpc_sio);

	ret = of_property_read_u32(dev->of_node, "reg", &lpc_sio->reg_base);
	if (ret) {
		dev_err(dev, "Couldn't read reg device-tree property\n");
		return ret;
	}

	lpc_sio->regmap = syscon_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(lpc_sio->regmap)) {
		dev_err(dev, "Couldn't get regmap\n");
		return -ENODEV;
	}

	/*
	 * We check that the regmap works on this very first access,
	 * but as this is an MMIO-backed regmap, subsequent regmap
	 * access is not going to fail and we skip error checks from
	 * this point.
	 */
	ret = regmap_read(lpc_sio->regmap, AST_LPC_SWCR0300, &val);
	if (ret) {
		dev_err(dev, "failed to read regmap\n");
		return ret;
	}

	sema_init(&lpc_sio->lock, 1);

	lpc_sio->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(lpc_sio->clk)) {
		ret = PTR_ERR(lpc_sio->clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "couldn't get clock\n");
		return ret;
	}
	ret = clk_prepare_enable(lpc_sio->clk);
	if (ret) {
		dev_err(dev, "couldn't enable clock\n");
		return ret;
	}

	lpc_sio->miscdev.minor = MISC_DYNAMIC_MINOR;
	lpc_sio->miscdev.name = DEVICE_NAME;
	lpc_sio->miscdev.fops = &aspeed_lpc_sio_fops;
	lpc_sio->miscdev.parent = dev;
	ret = misc_register(&lpc_sio->miscdev);
	if (ret) {
		dev_err(dev, "Unable to register device\n");
		goto err;
	}

	dev_info(dev, "Loaded at %pap (0x%08x)\n", &lpc_sio->regmap,
		 lpc_sio->reg_base);

	return 0;

err:
	clk_disable_unprepare(lpc_sio->clk);

	return ret;
}

static int aspeed_lpc_sio_remove(struct platform_device *pdev)
{
	struct aspeed_lpc_sio *lpc_sio = dev_get_drvdata(&pdev->dev);

	misc_deregister(&lpc_sio->miscdev);
	clk_disable_unprepare(lpc_sio->clk);

	return 0;
}

static const struct of_device_id aspeed_lpc_sio_match[] = {
	{ .compatible = "aspeed,ast2500-lpc-sio" },
	{ },
};
MODULE_DEVICE_TABLE(of, aspeed_lpc_sio_match);

static struct platform_driver aspeed_lpc_sio_driver = {
	.driver	= {
		.name		= SOC_NAME "-" DEVICE_NAME,
		.of_match_table = of_match_ptr(aspeed_lpc_sio_match),
	},
	.probe	= aspeed_lpc_sio_probe,
	.remove	= aspeed_lpc_sio_remove,
};
module_platform_driver(aspeed_lpc_sio_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_AUTHOR("Yong Li <yong.blli@linux.intel.com>");
MODULE_DESCRIPTION("ASPEED AST LPC SIO device driver");
