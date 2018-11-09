// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Intel Corporation
 * VGA Shared Memory driver for Aspeed AST2500
 */

#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_platform.h>

#define SHAREDMEM_NAME "vgasharedmem"

struct aspeed_vga_sharedmem {
	struct miscdevice	miscdev;
	unsigned int	addr;
	unsigned int	size;
	bool	mmap_enable;
};

static struct aspeed_vga_sharedmem *file_sharemem(struct file *file)
{
	return container_of(file->private_data,
			struct aspeed_vga_sharedmem, miscdev);
}

static int vga_open(struct inode *inode, struct file *file)
{
	struct aspeed_vga_sharedmem *vga_sharedmem = file_sharemem(file);

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!vga_sharedmem->mmap_enable)
		return -EPERM;

	return 0;
}

static int vga_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct aspeed_vga_sharedmem *vga_sharedmem = file_sharemem(file);

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	vma->vm_flags = (vma->vm_flags & (~VM_WRITE));
	remap_pfn_range(vma, vma->vm_start, vga_sharedmem->addr >> PAGE_SHIFT,
			vga_sharedmem->size, vma->vm_page_prot);
	return 0;
}

static ssize_t enable_mmap_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct aspeed_vga_sharedmem *vga_sharedmem = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", vga_sharedmem->mmap_enable);
}

static ssize_t enable_mmap_store(struct device *dev,
				 struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct aspeed_vga_sharedmem *vga_sharedmem =
		dev_get_drvdata(dev);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	vga_sharedmem->mmap_enable = val;

	return count;
}
static DEVICE_ATTR_RW(enable_mmap);

static struct attribute *sharedmem_attrs[] = {
	&dev_attr_enable_mmap.attr,
	NULL
};

static const struct attribute_group sharedmem_attr_group = {
	.attrs = sharedmem_attrs,
};

static const struct attribute_group *sharedmem_attr_groups[] = {
	&sharedmem_attr_group,
	NULL
};

static const struct file_operations vga_sharedmem_fops = {
	.owner	= THIS_MODULE,
	.open	= vga_open,
	.mmap	= vga_mmap,
};

static struct miscdevice vga_sharedmem_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = SHAREDMEM_NAME,
	.fops = &vga_sharedmem_fops,
	.groups = sharedmem_attr_groups,
};

static int vga_sharedmem_probe(struct platform_device *pdev)
{
	struct aspeed_vga_sharedmem *vga_sharedmem;
	struct device *dev = &pdev->dev;
	struct resource *rc;

	vga_sharedmem = devm_kzalloc(dev, sizeof(*vga_sharedmem), GFP_KERNEL);
	if (!vga_sharedmem)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, vga_sharedmem);

	rc = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!rc) {
		dev_err(dev, "Couldn't read size device-tree property\n");
		return -ENXIO;
	}

	vga_sharedmem->addr = rc->start;
	vga_sharedmem->size = resource_size(rc);
	vga_sharedmem->mmap_enable = true;

	vga_sharedmem->miscdev = vga_sharedmem_miscdev;

	return misc_register(&vga_sharedmem->miscdev);
}

static int vga_sharedmem_remove(struct platform_device *pdev)
{
	struct aspeed_vga_sharedmem *vga_sharedmem =
				dev_get_drvdata(&pdev->dev);

	misc_deregister(&vga_sharedmem->miscdev);

	return 0;
}

static const struct of_device_id vga_sharedmem_match[] = {
	{ .compatible = "aspeed,ast2500-vga-sharedmem", },
	{ }
};
MODULE_DEVICE_TABLE(of, vga_sharedmem_match);

static struct platform_driver vga_sharedmem_driver = {
	.driver = {
		.name	= "VGA-SHAREDMEM",
		.of_match_table = vga_sharedmem_match,
	},
	.probe = vga_sharedmem_probe,
	.remove = vga_sharedmem_remove,
};

module_platform_driver(vga_sharedmem_driver);

MODULE_AUTHOR("Yang Cheng <cheng.c.yang@intel.com>");
MODULE_DESCRIPTION("Shared VGA memory");
MODULE_LICENSE("GPL v2");
