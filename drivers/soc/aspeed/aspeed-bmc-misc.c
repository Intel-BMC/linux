// SPDX-License-Identifier: GPL-2.0+
// Copyright 2018 IBM Corp.

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <crypto/hash.h>

#define DEVICE_NAME "aspeed-bmc-misc"

struct aspeed_bmc_ctrl {
	const char *name;
	u32 offset;
	u64 mask;
	u32 shift;
	bool read_only;
	u32 reg_width;
	const char *hash_data;
	struct regmap *map;
	struct kobj_attribute attr;
};

struct aspeed_bmc_misc {
	struct device *dev;
	struct regmap *map;
	struct aspeed_bmc_ctrl *ctrls;
	int nr_ctrls;
};

static int aspeed_bmc_misc_parse_dt_child(struct device_node *child,
					  struct aspeed_bmc_ctrl *ctrl)
{
	int rc;
	u32 mask;

	/* Example child:
	 *
	 * ilpc2ahb {
	 *     offset = <0x80>;
	 *     bit-mask = <0x1>;
	 *     bit-shift = <6>;
	 *     reg-width = <64>;
	 *     label = "foo";
	 * }
	 */
	if (of_property_read_string(child, "label", &ctrl->name))
		ctrl->name = child->name;

	rc = of_property_read_u32(child, "offset", &ctrl->offset);
	if (rc < 0)
		return rc;

	/* optional reg-width, default to 32 */
	rc = of_property_read_u32(child, "reg-width", &ctrl->reg_width);
	if (rc < 0 || ctrl->reg_width != 64)
		ctrl->reg_width = 32;

	if (ctrl->reg_width == 32) {
		rc = of_property_read_u32(child, "bit-mask", &mask);
		if (rc < 0)
			return rc;
		ctrl->mask = mask;
	} else {
		rc = of_property_read_u64(child, "bit-mask", &ctrl->mask);
		if (rc < 0)
			return rc;
	}

	rc = of_property_read_u32(child, "bit-shift", &ctrl->shift);
	if (rc < 0)
		return rc;

	ctrl->read_only = of_property_read_bool(child, "read-only");

	ctrl->mask <<= ctrl->shift;
	/* optional hash_data for obfuscating reads */
	if (of_property_read_string(child, "hash-data", &ctrl->hash_data))
		ctrl->hash_data = NULL;

	return 0;
}

static int aspeed_bmc_misc_parse_dt(struct aspeed_bmc_misc *bmc,
				    struct device_node *parent)
{
	struct aspeed_bmc_ctrl *ctrl;
	struct device_node *child;
	int rc;

	bmc->nr_ctrls = of_get_child_count(parent);
	bmc->ctrls = devm_kcalloc(bmc->dev, bmc->nr_ctrls, sizeof(*bmc->ctrls),
				  GFP_KERNEL);
	if (!bmc->ctrls)
		return -ENOMEM;

	ctrl = bmc->ctrls;
	for_each_child_of_node(parent, child) {
		rc = aspeed_bmc_misc_parse_dt_child(child, ctrl++);
		if (rc < 0) {
			of_node_put(child);
			return rc;
		}
	}

	return 0;
}

#define SHA256_DIGEST_LEN 32
static int hmac_sha256(u8 *key, u8 ksize, const char *plaintext, u8 psize,
		u8 *output)
{
	struct crypto_shash *tfm;
	struct shash_desc *shash;
	int ret;

	if (!ksize)
		return -EINVAL;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm)) {
		return -ENOMEM;
	}

	ret = crypto_shash_setkey(tfm, key, ksize);
	if (ret)
		goto failed;

	shash = kzalloc(sizeof(*shash) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!shash) {
		ret = -ENOMEM;
		goto failed;
	}

	shash->tfm = tfm;
	ret = crypto_shash_digest(shash, plaintext, psize, output);

	kfree(shash);

failed:
	crypto_free_shash(tfm);
	return ret;
}

