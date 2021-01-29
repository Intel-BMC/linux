// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2017 IBM Corporation
// TODO: Rewrite this driver

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/kfifo.h>

#define DEVICE_NAME	"aspeed-mbox"

#define MBX_USE_INTERRUPT 1

#define   ASPEED_MBOX_CTRL_RECV BIT(7)
#define   ASPEED_MBOX_CTRL_MASK BIT(1)
#define   ASPEED_MBOX_CTRL_SEND BIT(0)

#define AST2600_MBOX_NUM_REGS 32
#define AST2600_MBOX_DATA_0 0x00
#define AST2600_MBOX_STATUS_0 0x80
#define AST2600_MBOX_BMC_CTRL 0x90
#define AST2600_MBOX_INTERRUPT_0 0xA0

#define AST2500_MBOX_NUM_REGS 16
#define AST2500_MBOX_DATA_0 0x00
#define AST2500_MBOX_STATUS_0 0x40
#define AST2500_MBOX_BMC_CTRL 0x48
#define AST2500_MBOX_INTERRUPT_0 0x50

struct aspeed_mbox_config {
	u32 num_regs;
	u32 data_offset;
	u32 status_offset;
	u32 bmc_control_offset;
	u32 bmc_interrupt_offset;
};

static const struct aspeed_mbox_config ast2500_config = {
	.num_regs = AST2500_MBOX_NUM_REGS,
	.data_offset = AST2500_MBOX_DATA_0,
	.status_offset = AST2500_MBOX_STATUS_0,
	.bmc_control_offset = AST2500_MBOX_BMC_CTRL,
	.bmc_interrupt_offset = AST2500_MBOX_INTERRUPT_0,
};

static const struct aspeed_mbox_config ast2600_config = {
	.num_regs = AST2600_MBOX_NUM_REGS,
	.data_offset = AST2600_MBOX_DATA_0,
	.status_offset = AST2600_MBOX_STATUS_0,
	.bmc_control_offset = AST2600_MBOX_BMC_CTRL,
	.bmc_interrupt_offset = AST2600_MBOX_INTERRUPT_0,
};

struct aspeed_mbox {
	struct miscdevice	miscdev;
	struct regmap		*regmap;
	struct clk		*clk;
	unsigned int		base;
	int			irq;
	wait_queue_head_t	queue;
	struct mutex		mutex;
	struct kfifo		fifo;
	spinlock_t			lock;
	struct aspeed_mbox_config	configs;
};

static atomic_t aspeed_mbox_open_count = ATOMIC_INIT(0);

static u8 aspeed_mbox_inb(struct aspeed_mbox *mbox, int reg)
{
	/*
	 * The mbox registers are actually only one byte but are addressed
	 * four bytes apart. The other three bytes are marked 'reserved',
	 * they *should* be zero but lets not rely on it.
	 * I am going to rely on the fact we can casually read/write to them...
	 */
	unsigned int val = 0xff; /* If regmap throws an error return 0xff */
	int rc = regmap_read(mbox->regmap, mbox->base + reg, &val);

	if (rc)
		dev_err(mbox->miscdev.parent, "regmap_read() failed with "
				"%d (reg: 0x%08x)\n", rc, reg);

	return val & 0xff;
}

static void aspeed_mbox_outb(struct aspeed_mbox *mbox, u8 data, int reg)
{
	int rc = regmap_write(mbox->regmap, mbox->base + reg, data);

	if (rc)
		dev_err(mbox->miscdev.parent, "regmap_write() failed with "
				"%d (data: %u reg: 0x%08x)\n", rc, data, reg);
}

static struct aspeed_mbox *file_mbox(struct file *file)
{
	return container_of(file->private_data, struct aspeed_mbox, miscdev);
}

/* Save a byte to a FIFO and discard the oldest byte if FIFO is full */
static void put_fifo_with_discard(struct aspeed_mbox *mbox, u8 val)
{
	if (!kfifo_initialized(&mbox->fifo))
		return;
	if (kfifo_is_full(&mbox->fifo))
		kfifo_skip(&mbox->fifo);
	kfifo_put(&mbox->fifo, val);
}

static int aspeed_mbox_open(struct inode *inode, struct file *file)
{
#if MBX_USE_INTERRUPT
	struct aspeed_mbox *mbox = file_mbox(file);
	int i;
#endif

	if (atomic_inc_return(&aspeed_mbox_open_count) == 1) {
#if MBX_USE_INTERRUPT
		/*
		 * Reset the FIFO while opening to clear the old cached data
		 * and load the FIFO with latest mailbox register values.
		 */
		kfifo_reset(&mbox->fifo);
		spin_lock_irq(&mbox->lock);
		for (i = 0; i < mbox->configs.num_regs; i++) {
			put_fifo_with_discard(mbox,
					aspeed_mbox_inb(mbox, mbox->configs.data_offset + (i * 4)));
		}
		spin_unlock_irq(&mbox->lock);

#endif
		return 0;
	}

	atomic_dec(&aspeed_mbox_open_count);
	return -EBUSY;
}

