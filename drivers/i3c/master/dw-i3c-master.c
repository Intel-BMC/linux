// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 *
 * Author: Vitor Soares <vitor.soares@synopsys.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i3c/master.h>
#include <linux/i3c/target.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#define CCC_WORKAROUND
#define DEVICE_CTRL			0x0
#define DEV_CTRL_ENABLE			BIT(31)
#define DEV_CTRL_RESUME			BIT(30)
#define DEV_CTRL_HOT_JOIN_NACK		BIT(8)
#define DEV_CTRL_I2C_SLAVE_PRESENT	BIT(7)
#define DEV_CTRL_IBI_DATA_EN		BIT(1)

#define DEVICE_ADDR			0x4
#define DEV_ADDR_DYNAMIC_ADDR_VALID	BIT(31)
#define DEV_ADDR_DYNAMIC(x)		(((x) << 16) & GENMASK(22, 16))

#define HW_CAPABILITY			0x8
#define COMMAND_QUEUE_PORT		0xc
#define COMMAND_PORT_TOC		BIT(30)
#define COMMAND_PORT_READ_TRANSFER	BIT(28)
#define COMMAND_PORT_SDAP		BIT(27)
#define COMMAND_PORT_ROC		BIT(26)
#define COMMAND_PORT_SPEED(x)		(((x) << 21) & GENMASK(23, 21))
#define COMMAND_PORT_DEV_INDEX(x)	(((x) << 16) & GENMASK(20, 16))
#define COMMAND_PORT_CP			BIT(15)
#define COMMAND_PORT_CMD(x)		(((x) << 7) & GENMASK(14, 7))
#define COMMAND_PORT_TID(x)		(((x) << 3) & GENMASK(6, 3))

#define COMMAND_PORT_ARG_DATA_LEN(x)	(((x) << 16) & GENMASK(31, 16))
#define COMMAND_PORT_ARG_DATA_LEN_MAX	65536
#define COMMAND_PORT_TRANSFER_ARG	0x01

#define COMMAND_PORT_SDA_DATA_BYTE_3(x)	(((x) << 24) & GENMASK(31, 24))
#define COMMAND_PORT_SDA_DATA_BYTE_2(x)	(((x) << 16) & GENMASK(23, 16))
#define COMMAND_PORT_SDA_DATA_BYTE_1(x)	(((x) << 8) & GENMASK(15, 8))
#define COMMAND_PORT_SDA_BYTE_STRB_3	BIT(5)
#define COMMAND_PORT_SDA_BYTE_STRB_2	BIT(4)
#define COMMAND_PORT_SDA_BYTE_STRB_1	BIT(3)
#define COMMAND_PORT_SHORT_DATA_ARG	0x02

#define COMMAND_PORT_DEV_COUNT(x)	(((x) << 21) & GENMASK(25, 21))
#define COMMAND_PORT_ADDR_ASSGN_CMD	0x03

#define RESPONSE_QUEUE_PORT		0x10
#define RESPONSE_PORT_ERR_STATUS(x)	(((x) & GENMASK(31, 28)) >> 28)
#define RESPONSE_NO_ERROR		0
#define RESPONSE_ERROR_CRC		1
#define RESPONSE_ERROR_PARITY		2
#define RESPONSE_ERROR_FRAME		3
#define RESPONSE_ERROR_IBA_NACK		4
#define RESPONSE_ERROR_ADDRESS_NACK	5
#define RESPONSE_ERROR_OVER_UNDER_FLOW	6
#define RESPONSE_ERROR_TRANSF_ABORT	8
#define RESPONSE_ERROR_I2C_W_NACK_ERR	9
#define RESPONSE_PORT_TID(x)		(((x) & GENMASK(27, 24)) >> 24)
#define RESPONSE_PORT_DATA_LEN(x)	((x) & GENMASK(15, 0))

#define RX_TX_DATA_PORT			0x14
#define IBI_QUEUE_STATUS		0x18
#define IBI_QUEUE_STATUS_RSP_NACK	BIT(31)
#define IBI_QUEUE_STATUS_PEC_ERR	BIT(30)
#define IBI_QUEUE_STATUS_IBI_ID(x)	(((x) & GENMASK(15, 8)) >> 8)
#define IBI_QUEUE_STATUS_DATA_LEN(x)	((x) & GENMASK(7, 0))

#define IBI_QUEUE_IBI_ADDR(x)		(IBI_QUEUE_STATUS_IBI_ID(x) >> 1)
#define IBI_QUEUE_IBI_RNW(x)		(IBI_QUEUE_STATUS_IBI_ID(x) & BIT(0))
#define IBI_TYPE_SIR(x)                                                         \
	({ typeof(x) x_ = (x);							\
	(IBI_QUEUE_IBI_ADDR(x_) != I3C_HOT_JOIN_ADDR) && IBI_QUEUE_IBI_RNW(x_); })

#define IBI_QUEUE_DATA			0x18
#define IBI_QUEUE_DATA_STATUS_MASK	GENMASK(31, 28)
#define IBI_QUEUE_DATA_PAYLOAD_MASK	GENMASK(15, 8)
#define QUEUE_THLD_CTRL			0x1c
#define QUEUE_THLD_CTRL_IBI_STA_MASK	GENMASK(31, 24)
#define QUEUE_THLD_CTRL_IBI_STA(x)	(((x) - 1) << 24)
#define QUEUE_THLD_CTRL_IBI_DAT_MASK	GENMASK(23, 16)
#define QUEUE_THLD_CTRL_IBI_DAT(x)	((x) << 16)
#define QUEUE_THLD_CTRL_RESP_BUF_MASK	GENMASK(15, 8)
#define QUEUE_THLD_CTRL_RESP_BUF(x)	(((x) - 1) << 8)

#define DATA_BUFFER_THLD_CTRL		0x20
#define DATA_BUFFER_THLD_TX_START	GENMASK(18, 16)
#define DATA_BUFFER_THLD_CTRL_RX_BUF	GENMASK(10, 8)

#define IBI_QUEUE_CTRL			0x24
#define IBI_MR_REQ_REJECT		0x2C
#define IBI_SIR_REQ_REJECT		0x30
#define IBI_REQ_REJECT_ALL		GENMASK(31, 0)

#define RESET_CTRL			0x34
#define RESET_CTRL_IBI_QUEUE		BIT(5)
#define RESET_CTRL_RX_FIFO		BIT(4)
#define RESET_CTRL_TX_FIFO		BIT(3)
#define RESET_CTRL_RESP_QUEUE		BIT(2)
#define RESET_CTRL_CMD_QUEUE		BIT(1)
#define RESET_CTRL_SOFT			BIT(0)

#define SLV_EVENT_CTRL			0x38
#define SLV_EVENT_CTRL_SIR_EN		BIT(0)

#define INTR_STATUS			0x3c
#define INTR_STATUS_EN			0x40
#define INTR_SIGNAL_EN			0x44
#define INTR_FORCE			0x48
#define INTR_BUSOWNER_UPDATE_STAT	BIT(13)
#define INTR_IBI_UPDATED_STAT		BIT(12)
#define INTR_READ_REQ_RECV_STAT		BIT(11)
#define INTR_DEFSLV_STAT		BIT(10)
#define INTR_TRANSFER_ERR_STAT		BIT(9)
#define INTR_DYN_ADDR_ASSGN_STAT	BIT(8)
#define INTR_CCC_UPDATED_STAT		BIT(6)
#define INTR_TRANSFER_ABORT_STAT	BIT(5)
#define INTR_RESP_READY_STAT		BIT(4)
#define INTR_CMD_QUEUE_READY_STAT	BIT(3)
#define INTR_IBI_THLD_STAT		BIT(2)
#define INTR_RX_THLD_STAT		BIT(1)
#define INTR_TX_THLD_STAT		BIT(0)
#define INTR_ALL			(INTR_BUSOWNER_UPDATE_STAT |	\
					INTR_IBI_UPDATED_STAT |		\
					INTR_READ_REQ_RECV_STAT |	\
					INTR_DEFSLV_STAT |		\
					INTR_TRANSFER_ERR_STAT |	\
					INTR_DYN_ADDR_ASSGN_STAT |	\
					INTR_CCC_UPDATED_STAT |		\
					INTR_TRANSFER_ABORT_STAT |	\
					INTR_RESP_READY_STAT |		\
					INTR_CMD_QUEUE_READY_STAT |	\
					INTR_IBI_THLD_STAT |		\
					INTR_TX_THLD_STAT |		\
					INTR_RX_THLD_STAT)
#define INTR_MASTER_MASK		(INTR_TRANSFER_ERR_STAT |	\
					 INTR_RESP_READY_STAT)

#define INTR_TARGET_MASK		(INTR_READ_REQ_RECV_STAT |	\
					INTR_RESP_READY_STAT |		\
					INTR_IBI_UPDATED_STAT  |	\
					INTR_TRANSFER_ERR_STAT)

#define QUEUE_STATUS_LEVEL		0x4c
#define QUEUE_STATUS_IBI_STATUS_CNT(x)	(((x) & GENMASK(28, 24)) >> 24)
#define QUEUE_STATUS_IBI_BUF_BLR(x)	(((x) & GENMASK(23, 16)) >> 16)
#define QUEUE_STATUS_LEVEL_RESP(x)	(((x) & GENMASK(15, 8)) >> 8)
#define QUEUE_STATUS_LEVEL_CMD(x)	((x) & GENMASK(7, 0))

#define DATA_BUFFER_STATUS_LEVEL	0x50
#define DATA_BUFFER_STATUS_LEVEL_TX(x)	((x) & GENMASK(7, 0))

#define PRESENT_STATE			0x54
#define CCC_DEVICE_STATUS		0x58
#define DEVICE_ADDR_TABLE_POINTER	0x5c
#define DEVICE_ADDR_TABLE_DEPTH(x)	(((x) & GENMASK(31, 16)) >> 16)
#define DEVICE_ADDR_TABLE_ADDR(x)	((x) & GENMASK(7, 0))

#define DEV_CHAR_TABLE_POINTER		0x60
#define VENDOR_SPECIFIC_REG_POINTER	0x6c
#define SLV_MIPI_ID_VALUE		0x70
#define SLV_PID_VALUE			0x74
#define SLV_PID_HI(x)			(((x) >> 32) & GENMASK(15, 0))
#define SLV_PID_LO(x)			((x) & GENMASK(31, 0))
#define SLV_CHAR_CTRL			0x78
#define SLV_DCR_MASK			GENMASK(15, 8)
#define SLV_DCR(x)			(((x) << 8) & SLV_DCR_MASK)
#define SLV_DEVICE_ROLE_MASK		GENMASK(7, 6)
#define SLV_DEVICE_ROLE(x)		(((x) << 6) & SLV_DEVICE_ROLE_MASK)
#define SLV_HDR_CAPABLE			BIT(5)
#define SLV_MAX_DATA_SPEED_LIMIT	BIT(0)

