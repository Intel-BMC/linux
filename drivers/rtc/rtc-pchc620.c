// SPDX-License-Identifier: GPL-2.0+
/*
 * RTC driver for PCHC620
 * Copyright (C) 2021 YADRO
 */

#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/rtc.h>
#include <linux/bcd.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define PCH_REG_FORCE_OFF		0x00
#define PCH_REG_SC			0x09
#define PCH_REG_MN			0x0a
#define PCH_REG_HR			0x0b
#define PCH_REG_DW			0x0c
#define PCH_REG_DM			0x0d
#define PCH_REG_MO			0x0e
#define PCH_REG_YR			0x0f

#define NUM_TIME_REGS   (PCH_REG_YR - PCH_REG_SC + 1)

struct pch {
	struct rtc_device *rtc;
	struct regmap *regmap;
};

static int pchc620_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pch *pch = i2c_get_clientdata(client);
	unsigned char rtc_data[NUM_TIME_REGS] = {0};
	int rc;

	rc = regmap_bulk_read(pch->regmap, PCH_REG_SC, rtc_data, NUM_TIME_REGS);
	if (rc < 0) {
		dev_err(dev, "Fail to read time reg(%d)\n", rc);
		return rc;
	}

	tm->tm_sec	= bcd2bin(rtc_data[0]);
	tm->tm_min	= bcd2bin(rtc_data[1]);
	tm->tm_hour	= bcd2bin(rtc_data[2]);
	tm->tm_wday	= rtc_data[3];
	tm->tm_mday	= bcd2bin(rtc_data[4]);
	tm->tm_mon	= bcd2bin(rtc_data[5]) - 1;
	tm->tm_year	= bcd2bin(rtc_data[6]) + 100;

	return 0;
}

static ssize_t pch_force_off(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pch *pch = i2c_get_clientdata(client);
	unsigned long val;
	int rc;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val) {
		/* 0x02 host force off */
		rc = regmap_write(pch->regmap, PCH_REG_FORCE_OFF, 0x2);
		if (rc < 0) {
			dev_err(dev, "Fail to read time reg(%d)\n", rc);
			return rc;
		}
	}

	return 0;
}
static DEVICE_ATTR(force_off, S_IWUSR | S_IWGRP, NULL, pch_force_off);

static const struct rtc_class_ops pchc620_rtc_ops = {
	.read_time = pchc620_rtc_read_time,
};

static const struct regmap_config pchc620_rtc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.use_single_read = true,
};

static int pchc620_rtc_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct pch *pch;
	int rc;

	pch = devm_kzalloc(&client->dev, sizeof(*pch), GFP_KERNEL);
	if (!pch)
		return -ENOMEM;

	pch->regmap = devm_regmap_init_i2c(client, &pchc620_rtc_regmap_config);
	if (IS_ERR(pch->regmap)) {
		dev_err(&client->dev, "regmap_init failed\n");
		return PTR_ERR(pch->regmap);
	}

	i2c_set_clientdata(client, pch);

	pch->rtc = devm_rtc_device_register(&client->dev, "pch-rtc",
				&pchc620_rtc_ops, THIS_MODULE);
	if (IS_ERR(pch->rtc))
		return PTR_ERR(pch->rtc);

	rc = sysfs_create_file(&client->dev.kobj, &dev_attr_force_off.attr);
	if (rc)
		return rc;

	return 0;
}

static int pchc620_rtc_remove(struct i2c_client *client)
{
	sysfs_remove_file(&client->dev.kobj, &dev_attr_force_off.attr);
	return 0;
}

static const struct i2c_device_id pchc620_rtc_id[] = {
	{ "pchc620-rtc", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pchc620_rtc_id);

static const struct of_device_id pchc620_rtc_of_match[] = {
	{ .compatible = "rtc,pchc620", },
	{ }
};
MODULE_DEVICE_TABLE(of, pchc620_rtc_of_match);

static struct i2c_driver pchc620_rtc_driver = {
	.driver		= {
		.name	= "pchc620-rtc",
		.of_match_table = pchc620_rtc_of_match,
	},
	.probe		= pchc620_rtc_probe,
	.remove		= pchc620_rtc_remove,
	.id_table	= pchc620_rtc_id,
};
module_i2c_driver(pchc620_rtc_driver);

MODULE_DESCRIPTION("RTC PCHC620 driver");
MODULE_AUTHOR("Ivan Mikhaylov <i.mikhaylov@yadro.com>");
MODULE_LICENSE("GPL");