static ssize_t aspeed_mbox_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct aspeed_mbox *mbox = file_mbox(file);
	char __user *p = buf;
	ssize_t ret;
	unsigned int copied;
	unsigned long flags;
	int i;

	if (!access_ok(buf, count))
		return -EFAULT;

	if (count + *ppos > mbox->configs.num_regs)
		return -EINVAL;

#if MBX_USE_INTERRUPT
	/*
	 * Restrict count as per the number of mailbox registers
	 * to use kfifo.
	 */
	if (count != mbox->configs.num_regs)
		goto reg_read;

	if (kfifo_is_empty(&mbox->fifo)) {
		if (file->f_flags & O_NONBLOCK){
			return -EAGAIN;
			}
		ret = wait_event_interruptible(mbox->queue,
				!kfifo_is_empty(&mbox->fifo));
		if (ret == -ERESTARTSYS){
			return -EINTR;
			}
	}

	spin_lock_irqsave(&mbox->lock, flags);
	ret = kfifo_to_user(&mbox->fifo, buf, count, &copied);
	spin_unlock_irqrestore(&mbox->lock, flags);
	return ret ? ret : copied;

#endif

reg_read:
	mutex_lock(&mbox->mutex);

	for (i = *ppos; count > 0 && i < mbox->configs.num_regs; i++) {
		uint8_t reg = aspeed_mbox_inb(mbox, mbox->configs.data_offset + (i * 4));

		ret = __put_user(reg, p);
		if (ret)
			goto out_unlock;

		p++;
		count--;
	}
	ret = p - buf;

out_unlock:
	mutex_unlock(&mbox->mutex);
	return ret;
}

static ssize_t aspeed_mbox_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct aspeed_mbox *mbox = file_mbox(file);
	const char __user *p = buf;
	ssize_t ret;
	char c;
	int i;

	if (!access_ok(buf, count))
		return -EFAULT;

	if (count + *ppos > mbox->configs.num_regs)
		return -EINVAL;

	mutex_lock(&mbox->mutex);

	for (i = *ppos; count > 0 && i < mbox->configs.num_regs; i++) {
		ret = __get_user(c, p);
		if (ret)
			goto out_unlock;

		aspeed_mbox_outb(mbox, c, mbox->configs.data_offset + (i * 4));
		p++;
		count--;
	}

	for (i = 0; i < mbox->configs.num_regs / 8; i++)
		aspeed_mbox_outb(mbox, 0xff, mbox->configs.status_offset + (i * 4));

	aspeed_mbox_outb(mbox,
		ASPEED_MBOX_CTRL_RECV | ASPEED_MBOX_CTRL_MASK | ASPEED_MBOX_CTRL_SEND,
		mbox->configs.bmc_control_offset);
	ret = p - buf;

out_unlock:
	mutex_unlock(&mbox->mutex);
	return ret;
}

static unsigned int aspeed_mbox_poll(struct file *file, poll_table *wait)
{
	struct aspeed_mbox *mbox = file_mbox(file);

	poll_wait(file, &mbox->queue, wait);
	return !kfifo_is_empty(&mbox->fifo) ? POLLIN : 0;
}

static int aspeed_mbox_release(struct inode *inode, struct file *file)
{
	atomic_dec(&aspeed_mbox_open_count);
	return 0;
}

static const struct file_operations aspeed_mbox_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_seek_end_llseek,
	.read		= aspeed_mbox_read,
	.write		= aspeed_mbox_write,
	.open		= aspeed_mbox_open,
	.release	= aspeed_mbox_release,
	.poll		= aspeed_mbox_poll,
};

static irqreturn_t aspeed_mbox_irq(int irq, void *arg)
{
	struct aspeed_mbox *mbox = arg;
#if MBX_USE_INTERRUPT
	int i;

	dev_dbg(mbox->miscdev.parent, "BMC_CTRL11: 0x%02x\n",
		aspeed_mbox_inb(mbox, mbox->configs.bmc_control_offset));
	for (i = 0; i < mbox->configs.num_regs / 8; i++) {
		dev_dbg(mbox->miscdev.parent, "STATUS: 0x%02x\n",
		aspeed_mbox_inb(mbox, mbox->configs.status_offset + (i * 4)));
	}
	for (i = 0; i < mbox->configs.num_regs; i++) {
		dev_dbg(mbox->miscdev.parent, "DATA_%d: 0x%02x\n", i,
		aspeed_mbox_inb(mbox, mbox->configs.data_offset + (i * 4)));
	}

	spin_lock(&mbox->lock);
	for (i = 0; i < mbox->configs.num_regs; i++) {
		put_fifo_with_discard(mbox,
				aspeed_mbox_inb(mbox, mbox->configs.data_offset + (i * 4)));
	}
	spin_unlock(&mbox->lock);
#endif

	/* Clear interrupt status */
	for (i = 0; i < mbox->configs.num_regs / 8; i++)
		aspeed_mbox_outb(mbox, 0xff, mbox->configs.status_offset + (i * 4));

	aspeed_mbox_outb(mbox, ASPEED_MBOX_CTRL_RECV, mbox->configs.bmc_control_offset);

	wake_up(&mbox->queue);
	return IRQ_HANDLED;
}