#define SLV_MAX_LEN			0x7c
#define SLV_MAX_RD_LEN(x)		(((x) & GENMASK(31, 16)) >> 16)
#define SLV_MAX_WR_LEN(x)		((x) & GENMASK(15, 0))

#define MAX_READ_TURNAROUND		0x80
#define MAX_DATA_SPEED			0x84
#define SLV_DEBUG_STATUS		0x88
#define SLV_INTR_REQ			0x8c
#define SLV_INTR_REQ_IBI_STS(x)		(((x) & GENMASK(9, 8)) >> 8)
#define IBI_STS_ACCEPTED		0x01
#define IBI_STS_NOT_ATTEMPTED		0x11

#define DEVICE_CTRL_EXTENDED		0xb0
#define DEVICE_CTRL_EXTENDED_MODE_MASK	GENMASK(1, 0)
#define DEVICE_CTRL_EXTENDED_MODE(x)	((x) & DEVICE_CTRL_EXTENDED_MODE_MASK)
#define DEV_OPERATION_MODE_CONTROLLER	0x00
#define DEV_OPERATION_MODE_TARGET	0x01

#define SCL_I3C_OD_TIMING		0xb4
#define SCL_I3C_PP_TIMING		0xb8
#define SCL_I3C_TIMING_HCNT(x)		(((x) << 16) & GENMASK(23, 16))
#define SCL_I3C_TIMING_LCNT(x)		((x) & GENMASK(7, 0))
#define SCL_I3C_TIMING_CNT_MIN		5

#define SCL_I2C_FM_TIMING		0xbc
#define SCL_I2C_FM_TIMING_HCNT(x)	(((x) << 16) & GENMASK(31, 16))
#define SCL_I2C_FM_TIMING_LCNT(x)	((x) & GENMASK(15, 0))

#define SCL_I2C_FMP_TIMING		0xc0
#define SCL_I2C_FMP_TIMING_HCNT(x)	(((x) << 16) & GENMASK(23, 16))
#define SCL_I2C_FMP_TIMING_LCNT(x)	((x) & GENMASK(15, 0))

#define SCL_EXT_LCNT_TIMING		0xc8
#define SCL_EXT_LCNT_4(x)		(((x) << 24) & GENMASK(31, 24))
#define SCL_EXT_LCNT_3(x)		(((x) << 16) & GENMASK(23, 16))
#define SCL_EXT_LCNT_2(x)		(((x) << 8) & GENMASK(15, 8))
#define SCL_EXT_LCNT_1(x)		((x) & GENMASK(7, 0))

#define SCL_EXT_TERMN_LCNT_TIMING	0xcc

#define SDA_HOLD_SWITCH_DLY_TIMING	0xd0
#define SDA_TX_HOLD(x)			(((x) << 16) & GENMASK(18, 16))
#define SDA_TX_HOLD_MIN			1
#define SDA_TX_HOLD_MAX			7

#define BUS_FREE_TIMING			0xd4
#define BUS_AVAIL_TIME(x)		(((x) << 16) & GENMASK(31, 16))
#define MAX_BUS_AVAIL_CNT		0xffffU
#define BUS_I3C_MST_FREE(x)		((x) & GENMASK(15, 0))

#define BUS_IDLE_TIMING			0xd8
#define I3C_VER_ID			0xe0
#define I3C_VER_TYPE			0xe4
#define I3C_VER_RELEASE_TYPE(x)		(((x) & GENMASK(31, 16)) >> 16)
#define I3C_VER_RELEASE_VERSION(x)	((x) & GENMASK(15, 0))

#define I3C_LC_RELEASE			0x6c63

#define EXTENDED_CAPABILITY		0xe8
#define SLAVE_CONFIG			0xec

#define DEV_ADDR_TABLE_LEGACY_I2C_DEV	BIT(31)
#define DEV_ADDR_TABLE_DEV_NACK_RETRY(x) (((x) << 29) & GENMASK(30, 29))
#define DEV_ADDR_TABLE_MR_REJECT	BIT(14)
#define DEV_ADDR_TABLE_SIR_REJECT	BIT(13)
#define DEV_ADDR_TABLE_IBI_WITH_DATA	BIT(12)
#define DEV_ADDR_TABLE_DYNAMIC_ADDR(x)	(((x) << 16) & GENMASK(23, 16))
#define DEV_ADDR_TABLE_STATIC_ADDR(x)	((x) & GENMASK(6, 0))
#define DEV_ADDR_TABLE_LOC(start, idx)	((start) + ((idx) << 2))

#define MAX_DEVS 32

#define I3C_BUS_SDR1_SCL_RATE		8000000
#define I3C_BUS_SDR2_SCL_RATE		6000000
#define I3C_BUS_SDR3_SCL_RATE		4000000
#define I3C_BUS_SDR4_SCL_RATE		2000000
#define I3C_BUS_I2C_FM_TLOW_MIN_NS	1300
#define I3C_BUS_I2C_FM_THIGH_MIN_NS	600
#define I3C_BUS_I2C_FMP_TLOW_MIN_NS	500
#define I3C_BUS_I2C_FMP_THIGH_MIN_NS	260
#define I3C_BUS_I3C_OD_TLOW_MIN_NS	200
#define I3C_BUS_I3C_OD_THIGH_MAX_NS	41
#define I3C_BUS_I3C_PP_TLOW_MIN_NS	25
#define I3C_BUS_I3C_PP_THIGH_MIN_NS	25

#define XFER_TIMEOUT (msecs_to_jiffies(1000))

#define DW_I3C_TIMING_MIN 0x0
#define DW_I3C_TIMING_MAX 0xffffffff

struct dw_i3c_master_caps {
	u8 cmdfifodepth;
	u8 datafifodepth;
};

struct dw_i3c_cmd {
	u32 cmd_lo;
	u32 cmd_hi;
	u16 tx_len;
	const void *tx_buf;
	u16 rx_len;
	void *rx_buf;
	u8 error;
};

struct dw_i3c_xfer {
	struct list_head node;
	struct completion comp;
	int ret;
	unsigned int ncmds;
	struct dw_i3c_cmd cmds[];
};

struct dw_i3c_master {
	struct device *dev;
	struct i3c_master_controller base;
	u16 maxdevs;
	u16 datstartaddr;
	u32 free_pos;
	struct {
		struct list_head list;
		struct dw_i3c_xfer *cur;
		spinlock_t lock;
	} xferqueue;
	union {
		struct {
			struct i3c_dev_desc *slots[MAX_DEVS];
			/*
			 * Prevents simultaneous access to IBI related registers
			 * and slots array.
			 */
			spinlock_t lock;
		} master;
		struct {
			struct completion comp;
		} target;
	} ibi;
	struct dw_i3c_master_caps caps;
	void __iomem *regs;
	struct reset_control *core_rst;
	struct clk *core_clk;
	u32 ver_id;
	u16 ver_type;
	u8 addrs[MAX_DEVS];

	/* All parameters are expressed in nanoseconds */
	struct {
		u32 i3c_od_scl_freq;
		u32 i3c_od_scl_low;
		u32 i3c_od_scl_high;
		u32 i3c_pp_scl_freq;
		u32 i3c_pp_scl_low;
		u32 i3c_pp_scl_high;
		u32 sda_tx_hold;
	} timings;
	/* Used for handling private write */
	struct {
		void *buf;
		u16 max_len;
	} target_rx;
};

struct dw_i3c_i2c_dev_data {
	u8 index;
	s8 ibi;
	struct i3c_generic_ibi_pool *ibi_pool;
};

/*
 * All timing parameters are expressed in nanoseconds.
 * All frequency parameters are expressed in Hz
 */
struct dw_i3c_scl_timing {
	u32 high;
	u32 high_min;
	u32 high_max;
	u32 low;
	u32 low_min;
	u32 low_max;
	u32 freq;
	u32 freq_min;
	u32 freq_max;
};

static u8 even_parity(u8 p)
{
	p ^= p >> 4;
	p &= 0xf;

	return (0x9669 >> p) & 1;
}

static bool dw_i3c_master_supports_ccc_cmd(struct i3c_master_controller *m,
					   const struct i3c_ccc_cmd *cmd)
{
	if (cmd->ndests > 1)
		return false;

	switch (cmd->id) {
	case I3C_CCC_ENEC(true):
	case I3C_CCC_ENEC(false):
	case I3C_CCC_DISEC(true):
	case I3C_CCC_DISEC(false):
	case I3C_CCC_ENTAS(0, true):
	case I3C_CCC_ENTAS(0, false):
	case I3C_CCC_RSTDAA(true):
	case I3C_CCC_RSTDAA(false):
	case I3C_CCC_ENTDAA:
	case I3C_CCC_SETMWL(true):
	case I3C_CCC_SETMWL(false):
	case I3C_CCC_SETMRL(true):
	case I3C_CCC_SETMRL(false):
	case I3C_CCC_ENTHDR(0):
	case I3C_CCC_SETDASA:
	case I3C_CCC_SETNEWDA:
	case I3C_CCC_GETMWL:
	case I3C_CCC_GETMRL:
	case I3C_CCC_GETPID:
	case I3C_CCC_GETBCR:
	case I3C_CCC_GETDCR:
	case I3C_CCC_GETSTATUS:
	case I3C_CCC_GETMXDS:
	case I3C_CCC_GETHDRCAP:
	case I3C_CCC_SETAASA:
	case I3C_CCC_SETHID:
		return true;
	default:
		return false;
	}
}

static inline struct dw_i3c_master *
to_dw_i3c_master(struct i3c_master_controller *master)
{
	return container_of(master, struct dw_i3c_master, base);
}

static void dw_i3c_master_disable(struct dw_i3c_master *master)
{
	writel(readl(master->regs + DEVICE_CTRL) & ~DEV_CTRL_ENABLE,
	       master->regs + DEVICE_CTRL);
}

static void dw_i3c_master_enable(struct dw_i3c_master *master)
{
	writel(readl(master->regs + DEVICE_CTRL) | DEV_CTRL_ENABLE | DEV_CTRL_IBI_DATA_EN,
	       master->regs + DEVICE_CTRL);
}

