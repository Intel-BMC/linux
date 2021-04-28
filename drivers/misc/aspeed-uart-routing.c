/*
 * UART Routing driver for Aspeed AST2500
 *
 * Copyright (c) 2018 Google LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

/* The Aspeed AST2500 allows to dynamically route the inputs for the built-in
 * UARTS and physical serial I/O ports.
 *
 * This allows, for example, to connect the output of UART to another UART.
 * This can be used to enable host<->BMC communication via UARTs, e.g. to allow
 * access to the host's serial console.
 *
 * This driver is for the BMC side. The sysfs files allow the BMC userspace
 * which owns the system configuration policy, to configure how UARTs and
 * physical serial I/O ports are routed.
 */

#define ASPEED_HICRA_IO1	"io1"
#define ASPEED_HICRA_IO2	"io2"
#define ASPEED_HICRA_IO3	"io3"
#define ASPEED_HICRA_IO4	"io4"
#define ASPEED_HICRA_IO5	"io5"
#define ASPEED_HICRA_IO6	"io6"
#define ASPEED_HICRA_UART1	"uart1"
#define ASPEED_HICRA_UART2	"uart2"
#define ASPEED_HICRA_UART3	"uart3"
#define ASPEED_HICRA_UART4	"uart4"
#define ASPEED_HICRA_UART5	"uart5"
#define ASPEED_HICRA_RAW	"raw"

struct aspeed_uart_routing {
	struct device		*dev;
	void __iomem		*regs;
	spinlock_t		lock;
};

struct aspeed_uart_routing_selector {
	struct device_attribute	dev_attr;
	int				shift;
	int				mask;
	const char * const options[];
};

#define to_routing_selector(_dev_attr)					\
	container_of(_dev_attr, struct aspeed_uart_routing_selector, dev_attr)


static ssize_t aspeed_uart_routing_show(struct device *dev,
					struct device_attribute *attr,
					char *buf);

static ssize_t aspeed_uart_routing_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count);

#define ROUTING_ATTR(_name) {						\
	.attr = {.name = _name,					\
		 .mode = VERIFY_OCTAL_PERMISSIONS(S_IWUSR | S_IRUGO) }, \
	.show = aspeed_uart_routing_show,				\
	.store = aspeed_uart_routing_store,				\
}

static struct aspeed_uart_routing_selector raw_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_RAW),
	.shift = 0,
	.mask = 0xffffffff,
	.options = {
		    NULL                // NULL termination
		    },
};

static struct aspeed_uart_routing_selector uart5_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_UART5),
	.shift = 28,
	.mask = 0xf,
	.options = {
		    ASPEED_HICRA_IO5,   // 0
		    ASPEED_HICRA_IO1,   // 1
		    ASPEED_HICRA_IO2,   // 2
		    ASPEED_HICRA_IO3,   // 3
		    ASPEED_HICRA_IO4,   // 4
		    ASPEED_HICRA_UART1, // 5
		    ASPEED_HICRA_UART2, // 6
		    ASPEED_HICRA_UART3, // 7
		    ASPEED_HICRA_UART4, // 8
		    ASPEED_HICRA_IO6,   // 9
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector uart4_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_UART4),
	.shift = 25,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_IO4,   // 0
		    ASPEED_HICRA_IO1,   // 1
		    ASPEED_HICRA_IO2,   // 2
		    ASPEED_HICRA_IO3,   // 3
		    ASPEED_HICRA_UART1, // 4
		    ASPEED_HICRA_UART2, // 5
		    ASPEED_HICRA_UART3, // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
	},
};

static struct aspeed_uart_routing_selector uart3_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_UART3),
	.shift = 22,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_IO3,   // 0
		    ASPEED_HICRA_IO4,   // 1
		    ASPEED_HICRA_IO1,   // 2
		    ASPEED_HICRA_IO2,   // 3
		    ASPEED_HICRA_UART4, // 4
		    ASPEED_HICRA_UART1, // 5
		    ASPEED_HICRA_UART2, // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector uart2_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_UART2),
	.shift = 19,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_IO2,   // 0
		    ASPEED_HICRA_IO3,   // 1
		    ASPEED_HICRA_IO4,   // 2
		    ASPEED_HICRA_IO1,   // 3
		    ASPEED_HICRA_UART3, // 4
		    ASPEED_HICRA_UART4, // 5
		    ASPEED_HICRA_UART1, // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector uart1_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_UART1),
	.shift = 16,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_IO1,   // 0
		    ASPEED_HICRA_IO2,   // 1
		    ASPEED_HICRA_IO3,   // 2
		    ASPEED_HICRA_IO4,   // 3
		    ASPEED_HICRA_UART2, // 4
		    ASPEED_HICRA_UART3, // 5
		    ASPEED_HICRA_UART4, // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector io5_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_IO5),
	.shift = 12,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_UART5, // 0
		    ASPEED_HICRA_UART1, // 1
		    ASPEED_HICRA_UART2, // 2
		    ASPEED_HICRA_UART3, // 3
		    ASPEED_HICRA_UART4, // 4
		    ASPEED_HICRA_IO1,   // 5
		    ASPEED_HICRA_IO3,   // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector io4_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_IO4),
	.shift = 9,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_UART4, // 0
		    ASPEED_HICRA_UART5, // 1
		    ASPEED_HICRA_UART1, // 2
		    ASPEED_HICRA_UART2, // 3
		    ASPEED_HICRA_UART3, // 4
		    ASPEED_HICRA_IO1,   // 5
		    ASPEED_HICRA_IO2,   // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector io3_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_IO3),
	.shift = 6,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_UART3, // 0
		    ASPEED_HICRA_UART4, // 1
		    ASPEED_HICRA_UART5, // 2
		    ASPEED_HICRA_UART1, // 3
		    ASPEED_HICRA_UART2, // 4
		    ASPEED_HICRA_IO1,   // 5
		    ASPEED_HICRA_IO2,   // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector io2_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_IO2),
	.shift = 3,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_UART2, // 0
		    ASPEED_HICRA_UART3, // 1
		    ASPEED_HICRA_UART4, // 2
		    ASPEED_HICRA_UART5, // 3
		    ASPEED_HICRA_UART1, // 4
		    ASPEED_HICRA_IO3,   // 5
		    ASPEED_HICRA_IO4,   // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};

