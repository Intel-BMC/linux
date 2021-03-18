/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/* Copyright (c) 2021 Intel Corporation */

struct aspeed_mbox_ioctl_data {
	unsigned int data;
};

#define ASPEED_MBOX_IOCTL_BASE 0xA3

#define ASPEED_MBOX_SIZE \
	_IOR(ASPEED_MBOX_IOCTL_BASE, 0, struct aspeed_mbox_ioctl_data)