static int dw_i3c_master_get_addr_pos(struct dw_i3c_master *master, u8 addr)
{
	int pos;

	for (pos = 0; pos < master->maxdevs; pos++) {
		if (addr == master->addrs[pos])
			return pos;
	}

	return -EINVAL;
}

static int dw_i3c_master_get_free_pos(struct dw_i3c_master *master)
{
	if (!(master->free_pos & GENMASK(master->maxdevs - 1, 0)))
		return -ENOSPC;

	return ffs(master->free_pos) - 1;
}

static void dw_i3c_master_wr_tx_fifo(struct dw_i3c_master *master,
				     const u8 *bytes, int nbytes)
{
	writesl(master->regs + RX_TX_DATA_PORT, bytes, nbytes / 4);
	if (nbytes & 3) {
		u32 tmp = 0;

		memcpy(&tmp, bytes + (nbytes & ~3), nbytes & 3);
		writesl(master->regs + RX_TX_DATA_PORT, &tmp, 1);
		dev_dbg(master->dev, "TX data = %08x\n", tmp);
	}
}

static void dw_i3c_master_read_fifo(struct dw_i3c_master *master, u32 fifo_reg,
				    u8 *bytes, int nbytes)
{
	readsl(master->regs + fifo_reg, bytes, nbytes / 4);
	if (nbytes & 3) {
		u32 tmp;

		readsl(master->regs + fifo_reg, &tmp, 1);
		memcpy(bytes + (nbytes & ~3), &tmp, nbytes & 3);
	}
}

static void dw_i3c_master_read_rx_fifo(struct dw_i3c_master *master,
				       u8 *bytes, int nbytes)
{
	dw_i3c_master_read_fifo(master, RX_TX_DATA_PORT, bytes, nbytes);
}

static void dw_i3c_master_read_ibi_fifo(struct dw_i3c_master *master,
					u8 *bytes, int nbytes)
{
	dw_i3c_master_read_fifo(master, IBI_QUEUE_DATA, bytes, nbytes);
}

static void dw_i3c_master_flush_ibi_fifo(struct dw_i3c_master *master, int nbytes)
{
	int nwords = (nbytes + 3) >> 2;
	int i;

	for (i = 0; i < nwords; i++)
		readl(master->regs + IBI_QUEUE_DATA);
}

static struct dw_i3c_xfer *
dw_i3c_master_alloc_xfer(struct dw_i3c_master *master, unsigned int ncmds)
{
	struct dw_i3c_xfer *xfer;

	xfer = kzalloc(struct_size(xfer, cmds, ncmds), GFP_KERNEL);
	if (!xfer)
		return NULL;

	INIT_LIST_HEAD(&xfer->node);
	xfer->ncmds = ncmds;
	xfer->ret = -ETIMEDOUT;

	return xfer;
}

static void dw_i3c_master_free_xfer(struct dw_i3c_xfer *xfer)
{
	kfree(xfer);
}

static void dw_i3c_master_start_xfer_locked(struct dw_i3c_master *master)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	unsigned int i;
	u32 thld_ctrl;

	if (!xfer)
		return;

	for (i = 0; i < xfer->ncmds; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		dw_i3c_master_wr_tx_fifo(master, cmd->tx_buf, cmd->tx_len);
	}

	thld_ctrl = readl(master->regs + QUEUE_THLD_CTRL);
	thld_ctrl &= ~QUEUE_THLD_CTRL_RESP_BUF_MASK;
	thld_ctrl |= QUEUE_THLD_CTRL_RESP_BUF(xfer->ncmds);
	writel(thld_ctrl, master->regs + QUEUE_THLD_CTRL);

	for (i = 0; i < xfer->ncmds; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		writel(cmd->cmd_hi, master->regs + COMMAND_QUEUE_PORT);
		writel(cmd->cmd_lo, master->regs + COMMAND_QUEUE_PORT);
	}
}

static void dw_i3c_master_enqueue_xfer(struct dw_i3c_master *master,
				       struct dw_i3c_xfer *xfer)
{
	unsigned long flags;

	init_completion(&xfer->comp);
	spin_lock_irqsave(&master->xferqueue.lock, flags);
	if (master->xferqueue.cur) {
		list_add_tail(&xfer->node, &master->xferqueue.list);
	} else {
		master->xferqueue.cur = xfer;
		dw_i3c_master_start_xfer_locked(master);
	}
	spin_unlock_irqrestore(&master->xferqueue.lock, flags);
}

static void dw_i3c_master_dequeue_xfer_locked(struct dw_i3c_master *master,
					      struct dw_i3c_xfer *xfer)
{
	if (master->xferqueue.cur == xfer) {
		u32 status;

		master->xferqueue.cur = NULL;

		writel(RESET_CTRL_RX_FIFO | RESET_CTRL_TX_FIFO |
		       RESET_CTRL_RESP_QUEUE | RESET_CTRL_CMD_QUEUE,
		       master->regs + RESET_CTRL);

		readl_poll_timeout_atomic(master->regs + RESET_CTRL, status,
					  !status, 10, 1000000);
	} else {
		list_del_init(&xfer->node);
	}
}

static void dw_i3c_master_dequeue_xfer(struct dw_i3c_master *master,
				       struct dw_i3c_xfer *xfer)
{
	unsigned long flags;

	spin_lock_irqsave(&master->xferqueue.lock, flags);
	dw_i3c_master_dequeue_xfer_locked(master, xfer);
	spin_unlock_irqrestore(&master->xferqueue.lock, flags);
}

static void dw_i3c_master_end_xfer_locked(struct dw_i3c_master *master, u32 isr)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	int i, ret = 0;
	u32 nresp;

	if (!xfer)
		return;

	nresp = readl(master->regs + QUEUE_STATUS_LEVEL);
	nresp = QUEUE_STATUS_LEVEL_RESP(nresp);

	for (i = 0; i < nresp; i++) {
		struct dw_i3c_cmd *cmd;
		u32 resp;

		resp = readl(master->regs + RESPONSE_QUEUE_PORT);

		cmd = &xfer->cmds[RESPONSE_PORT_TID(resp)];
		cmd->rx_len = RESPONSE_PORT_DATA_LEN(resp);
		cmd->error = RESPONSE_PORT_ERR_STATUS(resp);
		if (cmd->rx_len && !cmd->error)
			dw_i3c_master_read_rx_fifo(master, cmd->rx_buf,
						   cmd->rx_len);
	}

	for (i = 0; i < nresp; i++) {
		switch (xfer->cmds[i].error) {
		case RESPONSE_NO_ERROR:
			break;
		case RESPONSE_ERROR_PARITY:
		case RESPONSE_ERROR_IBA_NACK:
		case RESPONSE_ERROR_TRANSF_ABORT:
		case RESPONSE_ERROR_CRC:
		case RESPONSE_ERROR_FRAME:
			ret = -EIO;
			break;
		case RESPONSE_ERROR_OVER_UNDER_FLOW:
			ret = -ENOSPC;
			break;
		case RESPONSE_ERROR_I2C_W_NACK_ERR:
		case RESPONSE_ERROR_ADDRESS_NACK:
		default:
			ret = -EINVAL;
			break;
		}
	}

	xfer->ret = ret;
	complete(&xfer->comp);

	if (ret < 0) {
		dw_i3c_master_dequeue_xfer_locked(master, xfer);
		writel(readl(master->regs + DEVICE_CTRL) | DEV_CTRL_RESUME,
		       master->regs + DEVICE_CTRL);
	}

	xfer = list_first_entry_or_null(&master->xferqueue.list,
					struct dw_i3c_xfer,
					node);
	if (xfer)
		list_del_init(&xfer->node);

	master->xferqueue.cur = xfer;
	dw_i3c_master_start_xfer_locked(master);
}

static void _timing_calc_when_no_params(struct dw_i3c_scl_timing *timings, u32 *scl_high,
					u32 *scl_low, u32 *scl_period_ns)
{
	u32 high, low, period;

	period = DIV_ROUND_CLOSEST(1000000000, timings->freq_max);
	high = clamp(period / 2, timings->high_min, timings->high_max);
	low = timings->low_min;

	if (period > high) {
		u32 delta = period - high;

		if (delta > timings->low_max)
			low = timings->low_max;
		else if (delta >= timings->low_min)
			low = delta;
	}

	*scl_high = high;
	*scl_low = low;
	*scl_period_ns = high + low;
}

static int _timing_calc_when_scl_high(struct dw_i3c_scl_timing *timings, u32 *scl_high,
				      u32 *scl_low, u32 *scl_period_ns)
{
	u32 high, low, period;

	high = timings->high;
	low = timings->low_min;
	period = DIV_ROUND_CLOSEST(1000000000, timings->freq_max);

	if (period > high) {
		u32 delta = period - high;

		if (delta > timings->low_max)
			low = timings->low_max;
		else if (delta >= timings->low_min)
			low = delta;
	}

	*scl_high = high;
	*scl_low = low;
	*scl_period_ns = high + low;

	return 0;
}

static int _timing_calc_when_scl_low(struct dw_i3c_scl_timing *timings, u32 *scl_high,
				     u32 *scl_low, u32 *scl_period_ns)
{
	u32 high, low, period;

	low = timings->low;
	high = timings->high_min;
	period = DIV_ROUND_CLOSEST(1000000000, timings->freq_max);

	if (period > low) {
		u32 delta = period - low;

		if (delta > timings->high_max)
			high = timings->high_max;
		else if (delta >= timings->high_min)
			high = delta;
	}

	*scl_high = high;
	*scl_low = low;
	*scl_period_ns = high + low;

	return 0;
}

static int _timing_calc_when_scl_freq(struct dw_i3c_scl_timing *timings, u32 *scl_high,
				      u32 *scl_low, u32 *scl_period_ns)
{
	u32 high, period;

	period = DIV_ROUND_CLOSEST(1000000000, timings->freq);
	high = clamp(period / 2, timings->high_min, timings->high_max);
	if (period <= high)
		return -EINVAL;

	*scl_high = high;
	*scl_low = period - high;
	*scl_period_ns = period;

	return 0;
}

static int _timing_calc_when_scl_high_low(struct dw_i3c_scl_timing *timings, u32 *scl_high,
					  u32 *scl_low, u32 *scl_period_ns)
{
	*scl_high = timings->high;
	*scl_low = timings->low;
	*scl_period_ns = *scl_high + *scl_low;

	return 0;
}