static struct aspeed_uart_routing_selector io1_sel = {
	.dev_attr = ROUTING_ATTR(ASPEED_HICRA_IO1),
	.shift = 0,
	.mask = 0x7,
	.options = {
		    ASPEED_HICRA_UART1, // 0
		    ASPEED_HICRA_UART2, // 1
		    ASPEED_HICRA_UART3, // 2
		    ASPEED_HICRA_UART4, // 3
		    ASPEED_HICRA_UART5, // 4
		    ASPEED_HICRA_IO3,   // 5
		    ASPEED_HICRA_IO4,   // 6
		    ASPEED_HICRA_IO6,   // 7
		    NULL,               // NULL termination
		    },
};


static struct attribute *aspeed_uart_routing_attrs[] = {
	&uart1_sel.dev_attr.attr,
	&uart2_sel.dev_attr.attr,
	&uart3_sel.dev_attr.attr,
	&uart4_sel.dev_attr.attr,
	&uart5_sel.dev_attr.attr,
	&io1_sel.dev_attr.attr,
	&io2_sel.dev_attr.attr,
	&io3_sel.dev_attr.attr,
	&io4_sel.dev_attr.attr,
	&io5_sel.dev_attr.attr,
	&raw_sel.dev_attr.attr,
	NULL,
};

static const struct attribute_group aspeed_uart_routing_attr_group = {
	.attrs = aspeed_uart_routing_attrs,
};

static ssize_t aspeed_uart_routing_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct aspeed_uart_routing *uart_routing = dev_get_drvdata(dev);
	struct aspeed_uart_routing_selector *sel = to_routing_selector(attr);
	int val, pos, len = 0;

	val = (readl(uart_routing->regs) >> sel->shift) & sel->mask;

	if (sel == &raw_sel) {
		len += snprintf(buf, PAGE_SIZE - 1, "0x%08x\n", val);
	} else {
		for (pos = 0; sel->options[pos] != NULL; ++pos) {
			if (pos == val) {
				len += snprintf(buf + len, PAGE_SIZE - 1 - len,
						"[%s] ", sel->options[pos]);
			} else {
				len += snprintf(buf + len, PAGE_SIZE - 1 - len,
						"%s ", sel->options[pos]);
			}
		}

		if (val >= pos) {
			len += snprintf(buf + len, PAGE_SIZE - 1 - len,
					"[unknown(%d)]", val);
		}

		len += snprintf(buf + len, PAGE_SIZE - 1 - len, "\n");
	}

	return len;
}

static ssize_t aspeed_uart_routing_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct aspeed_uart_routing *uart_routing = dev_get_drvdata(dev);
	struct aspeed_uart_routing_selector *sel = to_routing_selector(attr);
	int val, res;
	char end;
	u32 reg;

	if (sel == &raw_sel) {
		res = sscanf(buf, "%i%c", &val, &end);
		if (res < 1 || (res > 1 && end != '\n'))
			return -EINVAL;
	} else {
		val = match_string(sel->options, -1, buf);
		if (val < 0) {
			dev_err(dev, "invalid value \"%s\"\n", buf);
			return -EINVAL;
		}
	}

	spin_lock(&uart_routing->lock);
	reg = readl(uart_routing->regs);
	// Zero out existing value in specified bits.
	reg &= ~(sel->mask << sel->shift);
	// Set new value in specified bits.
	reg |= (val & sel->mask) << sel->shift;
	writel(reg, uart_routing->regs);
	spin_unlock(&uart_routing->lock);

	return count;
}

static int aspeed_uart_routing_probe(struct platform_device *pdev)
{
	struct aspeed_uart_routing *uart_routing;
	struct resource *res;
	int rc;

	uart_routing = devm_kzalloc(&pdev->dev,
				    sizeof(*uart_routing),
				    GFP_KERNEL);
	if (!uart_routing)
		return -ENOMEM;

	spin_lock_init(&uart_routing->lock);
	uart_routing->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	uart_routing->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(uart_routing->regs))
		return PTR_ERR(uart_routing->regs);

	rc = sysfs_create_group(&uart_routing->dev->kobj,
				&aspeed_uart_routing_attr_group);
	if (rc < 0)
		return rc;

	platform_set_drvdata(pdev, uart_routing);

	return 0;
}

static int aspeed_uart_routing_remove(struct platform_device *pdev)
{
	struct aspeed_uart_routing *uart_routing = platform_get_drvdata(pdev);

	sysfs_remove_group(&uart_routing->dev->kobj,
			   &aspeed_uart_routing_attr_group);

	return 0;
}

static const struct of_device_id aspeed_uart_routing_table[] = {
	{ .compatible = "aspeed,ast2500-uart-routing" },
	{ },
};

static struct platform_driver aspeed_uart_routing_driver = {
	.driver = {
		.name = "aspeed-uart-routing",
		.of_match_table = aspeed_uart_routing_table,
	},
	.probe = aspeed_uart_routing_probe,
	.remove = aspeed_uart_routing_remove,
};

module_platform_driver(aspeed_uart_routing_driver);

MODULE_AUTHOR("Oskar Senft <osk@google.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Driver to configure Aspeed UART routing");