static ssize_t aspeed_bmc_misc_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct aspeed_bmc_ctrl *ctrl;
	u32 val;
	u64 val64;
	int rc;
	u8 *binbuf;
	size_t buf_len;
	u8 hashbuf[SHA256_DIGEST_LEN];

	ctrl = container_of(attr, struct aspeed_bmc_ctrl, attr);

	if (ctrl->reg_width == 32) {
		rc = regmap_read(ctrl->map, ctrl->offset, &val);
		if (rc)
			return rc;
		val &= (u32)ctrl->mask;
		val >>= ctrl->shift;

		return sprintf(buf, "%u\n", val);
	}
	rc = regmap_read(ctrl->map, ctrl->offset, &val);
	if (rc)
		return rc;
	val64 = val;
	rc = regmap_read(ctrl->map, ctrl->offset + sizeof(u32), &val);
	if (rc)
		return rc;
	/* aspeed puts 64-bit regs as L, H in address space */
	val64 |= (u64)val << 32;
	val64 &= ctrl->mask;
	val64 >>= ctrl->shift;
	buf_len = sizeof(val64);

	if (ctrl->hash_data) {
		rc = hmac_sha256((u8*)&val64, buf_len, ctrl->hash_data,
				strlen(ctrl->hash_data), hashbuf);
		if (rc)
			return rc;
		buf_len = SHA256_DIGEST_LEN;
		binbuf = hashbuf;
	} else {
		binbuf = (u8*)&val64;
		buf_len = sizeof(val64);
	}
	bin2hex(buf, binbuf, buf_len);
	buf[buf_len * 2] = '\n';
	rc = buf_len * 2 + 1;

	return rc;

}

static ssize_t aspeed_bmc_misc_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	struct aspeed_bmc_ctrl *ctrl;
	long val;
	int rc;

	ctrl = container_of(attr, struct aspeed_bmc_ctrl, attr);

	if (ctrl->read_only)
		return -EROFS;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;

	val <<= ctrl->shift;
	rc = regmap_write_bits(ctrl->map, ctrl->offset, ctrl->mask, val);

	return rc < 0 ? rc : count;
}

static int aspeed_bmc_misc_add_sysfs_attr(struct aspeed_bmc_misc *bmc,
					  struct aspeed_bmc_ctrl *ctrl)
{
	ctrl->map = bmc->map;

	sysfs_attr_init(&ctrl->attr.attr);
	ctrl->attr.attr.name = ctrl->name;
	ctrl->attr.attr.mode = 0664;
	ctrl->attr.show = aspeed_bmc_misc_show;
	ctrl->attr.store = aspeed_bmc_misc_store;

	return sysfs_create_file(&bmc->dev->kobj, &ctrl->attr.attr);
}

static int aspeed_bmc_misc_populate_sysfs(struct aspeed_bmc_misc *bmc)
{
	int rc;
	int i;

	for (i = 0; i < bmc->nr_ctrls; i++) {
		rc = aspeed_bmc_misc_add_sysfs_attr(bmc, &bmc->ctrls[i]);
		if (rc < 0)
			return rc;
	}

	return 0;
}

static int aspeed_bmc_misc_probe(struct platform_device *pdev)
{
	struct aspeed_bmc_misc *bmc;
	int rc;

	bmc = devm_kzalloc(&pdev->dev, sizeof(*bmc), GFP_KERNEL);
	if (!bmc)
		return -ENOMEM;

	bmc->dev = &pdev->dev;
	bmc->map = syscon_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(bmc->map))
		return PTR_ERR(bmc->map);

	rc = aspeed_bmc_misc_parse_dt(bmc, pdev->dev.of_node);
	if (rc < 0)
		return rc;

	return aspeed_bmc_misc_populate_sysfs(bmc);
}

static const struct of_device_id aspeed_bmc_misc_match[] = {
	{ .compatible = "aspeed,bmc-misc" },
	{ },
};

static struct platform_driver aspeed_bmc_misc = {
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table = aspeed_bmc_misc_match,
	},
	.probe = aspeed_bmc_misc_probe,
};

module_platform_driver(aspeed_bmc_misc);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