static int _timing_calc_when_scl_high_freq(struct dw_i3c_scl_timing *timings, u32 *scl_high,
					   u32 *scl_low, u32 *scl_period_ns)
{
	*scl_period_ns = DIV_ROUND_CLOSEST(1000000000, timings->freq);
	*scl_high = timings->high;
	if (*scl_period_ns <= *scl_high)
		return -EINVAL;

	*scl_low = *scl_period_ns - *scl_high;

	return 0;
}

static int _timing_calc_when_scl_low_freq(struct dw_i3c_scl_timing *timings, u32 *scl_high,
					  u32 *scl_low, u32 *scl_period_ns)
{
	*scl_period_ns = DIV_ROUND_CLOSEST(1000000000, timings->freq);
	*scl_low = timings->low;
	if (*scl_period_ns <= *scl_low)
		return -EINVAL;

	*scl_high = *scl_period_ns - *scl_low;

	return 0;
}

static int _timing_calc_when_all(struct dw_i3c_scl_timing *timings, u32 *scl_high,
				 u32 *scl_low, u32 *scl_period_ns)
{
	*scl_period_ns = DIV_ROUND_CLOSEST(1000000000, timings->freq);
	*scl_high = timings->high;
	*scl_low = timings->low;

	return 0;
}

static int dw_i3c_timing_calc(struct dw_i3c_scl_timing *timings, u32 *scl_high, u32 *scl_low)
{
	u32 high = timings->high;
	u32 low = timings->low;
	u32 freq = timings->freq;
	u32 period;
	int ret = 0;

	if ((high > 0 && (high < timings->high_min || high > timings->high_max)) ||
	    (low > 0 && (low < timings->low_min || low > timings->low_max)) ||
	    (freq > 0 && (freq < timings->freq_min || freq > timings->freq_max)))
		return -EINVAL;

	if (high == 0 && low == 0 && freq == 0)
		_timing_calc_when_no_params(timings, &high, &low, &period);
	else if (high > 0 && low == 0 && freq == 0)
		ret = _timing_calc_when_scl_high(timings, &high, &low, &period);
	else if (high == 0 && low > 0 && freq == 0)
		ret = _timing_calc_when_scl_low(timings, &high, &low, &period);
	else if (high == 0 && low == 0 && freq > 0)
		ret = _timing_calc_when_scl_freq(timings, &high, &low, &period);
	else if (high > 0 && low > 0 && freq == 0)
		ret = _timing_calc_when_scl_high_low(timings, &high, &low, &period);
	else if (high > 0 && low == 0 && freq > 0)
		ret = _timing_calc_when_scl_high_freq(timings, &high, &low, &period);
	else if (high == 0 && low > 0 && freq > 0)
		ret = _timing_calc_when_scl_low_freq(timings, &high, &low, &period);
	else
		ret = _timing_calc_when_all(timings, &high, &low, &period);

	if (ret)
		return ret;

	if (high < timings->high_min || high > timings->high_max ||
	    low < timings->low_min || low > timings->low_max)
		return -EINVAL;

	freq = DIV_ROUND_CLOSEST(1000000000, period);
	if (freq < timings->freq_min || freq > timings->freq_max)
		return -EINVAL;

	if ((high + low) != period)
		return -EINVAL;

	*scl_high = high;
	*scl_low = low;

	return 0;
}

static void dw_i3c_timing_calc_cnt(u32 core_rate_hz, u32 high, u32 low, u8 *hcnt, u8 *lcnt)
{
	u32 hcnt_tmp, lcnt_tmp;
	u32 core_period_ns;

	core_period_ns = DIV_ROUND_CLOSEST(1000000000, core_rate_hz);
	hcnt_tmp = DIV_ROUND_CLOSEST(high, core_period_ns);
	lcnt_tmp = DIV_ROUND_CLOSEST(low, core_period_ns);

	if (hcnt_tmp < SCL_I3C_TIMING_CNT_MIN)
		*hcnt = SCL_I3C_TIMING_CNT_MIN;
	else if (hcnt_tmp > 0xFF)
		*hcnt = 0xFF;
	else
		*hcnt = (u8)hcnt_tmp;

	if (lcnt_tmp < SCL_I3C_TIMING_CNT_MIN)
		*lcnt = SCL_I3C_TIMING_CNT_MIN;
	else if (lcnt_tmp > 0xFF)
		*lcnt = 0xFF;
	else
		*lcnt = (u8)lcnt_tmp;
}

static int dw_i3c_clk_cfg(struct dw_i3c_master *master)
{
	unsigned long core_rate, core_period;
	struct dw_i3c_scl_timing timings;
	u32 high, low;
	u32 scl_timing;
	u8 hcnt, lcnt;
	int ret;

	core_rate = clk_get_rate(master->core_clk);
	if (!core_rate)
		return -EINVAL;

	core_period = DIV_ROUND_UP(1000000000, core_rate);

	/* Open-drain clock configuration */
	timings.high = master->timings.i3c_od_scl_high;
	timings.high_min = I3C_BUS_I3C_PP_THIGH_MIN_NS;
	timings.high_max = DW_I3C_TIMING_MAX;
	timings.low = master->timings.i3c_od_scl_low;
	timings.low_min = I3C_BUS_I3C_OD_TLOW_MIN_NS;
	timings.low_max = DW_I3C_TIMING_MAX;
	timings.freq = master->timings.i3c_od_scl_freq;
	timings.freq_min = DW_I3C_TIMING_MIN;
	timings.freq_max = I3C_BUS_TYP_I3C_SCL_RATE;
	ret = dw_i3c_timing_calc(&timings, &high, &low);
	if (ret)
		return ret;

	dw_i3c_timing_calc_cnt(core_rate, high, low, &hcnt,
			       &lcnt);
	scl_timing = SCL_I3C_TIMING_HCNT(hcnt) | SCL_I3C_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I3C_OD_TIMING);

	/* SDR0 (push-pull) clock configuration */
	timings.high = master->timings.i3c_pp_scl_high;
	timings.high_min = I3C_BUS_I3C_PP_THIGH_MIN_NS;
	timings.high_max = DW_I3C_TIMING_MAX;
	timings.low = master->timings.i3c_pp_scl_low;
	timings.low_min = I3C_BUS_I3C_PP_TLOW_MIN_NS;
	timings.low_max = DW_I3C_TIMING_MAX;
	timings.freq = master->timings.i3c_pp_scl_freq;
	timings.freq_min = DW_I3C_TIMING_MIN;
	timings.freq_max = I3C_BUS_TYP_I3C_SCL_RATE;
	ret = dw_i3c_timing_calc(&timings, &high, &low);
	if (ret)
		return ret;

	dw_i3c_timing_calc_cnt(core_rate, high, low, &hcnt,
			       &lcnt);
	scl_timing = SCL_I3C_TIMING_HCNT(hcnt) | SCL_I3C_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I3C_PP_TIMING);

	if (!(readl(master->regs + DEVICE_CTRL) & DEV_CTRL_I2C_SLAVE_PRESENT))
		writel(BUS_I3C_MST_FREE(lcnt), master->regs + BUS_FREE_TIMING);

	/* SDR1, SDR2, SDR3, SDR4 (push-pull) clocks configuration */
	hcnt = DIV_ROUND_UP(I3C_BUS_I3C_OD_THIGH_MAX_NS, core_period) - 1;
	if (hcnt < SCL_I3C_TIMING_CNT_MIN)
		hcnt = SCL_I3C_TIMING_CNT_MIN;

	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR1_SCL_RATE) - hcnt;
	scl_timing = SCL_EXT_LCNT_1(lcnt);
	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR2_SCL_RATE) - hcnt;
	scl_timing |= SCL_EXT_LCNT_2(lcnt);
	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR3_SCL_RATE) - hcnt;
	scl_timing |= SCL_EXT_LCNT_3(lcnt);
	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR4_SCL_RATE) - hcnt;
	scl_timing |= SCL_EXT_LCNT_4(lcnt);
	writel(scl_timing, master->regs + SCL_EXT_LCNT_TIMING);

	return 0;
}

static int dw_i2c_clk_cfg(struct dw_i3c_master *master)
{
	unsigned long core_rate, core_period;
	u16 hcnt, lcnt;
	u32 scl_timing;

	core_rate = clk_get_rate(master->core_clk);
	if (!core_rate)
		return -EINVAL;

	core_period = DIV_ROUND_UP(1000000000, core_rate);

	lcnt = DIV_ROUND_UP(I3C_BUS_I2C_FMP_TLOW_MIN_NS, core_period);
	hcnt = DIV_ROUND_UP(core_rate, I3C_BUS_I2C_FM_PLUS_SCL_RATE) - lcnt;
	scl_timing = SCL_I2C_FMP_TIMING_HCNT(hcnt) |
		     SCL_I2C_FMP_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I2C_FMP_TIMING);

	lcnt = DIV_ROUND_UP(I3C_BUS_I2C_FM_TLOW_MIN_NS, core_period);
	hcnt = DIV_ROUND_UP(core_rate, I3C_BUS_I2C_FM_SCL_RATE) - lcnt;
	scl_timing = SCL_I2C_FM_TIMING_HCNT(hcnt) |
		     SCL_I2C_FM_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I2C_FM_TIMING);

	writel(BUS_I3C_MST_FREE(lcnt), master->regs + BUS_FREE_TIMING);
	writel(readl(master->regs + DEVICE_CTRL) | DEV_CTRL_I2C_SLAVE_PRESENT,
	       master->regs + DEVICE_CTRL);

	return 0;
}

static int dw_sda_tx_hold_cfg(struct dw_i3c_master *master)
{
	unsigned long core_rate, core_period;
	u8 sda_tx_hold;

	/* Do not modify register if there is no DT configuration or 0 was provied */
	if (!master->timings.sda_tx_hold)
		return 0;

	core_rate = clk_get_rate(master->core_clk);
	if (!core_rate)
		return -EINVAL;

	core_period = DIV_ROUND_UP(1000000000, core_rate);
	sda_tx_hold = clamp((u32)DIV_ROUND_CLOSEST(master->timings.sda_tx_hold, core_period),
			    (u32)SDA_TX_HOLD_MIN, (u32)SDA_TX_HOLD_MAX);
	writel(SDA_TX_HOLD(sda_tx_hold), master->regs + SDA_HOLD_SWITCH_DLY_TIMING);

	return 0;
}

