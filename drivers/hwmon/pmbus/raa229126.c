// SPDX-License-Identifier: GPL-2.0+
/*
 * Hardware monitoring driver for RAA229126
 *
 * Copyright (c) 2022 Intel Corporation.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "pmbus.h"

#define RAA229126_NUM_PAGES 2

static struct pmbus_driver_info raa229126_info = {
	.pages = RAA229126_NUM_PAGES,
	.format[PSC_VOLTAGE_IN] = direct,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_CURRENT_IN] = direct,
	.format[PSC_CURRENT_OUT] = direct,
	.format[PSC_TEMPERATURE] = direct,
	.format[PSC_POWER] = direct,
	.m[PSC_TEMPERATURE] = 1,
	.m[PSC_VOLTAGE_OUT] = 1,
	.R[PSC_VOLTAGE_OUT] = 3,
	.m[PSC_CURRENT_OUT] = 1,
	.m[PSC_POWER] = 1,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_IIN |
		   PMBUS_HAVE_IOUT | PMBUS_HAVE_PIN | PMBUS_HAVE_POUT |
		   PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_VOUT |
		   PMBUS_HAVE_STATUS_IOUT | PMBUS_HAVE_STATUS_INPUT |
		   PMBUS_HAVE_STATUS_TEMP,
	.func[1] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_IIN |
		   PMBUS_HAVE_IOUT | PMBUS_HAVE_PIN | PMBUS_HAVE_POUT |
		   PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_VOUT |
		   PMBUS_HAVE_STATUS_IOUT | PMBUS_HAVE_STATUS_INPUT |
		   PMBUS_HAVE_STATUS_TEMP,
};

static int raa229126_probe(struct i2c_client *client)
{
	struct pmbus_driver_info *info;
	u8 buf[I2C_SMBUS_BLOCK_MAX];
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_BYTE_DATA |
				     I2C_FUNC_SMBUS_READ_WORD_DATA |
				     I2C_FUNC_SMBUS_READ_BLOCK_DATA))
		return -ENODEV;

	/* Read Manufacturer id */
	ret = i2c_smbus_read_block_data(client, PMBUS_IC_DEVICE_ID, buf);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to read PMBUS_IC_DEVICE_ID\n");
		return ret;
	}
	if (ret != 4 || strncmp(buf, "\x00\x82\xd2\x49", 4)) {
		dev_err(&client->dev, "DEVICE_ID unrecognized\n");
		return -ENODEV;
	}

	info = devm_kmemdup(&client->dev, &raa229126_info, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	return pmbus_do_probe(client, info);
}

static const struct i2c_device_id raa229126_id[] = {
	{"raa229126", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, raa229126_id);

static struct i2c_driver raa229126_driver = {
	.driver = {
		   .name = "raa229126",
		   },
	.probe_new = raa229126_probe,
	.id_table = raa229126_id,
};

module_i2c_driver(raa229126_driver);

MODULE_AUTHOR("Zhikui Ren <zhikui.ren@intel.com>");
MODULE_DESCRIPTION("PMBus driver for Renesas RAA229126");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