static int aspeed_mbox_config_irq(struct aspeed_mbox *mbox,
		struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc;
	int i;
	mbox->irq = platform_get_irq(pdev, 0);
	if (!mbox->irq)
		return -ENODEV;

	rc = devm_request_irq(dev, mbox->irq, aspeed_mbox_irq,
				IRQF_SHARED, DEVICE_NAME, mbox);
	if (rc < 0) {
		dev_err(dev, "Unable to request IRQ %d\n", mbox->irq);
		return rc;
	}

	/* Disable all register based interrupts. */
	for (i = 0; i < mbox->configs.num_regs / 8; i++)
		aspeed_mbox_outb(mbox, 0xff, mbox->configs.bmc_interrupt_offset + i * 4);

	/* These registers are write one to clear. Clear them. */
	for (i = 0; i < mbox->configs.num_regs / 8; i++)
		aspeed_mbox_outb(mbox, 0xff, mbox->configs.status_offset + i * 4);

	aspeed_mbox_outb(mbox, ASPEED_MBOX_CTRL_RECV, mbox->configs.bmc_control_offset);
	return 0;
}

static const struct of_device_id aspeed_mbox_match[] = {
	{ .compatible = "aspeed,ast2400-mbox", .data = &ast2500_config },
	{ .compatible = "aspeed,ast2500-mbox", .data = &ast2500_config },
	{ .compatible = "aspeed,ast2600-mbox", .data = &ast2600_config },
	{ },
};
MODULE_DEVICE_TABLE(of, aspeed_mbox_match);

static int aspeed_mbox_probe(struct platform_device *pdev)
{
	const struct aspeed_mbox_config *config;
	const struct of_device_id *match;
	struct aspeed_mbox *mbox;
	struct device *dev;
	int rc;

	dev = &pdev->dev;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, mbox);

	match = of_match_node(aspeed_mbox_match, pdev->dev.of_node);
	if (!match)
		return -EINVAL;

	config = match->data;
	memcpy(&mbox->configs, config, sizeof(mbox->configs));

	rc = of_property_read_u32(dev->of_node, "reg", &mbox->base);
	if (rc) {
		dev_err(dev, "Couldn't read reg device-tree property\n");
		return rc;
	}

	mbox->regmap = syscon_node_to_regmap(
			pdev->dev.parent->of_node);
	if (IS_ERR(mbox->regmap)) {
		dev_err(dev, "Couldn't get regmap\n");
		return -ENODEV;
	}

	spin_lock_init(&mbox->lock);
	mutex_init(&mbox->mutex);
	init_waitqueue_head(&mbox->queue);

	mbox->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(mbox->clk)) {
		rc = PTR_ERR(mbox->clk);
		if (rc != -EPROBE_DEFER)
			dev_err(dev, "couldn't get clock\n");
		return rc;
	}
	rc = clk_prepare_enable(mbox->clk);
	if (rc) {
		dev_err(dev, "couldn't enable clock\n");
		return rc;
	}

	/* Create FIFO data structure */
	rc = kfifo_alloc(&mbox->fifo, mbox->configs.num_regs * sizeof(u32), GFP_KERNEL);
	if (rc)
		return rc;

	mbox->miscdev.minor = MISC_DYNAMIC_MINOR;
	mbox->miscdev.name = DEVICE_NAME;
	mbox->miscdev.fops = &aspeed_mbox_fops;
	mbox->miscdev.parent = dev;
	rc = misc_register(&mbox->miscdev);
	if (rc) {
		dev_err(dev, "Unable to register device\n");
		goto err;
	}

	rc = aspeed_mbox_config_irq(mbox, pdev);
	if (rc) {
		dev_err(dev, "Failed to configure IRQ\n");
		misc_deregister(&mbox->miscdev);
		goto err;
	}

	dev_info(&pdev->dev, "LPC mbox registered, irq %d\n", mbox->irq);

	return 0;

err:
	clk_disable_unprepare(mbox->clk);

	return rc;
}

static int aspeed_mbox_remove(struct platform_device *pdev)
{
	struct aspeed_mbox *mbox = dev_get_drvdata(&pdev->dev);

	misc_deregister(&mbox->miscdev);
	clk_disable_unprepare(mbox->clk);
	kfifo_free(&mbox->fifo);

	return 0;
}

static struct platform_driver aspeed_mbox_driver = {
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table = aspeed_mbox_match,
	},
	.probe = aspeed_mbox_probe,
	.remove = aspeed_mbox_remove,
};

module_platform_driver(aspeed_mbox_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cyril Bur <cyrilbur@gmail.com>");
MODULE_DESCRIPTION("Aspeed mailbox device driver");