static int dw_i3c_bus_clk_cfg(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct i3c_bus *bus = i3c_master_get_bus(m);
	int ret;

	switch (bus->mode) {
	case I3C_BUS_MODE_MIXED_FAST:
	case I3C_BUS_MODE_MIXED_LIMITED:
		ret = dw_i2c_clk_cfg(master);
		if (ret)
			return ret;
		fallthrough;
	case I3C_BUS_MODE_PURE:
		ret = dw_i3c_clk_cfg(master);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int dw_i3c_target_bus_init(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct i3c_dev_desc *desc = master->base.this;
	void *rx_buf;
	u32 reg;
	int ret;

	ret = dw_i3c_bus_clk_cfg(m);
	if (ret)
		return ret;

	reg = readl(master->regs + SLV_MAX_LEN);
	/*
	 * Set max private write length value based on read-only register.
	 * TODO: Handle updates after receiving SETMWL CCC.
	 */
	master->target_rx.max_len = SLV_MAX_WR_LEN(reg);

	rx_buf = kzalloc(master->target_rx.max_len, GFP_KERNEL);
	if (!rx_buf)
		return -ENOMEM;

	master->target_rx.buf = rx_buf;

	dw_i3c_master_disable(master);

	reg = readl(master->regs + QUEUE_THLD_CTRL) & ~QUEUE_THLD_CTRL_RESP_BUF_MASK;
	writel(reg, master->regs + QUEUE_THLD_CTRL);

	reg = readl(master->regs + DATA_BUFFER_THLD_CTRL) & ~DATA_BUFFER_THLD_CTRL_RX_BUF;
	writel(reg, master->regs + DATA_BUFFER_THLD_CTRL);

	writel(INTR_ALL, master->regs + INTR_STATUS);
	writel(INTR_TARGET_MASK, master->regs + INTR_STATUS_EN);
	writel(INTR_TARGET_MASK, master->regs + INTR_SIGNAL_EN);

	reg = readl(master->regs + DEVICE_CTRL_EXTENDED) & ~DEVICE_CTRL_EXTENDED_MODE_MASK;
	reg |= DEVICE_CTRL_EXTENDED_MODE(DEV_OPERATION_MODE_TARGET);
	writel(reg, master->regs + DEVICE_CTRL_EXTENDED);

	writel(SLV_PID_LO(desc->info.pid), master->regs + SLV_PID_VALUE);
	writel(SLV_PID_HI(desc->info.pid), master->regs + SLV_MIPI_ID_VALUE);

	reg = readl(master->regs + SLV_CHAR_CTRL) & ~SLV_DCR_MASK & ~SLV_DEVICE_ROLE_MASK;
	reg |= SLV_DCR(desc->info.dcr) | SLV_DEVICE_ROLE(0);
	writel(reg, master->regs + SLV_CHAR_CTRL);

	reg = readl(master->regs + BUS_FREE_TIMING) | BUS_AVAIL_TIME(MAX_BUS_AVAIL_CNT);
	writel(reg, master->regs + BUS_FREE_TIMING);

	dw_i3c_master_enable(master);

	return 0;
}

static void dw_i3c_target_bus_cleanup(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);

	dw_i3c_master_disable(master);
	kfree(master->target_rx.buf);
}

static int dw_i3c_master_bus_init(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	u32 interrupt_mask = INTR_MASTER_MASK;
	struct i3c_device_info info = { };
	u32 thld_ctrl;
	int ret;

	spin_lock_init(&master->ibi.master.lock);

	ret = dw_i3c_bus_clk_cfg(m);
	if (ret)
		return ret;

	ret = dw_sda_tx_hold_cfg(master);
	if (ret)
		return ret;

	thld_ctrl = readl(master->regs + QUEUE_THLD_CTRL);
	thld_ctrl &= ~QUEUE_THLD_CTRL_RESP_BUF_MASK;
	writel(thld_ctrl, master->regs + QUEUE_THLD_CTRL);

	thld_ctrl = readl(master->regs + DATA_BUFFER_THLD_CTRL);
	thld_ctrl &= ~DATA_BUFFER_THLD_CTRL_RX_BUF;
	writel(thld_ctrl, master->regs + DATA_BUFFER_THLD_CTRL);

	if (master->ver_type >= I3C_LC_RELEASE) {
		thld_ctrl = readl(master->regs + QUEUE_THLD_CTRL);
		thld_ctrl &= ~(QUEUE_THLD_CTRL_IBI_STA_MASK | QUEUE_THLD_CTRL_IBI_DAT_MASK);
		thld_ctrl |= QUEUE_THLD_CTRL_IBI_STA(1) | QUEUE_THLD_CTRL_IBI_DAT(1);
		writel(thld_ctrl, master->regs + QUEUE_THLD_CTRL);
		interrupt_mask |= INTR_IBI_THLD_STAT;
	}

	writel(INTR_ALL, master->regs + INTR_STATUS);
	writel(interrupt_mask, master->regs + INTR_STATUS_EN);
	writel(interrupt_mask, master->regs + INTR_SIGNAL_EN);


	ret = i3c_master_get_free_addr(m, 0);
	if (ret < 0)
		return ret;

	writel(DEV_ADDR_DYNAMIC_ADDR_VALID | DEV_ADDR_DYNAMIC(ret),
	       master->regs + DEVICE_ADDR);

	memset(&info, 0, sizeof(info));
	info.dyn_addr = ret;

	ret = i3c_master_set_info(&master->base, &info);
	if (ret)
		return ret;

	writel(IBI_REQ_REJECT_ALL, master->regs + IBI_MR_REQ_REJECT);

	/* For now don't support Hot-Join */
	writel(readl(master->regs + DEVICE_CTRL) | DEV_CTRL_HOT_JOIN_NACK,
	       master->regs + DEVICE_CTRL);

	dw_i3c_master_enable(master);

	return 0;
}

static void dw_i3c_master_bus_cleanup(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);

	dw_i3c_master_disable(master);
}

static int dw_i3c_ccc_set(struct dw_i3c_master *master,
			  struct i3c_ccc_cmd *ccc)
{
	struct dw_i3c_xfer *xfer;
	struct dw_i3c_cmd *cmd;
	int ret, pos = 0;

	if (ccc->id & I3C_CCC_DIRECT) {
		pos = dw_i3c_master_get_addr_pos(master, ccc->dests[0].addr);
		if (pos < 0)
			return pos;
	}

	xfer = dw_i3c_master_alloc_xfer(master, 1);
	if (!xfer)
		return -ENOMEM;

	cmd = xfer->cmds;
	cmd->tx_buf = ccc->dests[0].payload.data;
	cmd->tx_len = ccc->dests[0].payload.len;

	cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(ccc->dests[0].payload.len) |
		      COMMAND_PORT_TRANSFER_ARG;

	cmd->cmd_lo = COMMAND_PORT_CP |
		      COMMAND_PORT_DEV_INDEX(pos) |
		      COMMAND_PORT_CMD(ccc->id) |
		      COMMAND_PORT_TOC |
		      COMMAND_PORT_ROC;

	dev_dbg(master->dev, "%s:cmd_hi=0x%08x cmd_lo=0x%08x tx_len=%d id=%x\n",
		__func__, cmd->cmd_hi, cmd->cmd_lo, cmd->tx_len, ccc->id);

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	if (xfer->cmds[0].error == RESPONSE_ERROR_IBA_NACK)
		ccc->err = I3C_ERROR_M2;

	dw_i3c_master_free_xfer(xfer);

	return ret;
}

static int dw_i3c_ccc_get(struct dw_i3c_master *master, struct i3c_ccc_cmd *ccc)
{
	struct dw_i3c_xfer *xfer;
	struct dw_i3c_cmd *cmd;
	int ret, pos;

	pos = dw_i3c_master_get_addr_pos(master, ccc->dests[0].addr);
	if (pos < 0)
		return pos;

	xfer = dw_i3c_master_alloc_xfer(master, 1);
	if (!xfer)
		return -ENOMEM;

	cmd = xfer->cmds;
	cmd->rx_buf = ccc->dests[0].payload.data;
	cmd->rx_len = ccc->dests[0].payload.len;

	cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(ccc->dests[0].payload.len) |
		      COMMAND_PORT_TRANSFER_ARG;

	cmd->cmd_lo = COMMAND_PORT_READ_TRANSFER |
		      COMMAND_PORT_CP |
		      COMMAND_PORT_DEV_INDEX(pos) |
		      COMMAND_PORT_CMD(ccc->id) |
		      COMMAND_PORT_TOC |
		      COMMAND_PORT_ROC;

	dev_dbg(master->dev, "%s:cmd_hi=0x%08x cmd_lo=0x%08x rx_len=%d id=%x\n",
		__func__, cmd->cmd_hi, cmd->cmd_lo, cmd->rx_len, ccc->id);

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	if (xfer->cmds[0].error == RESPONSE_ERROR_IBA_NACK)
		ccc->err = I3C_ERROR_M2;
	dw_i3c_master_free_xfer(xfer);

	return ret;
}

static int dw_i3c_master_send_ccc_cmd(struct i3c_master_controller *m,
				      struct i3c_ccc_cmd *ccc)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	int ret = 0;
	u32 i3c_pp_timing, i3c_od_timing;

	if (ccc->id == I3C_CCC_ENTDAA)
		return -EINVAL;

	i3c_od_timing = readl(master->regs + SCL_I3C_OD_TIMING);
	i3c_pp_timing = readl(master->regs + SCL_I3C_PP_TIMING);
	if ((ccc->id == I3C_CCC_SETAASA) || (ccc->id == I3C_CCC_SETHID) ||
	    (ccc->id == I3C_CCC_DEVCTRL)) {
		writel(i3c_od_timing, master->regs + SCL_I3C_PP_TIMING);
	}

	if (ccc->rnw)
		ret = dw_i3c_ccc_get(master, ccc);
	else
		ret = dw_i3c_ccc_set(master, ccc);

	if ((ccc->id == I3C_CCC_SETAASA) || (ccc->id == I3C_CCC_SETHID))
		writel(i3c_pp_timing, master->regs + SCL_I3C_PP_TIMING);

	return ret;
}

static int dw_i3c_master_daa(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_xfer *xfer;
	struct dw_i3c_cmd *cmd;
	u32 olddevs, newdevs;
	u8 p, last_addr = 0;
	int ret, pos;

	olddevs = ~(master->free_pos);

	/* Prepare DAT before launching DAA. */
	for (pos = 0; pos < master->maxdevs; pos++) {
		if (olddevs & BIT(pos))
			continue;

		ret = i3c_master_get_free_addr(m, last_addr + 1);
		if (ret < 0)
			return -ENOSPC;

		master->addrs[pos] = ret;
		p = even_parity(ret);
		last_addr = ret;
		ret |= (p << 7);

		writel(DEV_ADDR_TABLE_DYNAMIC_ADDR(ret),
		       master->regs +
		       DEV_ADDR_TABLE_LOC(master->datstartaddr, pos));
	}

	xfer = dw_i3c_master_alloc_xfer(master, 1);
	if (!xfer)
		return -ENOMEM;

	pos = dw_i3c_master_get_free_pos(master);
	cmd = &xfer->cmds[0];
	cmd->cmd_hi = 0x1;
	cmd->cmd_lo = COMMAND_PORT_DEV_COUNT(master->maxdevs - pos) |
		      COMMAND_PORT_DEV_INDEX(pos) |
		      COMMAND_PORT_CMD(I3C_CCC_ENTDAA) |
		      COMMAND_PORT_ADDR_ASSGN_CMD |
		      COMMAND_PORT_TOC |
		      COMMAND_PORT_ROC;

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	newdevs = GENMASK(master->maxdevs - cmd->rx_len - 1, 0);
	newdevs &= ~olddevs;

	for (pos = 0; pos < master->maxdevs; pos++) {
		if (newdevs & BIT(pos))
			i3c_master_add_i3c_dev_locked(m, master->addrs[pos]);
	}

	dw_i3c_master_free_xfer(xfer);

	return 0;
}
#ifdef CCC_WORKAROUND
/*
 * Provide an interface for sending CCC from userspace.  Especially for the
 * transfers with PEC and direct CCC.
 */
static int dw_i3c_master_ccc_xfers(struct i3c_dev_desc *dev,
				    struct i3c_priv_xfer *i3c_xfers,
				    int i3c_nxfers)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_xfer *xfer;
	int i, ret = 0;
	struct dw_i3c_cmd *cmd_ccc;

	xfer = dw_i3c_master_alloc_xfer(master, i3c_nxfers);
	if (!xfer)
		return -ENOMEM;

	/* i3c_xfers[0] handles the CCC data */
	cmd_ccc = &xfer->cmds[0];
	cmd_ccc->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(i3c_xfers[0].len - 1) |
			  COMMAND_PORT_TRANSFER_ARG;
	cmd_ccc->tx_buf = i3c_xfers[0].data.out + 1;
	cmd_ccc->tx_len = i3c_xfers[0].len - 1;
	cmd_ccc->cmd_lo = COMMAND_PORT_SPEED(dev->info.max_write_ds);
	cmd_ccc->cmd_lo |= COMMAND_PORT_TID(0) |
			   COMMAND_PORT_DEV_INDEX(master->maxdevs - 1) |
			   COMMAND_PORT_ROC;
	if (i3c_nxfers == 1)
		cmd_ccc->cmd_lo |= COMMAND_PORT_TOC;

	dev_dbg(master->dev,
		"%s:cmd_ccc_hi=0x%08x cmd_ccc_lo=0x%08x tx_len=%d\n", __func__,
		cmd_ccc->cmd_hi, cmd_ccc->cmd_lo, cmd_ccc->tx_len);

	for (i = 1; i < i3c_nxfers; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(i3c_xfers[i].len) |
			COMMAND_PORT_TRANSFER_ARG;

		if (i3c_xfers[i].rnw) {
			cmd->rx_buf = i3c_xfers[i].data.in;
			cmd->rx_len = i3c_xfers[i].len;
			cmd->cmd_lo = COMMAND_PORT_READ_TRANSFER |
				      COMMAND_PORT_SPEED(dev->info.max_read_ds);

		} else {
			cmd->tx_buf = i3c_xfers[i].data.out;
			cmd->tx_len = i3c_xfers[i].len;
			cmd->cmd_lo =
				COMMAND_PORT_SPEED(dev->info.max_write_ds);
		}

		cmd->cmd_lo |= COMMAND_PORT_TID(i) |
			       COMMAND_PORT_DEV_INDEX(data->index) |
			       COMMAND_PORT_ROC;

		if (i == (i3c_nxfers - 1))
			cmd->cmd_lo |= COMMAND_PORT_TOC;

		dev_dbg(master->dev,
			"%s:cmd_hi=0x%08x cmd_lo=0x%08x tx_len=%d rx_len=%d\n",
			__func__, cmd->cmd_hi, cmd->cmd_lo, cmd->tx_len,
			cmd->rx_len);
	}

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	dw_i3c_master_free_xfer(xfer);

	return ret;
}
#endif
static int dw_i3c_master_priv_xfers(struct i3c_dev_desc *dev,
				    struct i3c_priv_xfer *i3c_xfers,
				    int i3c_nxfers)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	unsigned int nrxwords = 0, ntxwords = 0;
	struct dw_i3c_xfer *xfer;
	int i, ret = 0;

	if (!i3c_nxfers)
		return 0;

	if (i3c_nxfers > master->caps.cmdfifodepth)
		return -ENOTSUPP;

	for (i = 0; i < i3c_nxfers; i++) {
		if (i3c_xfers[i].rnw)
			nrxwords += DIV_ROUND_UP(i3c_xfers[i].len, 4);
		else
			ntxwords += DIV_ROUND_UP(i3c_xfers[i].len, 4);
	}

	if (ntxwords > master->caps.datafifodepth ||
	    nrxwords > master->caps.datafifodepth)
		return -ENOTSUPP;

#ifdef CCC_WORKAROUND
	if (i3c_xfers[0].rnw == 0) {
		/* write command: check if hit special address */
		u8 tmp;

		memcpy(&tmp, i3c_xfers[0].data.out, 1);
		if (tmp == 0xff)
			return dw_i3c_master_ccc_xfers(dev, i3c_xfers, i3c_nxfers);
	}
#endif

	xfer = dw_i3c_master_alloc_xfer(master, i3c_nxfers);
	if (!xfer)
		return -ENOMEM;

	for (i = 0; i < i3c_nxfers; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(i3c_xfers[i].len) |
			COMMAND_PORT_TRANSFER_ARG;

		if (i3c_xfers[i].rnw) {
			cmd->rx_buf = i3c_xfers[i].data.in;
			cmd->rx_len = i3c_xfers[i].len;
			cmd->cmd_lo = COMMAND_PORT_READ_TRANSFER |
				      COMMAND_PORT_SPEED(dev->info.max_read_ds);

		} else {
			cmd->tx_buf = i3c_xfers[i].data.out;
			cmd->tx_len = i3c_xfers[i].len;
			cmd->cmd_lo =
				COMMAND_PORT_SPEED(dev->info.max_write_ds);
		}

		cmd->cmd_lo |= COMMAND_PORT_TID(i) |
			       COMMAND_PORT_DEV_INDEX(data->index) |
			       COMMAND_PORT_ROC;

		if (i == (i3c_nxfers - 1))
			cmd->cmd_lo |= COMMAND_PORT_TOC;

		dev_dbg(master->dev,
			"%s:cmd_hi=0x%08x cmd_lo=0x%08x tx_len=%d rx_len=%d\n",
			__func__, cmd->cmd_hi, cmd->cmd_lo, cmd->tx_len,
			cmd->rx_len);
	}

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	if (ret)
		goto out;

	for (i = 0; i < i3c_nxfers; i++)
		if (i3c_xfers[i].rnw)
			i3c_xfers[i].len = xfer->cmds[i].rx_len;
out:
	dw_i3c_master_free_xfer(xfer);

	return ret;
}

static int dw_i3c_target_priv_xfers(struct i3c_dev_desc *dev,
				    struct i3c_priv_xfer *i3c_xfers,
				    int i3c_nxfers)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_xfer *xfer;
	int i;

	if (!i3c_nxfers)
		return 0;

	xfer = dw_i3c_master_alloc_xfer(master, i3c_nxfers);
	if (!xfer)
		return -ENOMEM;

	for (i = 0; i < i3c_nxfers; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		if (!i3c_xfers[i].rnw) {
			cmd->tx_buf = i3c_xfers[i].data.out;
			cmd->tx_len = i3c_xfers[i].len;
			cmd->cmd_lo = 0 | (i << 3) | (cmd->tx_len << 16);

			dw_i3c_master_wr_tx_fifo(master, cmd->tx_buf, cmd->tx_len);
			writel(cmd->cmd_lo, master->regs + COMMAND_QUEUE_PORT);
		}
	}

	dw_i3c_master_free_xfer(xfer);

	return 0;
}

static int dw_i3c_target_generate_ibi(struct i3c_dev_desc *dev, const u8 *data, int len)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	u32 reg;

	if (data || len != 0)
		return -EOPNOTSUPP;

	reg = readl(master->regs + SLV_EVENT_CTRL);
	if ((reg & SLV_EVENT_CTRL_SIR_EN) == 0)
		return -EPERM;

	init_completion(&master->ibi.target.comp);
	writel(1, master->regs + SLV_INTR_REQ);

	if (!wait_for_completion_timeout(&master->ibi.target.comp, XFER_TIMEOUT)) {
		pr_warn("timeout waiting for completion\n");
		return -EINVAL;
	}

	reg = readl(master->regs + SLV_INTR_REQ);
	if (SLV_INTR_REQ_IBI_STS(reg) != IBI_STS_ACCEPTED) {
		reg = readl(master->regs + SLV_EVENT_CTRL);
		if ((reg & SLV_EVENT_CTRL_SIR_EN) == 0)
			pr_warn("sir is disabled by master\n");
		return -EACCES;
	}

	return 0;
}

static int dw_i3c_master_reattach_i3c_dev(struct i3c_dev_desc *dev,
					  u8 old_dyn_addr)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	int pos;

	pos = dw_i3c_master_get_free_pos(master);

	if (data->index > pos && pos > 0) {
		writel(0,
		       master->regs +
		       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

		master->addrs[data->index] = 0;
		master->free_pos |= BIT(data->index);

		data->index = pos;
		master->addrs[pos] = dev->info.dyn_addr;
		master->free_pos &= ~BIT(pos);
	}

	writel(DEV_ADDR_TABLE_DYNAMIC_ADDR(dev->info.dyn_addr),
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	master->addrs[data->index] = dev->info.dyn_addr;

	return 0;
}

static int dw_i3c_master_attach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_i2c_dev_data *data;
	int pos;

	pos = dw_i3c_master_get_free_pos(master);
	if (pos < 0)
		return pos;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->index = pos;
	master->addrs[pos] = dev->info.dyn_addr ? : dev->info.static_addr;
	master->free_pos &= ~BIT(pos);
	i3c_dev_set_master_data(dev, data);

	writel(DEV_ADDR_TABLE_DYNAMIC_ADDR(master->addrs[pos]),
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	return 0;
}

static void dw_i3c_master_detach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);

	writel(0,
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	i3c_dev_set_master_data(dev, NULL);
	master->addrs[data->index] = 0;
	master->free_pos |= BIT(data->index);
	kfree(data);
}

static int dw_i3c_master_i2c_xfers(struct i2c_dev_desc *dev,
				   const struct i2c_msg *i2c_xfers,
				   int i2c_nxfers)
{
	struct dw_i3c_i2c_dev_data *data = i2c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	unsigned int nrxwords = 0, ntxwords = 0;
	struct dw_i3c_xfer *xfer;
	int i, ret = 0;

	if (!i2c_nxfers)
		return 0;

	if (i2c_nxfers > master->caps.cmdfifodepth)
		return -ENOTSUPP;

	for (i = 0; i < i2c_nxfers; i++) {
		if (i2c_xfers[i].flags & I2C_M_RD)
			nrxwords += DIV_ROUND_UP(i2c_xfers[i].len, 4);
		else
			ntxwords += DIV_ROUND_UP(i2c_xfers[i].len, 4);
	}

	if (ntxwords > master->caps.datafifodepth ||
	    nrxwords > master->caps.datafifodepth)
		return -ENOTSUPP;

	xfer = dw_i3c_master_alloc_xfer(master, i2c_nxfers);
	if (!xfer)
		return -ENOMEM;

	for (i = 0; i < i2c_nxfers; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(i2c_xfers[i].len) |
			COMMAND_PORT_TRANSFER_ARG;

		cmd->cmd_lo = COMMAND_PORT_TID(i) |
			      COMMAND_PORT_DEV_INDEX(data->index) |
			      COMMAND_PORT_ROC;

		if (i2c_xfers[i].flags & I2C_M_RD) {
			cmd->cmd_lo |= COMMAND_PORT_READ_TRANSFER;
			cmd->rx_buf = i2c_xfers[i].buf;
			cmd->rx_len = i2c_xfers[i].len;
		} else {
			cmd->tx_buf = i2c_xfers[i].buf;
			cmd->tx_len = i2c_xfers[i].len;
		}

		if (i == (i2c_nxfers - 1))
			cmd->cmd_lo |= COMMAND_PORT_TOC;
	}

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	dw_i3c_master_free_xfer(xfer);

	return ret;
}

static int dw_i3c_master_request_ibi(struct i3c_dev_desc *dev,
				     const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	unsigned int i;

	if (master->ver_type < I3C_LC_RELEASE)
		return -EOPNOTSUPP;

	data->ibi_pool = i3c_generic_ibi_alloc_pool(dev, req);
	if (IS_ERR(data->ibi_pool))
		return PTR_ERR(data->ibi_pool);

	spin_lock_irq(&master->ibi.master.lock);
	for (i = 0; i < master->maxdevs; i++) {
		if (!master->ibi.master.slots[i]) {
			data->ibi = i;
			master->ibi.master.slots[i] = dev;
			break;
		}
	}
	spin_unlock_irq(&master->ibi.master.lock);

	if (i >= master->maxdevs) {
		i3c_generic_ibi_free_pool(data->ibi_pool);
		data->ibi_pool = NULL;
		return -ENOSPC;
	}

	return 0;
}

static int dw_i3c_master_enable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	int ret, pos, dat_loc;
	u32 reg;

	pos = dw_i3c_master_get_addr_pos(master, dev->info.dyn_addr);
	if (pos < 0)
		return pos;

	/*
	 * Clean-up the bit in IBI_SIR_REQ_REJECT so that the SIR request from the specific
	 * slave device is acknowledged by the master device.
	 */
	spin_lock_irq(&master->ibi.master.lock);
	reg = readl(master->regs + IBI_SIR_REQ_REJECT) & ~BIT(dev->info.dyn_addr);
	writel(reg, master->regs + IBI_SIR_REQ_REJECT);

	/*
	 * Corresponding changes to DAT: ACK the SIR from the specific device;
	 * One or more data bytes must be present.
	 */
	dat_loc = DEV_ADDR_TABLE_LOC(master->datstartaddr, pos);
	reg = readl(master->regs + dat_loc);
	reg &= ~DEV_ADDR_TABLE_SIR_REJECT;
	if (dev->info.bcr & I3C_BCR_IBI_PAYLOAD)
		reg |= DEV_ADDR_TABLE_IBI_WITH_DATA;
	writel(reg, master->regs + dat_loc);

	spin_unlock_irq(&master->ibi.master.lock);

	/* Enable SIR generation on the requested slave device */
	ret = i3c_master_enec_locked(m, dev->info.dyn_addr, I3C_CCC_EVENT_SIR);
	if (ret) {
		spin_lock_irq(&master->ibi.master.lock);
		reg = readl(master->regs + IBI_SIR_REQ_REJECT);
		reg |= BIT(dev->info.dyn_addr);
		writel(reg, master->regs + IBI_SIR_REQ_REJECT);

		reg = readl(master->regs + dat_loc);
		reg |= DEV_ADDR_TABLE_SIR_REJECT;
		reg &= ~DEV_ADDR_TABLE_IBI_WITH_DATA;
		writel(reg, master->regs + dat_loc);
		spin_unlock_irq(&master->ibi.master.lock);
	}

	return ret;
}

static int dw_i3c_master_disable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	u32 reg;
	int ret, pos, dat_loc;

	/* Disable SIR generation on the requested slave device */
	ret = i3c_master_disec_locked(m, dev->info.dyn_addr, I3C_CCC_EVENT_SIR);
	if (ret)
		return ret;

	pos = dw_i3c_master_get_addr_pos(master, dev->info.dyn_addr);
	if (pos < 0) {
		dev_warn(master->dev, "Failed to get DAT addr pos for dev %02x\n",
			 dev->info.dyn_addr);
		return pos;
	}

	spin_lock_irq(&master->ibi.master.lock);
	reg = readl(master->regs + IBI_SIR_REQ_REJECT);
	reg |= BIT(dev->info.dyn_addr);
	writel(reg, master->regs + IBI_SIR_REQ_REJECT);

	dat_loc = DEV_ADDR_TABLE_LOC(master->datstartaddr, pos);
	reg = readl(master->regs + dat_loc);
	reg |= DEV_ADDR_TABLE_SIR_REJECT;
	reg &= ~DEV_ADDR_TABLE_IBI_WITH_DATA;
	writel(reg, master->regs + dat_loc);
	spin_unlock_irq(&master->ibi.master.lock);

	return 0;
}

static void dw_i3c_master_free_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);

	spin_lock_irq(&master->ibi.master.lock);
	master->ibi.master.slots[data->ibi] = NULL;
	data->ibi = -1;
	spin_unlock_irq(&master->ibi.master.lock);

	i3c_generic_ibi_free_pool(data->ibi_pool);
}

static void dw_i3c_master_recycle_ibi_slot(struct i3c_dev_desc *dev,
					   struct i3c_ibi_slot *slot)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);

	i3c_generic_ibi_recycle_slot(data->ibi_pool, slot);
}

static int dw_i3c_master_attach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_i2c_dev_data *data;
	int pos;

	pos = dw_i3c_master_get_free_pos(master);
	if (pos < 0)
		return pos;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->index = pos;
	master->addrs[pos] = dev->addr;
	master->free_pos &= ~BIT(pos);
	i2c_dev_set_master_data(dev, data);

	writel(DEV_ADDR_TABLE_LEGACY_I2C_DEV |
	       DEV_ADDR_TABLE_STATIC_ADDR(dev->addr),
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	return 0;
}

static void dw_i3c_master_detach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct dw_i3c_i2c_dev_data *data = i2c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);

	writel(0,
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	i2c_dev_set_master_data(dev, NULL);
	master->addrs[data->index] = 0;
	master->free_pos |= BIT(data->index);
	kfree(data);
}

static struct i3c_dev_desc *dw_get_i3c_dev_by_addr(struct dw_i3c_master *master,
						   u8 addr)
{
	int i;

	for (i = 0; i < master->maxdevs; i++) {
		if (master->ibi.master.slots[i] &&
		    master->ibi.master.slots[i]->info.dyn_addr == addr)
			return master->ibi.master.slots[i];
	}
	return NULL;
}

static void dw_i3c_master_sir_handler(struct dw_i3c_master *master,
				      u32 ibi_status)
{
	u8 length = IBI_QUEUE_STATUS_DATA_LEN(ibi_status);
	u8 addr = IBI_QUEUE_IBI_ADDR(ibi_status);
	struct dw_i3c_i2c_dev_data *data;
	struct i3c_ibi_slot *slot;
	struct i3c_dev_desc *dev;
	u8 *buf;

	dev = dw_get_i3c_dev_by_addr(master, addr);
	if (!dev) {
		dev_warn(master->dev, "no matching dev\n");
		goto err;
	}

	data = i3c_dev_get_master_data(dev);
	slot = i3c_generic_ibi_get_free_slot(data->ibi_pool);
	if (!slot) {
		dev_warn(master->dev, "no free ibi slot\n");
		goto err;
	}
	buf = slot->data;
	/* prepend ibi status */
	memcpy(buf, &ibi_status, sizeof(ibi_status));
	buf += sizeof(ibi_status);

	dw_i3c_master_read_ibi_fifo(master, buf, length);

	slot->len = length + sizeof(ibi_status);

	i3c_master_queue_ibi(dev, slot);
	return;

err:
	dw_i3c_master_flush_ibi_fifo(master, length);
}

static void dw_i3c_master_demux_ibis(struct dw_i3c_master *master)
{
	u32 nibi, status, intr_signal_en;
	int i;

	nibi = QUEUE_STATUS_IBI_STATUS_CNT(readl(master->regs + QUEUE_STATUS_LEVEL));

	spin_lock(&master->ibi.master.lock);
	intr_signal_en = readl(master->regs + INTR_SIGNAL_EN);
	intr_signal_en &= ~INTR_IBI_THLD_STAT;
	writel(intr_signal_en, master->regs + INTR_SIGNAL_EN);

	for (i = 0; i < nibi; i++) {
		status = readl(master->regs + IBI_QUEUE_STATUS);

		if (status & IBI_QUEUE_STATUS_RSP_NACK)
			dev_warn_once(master->dev, "ibi from unrecognized slave %02lx\n",
				      IBI_QUEUE_IBI_ADDR(status));

		if (status & IBI_QUEUE_STATUS_PEC_ERR)
			dev_warn(master->dev, "ibi crc/pec error\n");

		if (IBI_TYPE_SIR(status))
			dw_i3c_master_sir_handler(master, status);
	}

	intr_signal_en = readl(master->regs + INTR_SIGNAL_EN);
	intr_signal_en |= INTR_IBI_THLD_STAT;
	writel(intr_signal_en, master->regs + INTR_SIGNAL_EN);
	spin_unlock(&master->ibi.master.lock);
}

static void dw_i3c_target_handle_response_ready(struct dw_i3c_master *master)
{
	struct i3c_dev_desc *desc = master->base.this;
	u32 reg = readl(master->regs + QUEUE_STATUS_LEVEL);
	u32 nresp = QUEUE_STATUS_LEVEL_RESP(reg);
	int i;

	for (i = 0; i < nresp; i++) {
		u32 resp = readl(master->regs + RESPONSE_QUEUE_PORT);
		u32 nbytes = RESPONSE_PORT_DATA_LEN(resp);

		if (nbytes > master->target_rx.max_len) {
			dev_warn(master->dev, "private write data length is larger than max\n");
			return;
		}

		dw_i3c_master_read_rx_fifo(master, master->target_rx.buf, nbytes);

		if (desc->target_info.read_handler)
			desc->target_info.read_handler(desc->dev, master->target_rx.buf, nbytes);
	}
}

static irqreturn_t dw_i3c_master_irq_handler(int irq, void *dev_id)
{
	struct dw_i3c_master *master = dev_id;
	u32 status;

	status = readl(master->regs + INTR_STATUS);
	if (!(status & readl(master->regs + INTR_STATUS_EN))) {
		writel(INTR_ALL, master->regs + INTR_STATUS);
		return IRQ_NONE;
	}

	if (master->base.target) {
		if (status & INTR_IBI_UPDATED_STAT) {
			writel(INTR_IBI_UPDATED_STAT, master->regs + INTR_STATUS);
			complete(&master->ibi.target.comp);
		}

		if (status & INTR_READ_REQ_RECV_STAT) {
			/*
			 * TODO: Pass this information to the driver to take
			 * appropriate action.
			 */
			dev_dbg(master->dev,
				"private read received from controller when cmd queue is empty\n");
			writel(INTR_READ_REQ_RECV_STAT, master->regs + INTR_STATUS);
		}

		if (status & INTR_RESP_READY_STAT)
			dw_i3c_target_handle_response_ready(master);
	}

	spin_lock(&master->xferqueue.lock);
	dw_i3c_master_end_xfer_locked(master, status);
	if (status & INTR_TRANSFER_ERR_STAT)
		writel(INTR_TRANSFER_ERR_STAT, master->regs + INTR_STATUS);
	spin_unlock(&master->xferqueue.lock);

	if (status & INTR_IBI_THLD_STAT)
		dw_i3c_master_demux_ibis(master);

	return IRQ_HANDLED;
}

static void dw_i3c_master_of_timings(struct dw_i3c_master *master,
				     struct device_node *node)
{
	u32 val;

	if (!of_property_read_u32(node, "i2c-scl-hz", &val))
		master->timings.i3c_od_scl_freq = val;

	if (!of_property_read_u32(node, "i3c-od-scl-low-ns", &val)) {
		if (val < I3C_BUS_I3C_OD_TLOW_MIN_NS)
			dev_warn(master->dev,
				 "invalid i3c-od-scl-low-ns: %u, ignoring provided value\n", val);
		else
			master->timings.i3c_od_scl_low = val;
	}

	if (!of_property_read_u32(node, "i3c-od-scl-high-ns", &val))
		master->timings.i3c_od_scl_high = val;

	if (!of_property_read_u32(node, "i3c-scl-hz", &val))
		master->timings.i3c_pp_scl_freq = val;

	if (!of_property_read_u32(node, "i3c-pp-scl-low-ns", &val)) {
		if (val < I3C_BUS_I3C_PP_TLOW_MIN_NS)
			dev_warn(master->dev,
				 "invalid i3c-pp-scl-low-ns: %u, ignoring provided value\n", val);
		else
			master->timings.i3c_pp_scl_low = val;
	}

	if (!of_property_read_u32(node, "i3c-pp-scl-high-ns", &val)) {
		if (val < I3C_BUS_I3C_PP_THIGH_MIN_NS)
			dev_warn(master->dev,
				 "invalid i3c-pp-scl-high-ns: %u, ignoring provided value\n", val);
		else
			master->timings.i3c_pp_scl_high = val;
	}

	if (!of_property_read_u32(node, "sda-tx-hold-ns", &val))
		master->timings.sda_tx_hold = val;
}

static const struct i3c_target_ops dw_mipi_i3c_target_ops = {
	.bus_init = dw_i3c_target_bus_init,
	.bus_cleanup = dw_i3c_target_bus_cleanup,
	.priv_xfers = dw_i3c_target_priv_xfers,
	.generate_ibi = dw_i3c_target_generate_ibi,
};

static const struct i3c_master_controller_ops dw_mipi_i3c_ops = {
	.bus_init = dw_i3c_master_bus_init,
	.bus_cleanup = dw_i3c_master_bus_cleanup,
	.attach_i3c_dev = dw_i3c_master_attach_i3c_dev,
	.reattach_i3c_dev = dw_i3c_master_reattach_i3c_dev,
	.detach_i3c_dev = dw_i3c_master_detach_i3c_dev,
	.do_daa = dw_i3c_master_daa,
	.supports_ccc_cmd = dw_i3c_master_supports_ccc_cmd,
	.send_ccc_cmd = dw_i3c_master_send_ccc_cmd,
	.priv_xfers = dw_i3c_master_priv_xfers,
	.attach_i2c_dev = dw_i3c_master_attach_i2c_dev,
	.detach_i2c_dev = dw_i3c_master_detach_i2c_dev,
	.i2c_xfers = dw_i3c_master_i2c_xfers,
	.request_ibi = dw_i3c_master_request_ibi,
	.enable_ibi = dw_i3c_master_enable_ibi,
	.free_ibi = dw_i3c_master_free_ibi,
	.disable_ibi = dw_i3c_master_disable_ibi,
	.recycle_ibi_slot = dw_i3c_master_recycle_ibi_slot,
};

static int dw_i3c_probe(struct platform_device *pdev)
{
	struct dw_i3c_master *master;
	int ret, irq;

	master = devm_kzalloc(&pdev->dev, sizeof(*master), GFP_KERNEL);
	if (!master)
		return -ENOMEM;

	master->dev = &pdev->dev;

	master->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(master->regs))
		return PTR_ERR(master->regs);

	master->core_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(master->core_clk))
		return PTR_ERR(master->core_clk);

	master->core_rst = devm_reset_control_get_optional_exclusive(&pdev->dev,
								    "core_rst");
	if (IS_ERR(master->core_rst))
		return PTR_ERR(master->core_rst);

	ret = clk_prepare_enable(master->core_clk);
	if (ret)
		goto err_disable_core_clk;

	reset_control_deassert(master->core_rst);

	spin_lock_init(&master->xferqueue.lock);
	INIT_LIST_HEAD(&master->xferqueue.list);

	spin_lock_init(&master->ibi.master.lock);

	platform_set_drvdata(pdev, master);

	/* Information regarding the FIFOs/QUEUEs depth */
	ret = readl(master->regs + QUEUE_STATUS_LEVEL);
	master->caps.cmdfifodepth = QUEUE_STATUS_LEVEL_CMD(ret);

	ret = readl(master->regs + DATA_BUFFER_STATUS_LEVEL);
	master->caps.datafifodepth = DATA_BUFFER_STATUS_LEVEL_TX(ret);

	ret = readl(master->regs + DEVICE_ADDR_TABLE_POINTER);
	master->datstartaddr = ret;
	master->maxdevs = ret >> 16;
	master->free_pos = GENMASK(master->maxdevs - 1, 0);
#ifdef CCC_WORKAROUND
	if (master->maxdevs > 0) {
		master->free_pos &= ~BIT(master->maxdevs - 1);
		ret = (even_parity(I3C_BROADCAST_ADDR) << 7) | I3C_BROADCAST_ADDR;
		master->addrs[master->maxdevs - 1] = ret;
		writel(DEV_ADDR_TABLE_DYNAMIC_ADDR(ret),
			   master->regs + DEV_ADDR_TABLE_LOC(master->datstartaddr,
							     master->maxdevs - 1));
	}
#endif
	writel(INTR_ALL, master->regs + INTR_STATUS);
	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq,
			       dw_i3c_master_irq_handler, 0,
			       dev_name(&pdev->dev), master);
	if (ret)
		goto err_assert_rst;

	ret = readl(master->regs + I3C_VER_TYPE);
	master->ver_type = I3C_VER_RELEASE_TYPE(ret);

	dw_i3c_master_of_timings(master, pdev->dev.of_node);

	ret = i3c_register(&master->base, &pdev->dev, &dw_mipi_i3c_ops, &dw_mipi_i3c_target_ops,
			   false);
	if (ret)
		goto err_assert_rst;

	dev_info(&pdev->dev, "i3c bus %d registered, irq %d\n",
		 master->base.bus.id, irq);

	return 0;

err_assert_rst:
	reset_control_assert(master->core_rst);

err_disable_core_clk:
	clk_disable_unprepare(master->core_clk);

	return ret;
}

static int dw_i3c_remove(struct platform_device *pdev)
{
	struct dw_i3c_master *master = platform_get_drvdata(pdev);
	int ret;

	ret = i3c_unregister(&master->base);
	if (ret)
		return ret;

	reset_control_assert(master->core_rst);

	clk_disable_unprepare(master->core_clk);

	return 0;
}

static const struct of_device_id dw_i3c_master_of_match[] = {
	{ .compatible = "snps,dw-i3c-master-1.00a", },
	{},
};
MODULE_DEVICE_TABLE(of, dw_i3c_master_of_match);

static struct platform_driver dw_i3c_driver = {
	.probe = dw_i3c_probe,
	.remove = dw_i3c_remove,
	.driver = {
		.name = "dw-i3c-master",
		.of_match_table = of_match_ptr(dw_i3c_master_of_match),
	},
};
module_platform_driver(dw_i3c_driver);

MODULE_AUTHOR("Vitor Soares <vitor.soares@synopsys.com>");
MODULE_DESCRIPTION("DesignWare MIPI I3C driver");
MODULE_LICENSE("GPL v2");
