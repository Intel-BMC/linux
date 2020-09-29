.. SPDX-License-Identifier: GPL-2.0

==================
JTAG userspace API
==================
JTAG master devices can be accessed through a character misc-device.

Each JTAG master interface can be accessed by using /dev/jtagN.

JTAG system calls set:
 * SIR (Scan Instruction Register, IEEE 1149.1 Instruction Register scan);
 * SDR (Scan Data Register, IEEE 1149.1 Data Register scan);
 * RUNTEST (Forces the IEEE 1149.1 bus to a run state for a specified number of clocks.

open(), close()
---------------
Open/Close  device:
::

	jtag_fd = open("/dev/jtag0", O_RDWR);
	close(jtag_fd);

ioctl()
-------
All access operations to JTAG devices are performed through ioctl interface.
The IOCTL interface supports these requests:
::

	JTAG_SIOCSTATE - Force JTAG state machine to go into a TAPC state
	JTAG_SIOCFREQ - Set JTAG TCK frequency
	JTAG_GIOCFREQ - Get JTAG TCK frequency
	JTAG_IOCXFER - send/receive JTAG data Xfer
	JTAG_GIOCSTATUS - get current JTAG TAP state
	JTAG_SIOCMODE - set JTAG mode flags.
	JTAG_IOCBITBANG - JTAG bitbang low level control.

JTAG_SIOCFREQ
~~~~~~~~~~~~~
Set JTAG clock speed:
::

	unsigned int jtag_fd;
	ioctl(jtag_fd, JTAG_SIOCFREQ, &frq);

JTAG_GIOCFREQ
~~~~~~~~~~~~~
Get JTAG clock speed:
::

	unsigned int jtag_fd;
	ioctl(jtag_fd, JTAG_GIOCFREQ, &frq);

JTAG_SIOCSTATE
~~~~~~~~~~~~~~
Force JTAG state machine to go into a TAPC state
::

	struct jtag_tap_state {
		__u8	reset;
		__u8	from;
		__u8	endstate;
		__u8	tck;
	};

reset: one of below options
::

	JTAG_NO_RESET - go through selected endstate from current state
	JTAG_FORCE_RESET - go through TEST_LOGIC/RESET state before selected endstate

endstate: any state listed in jtag_tapstate enum
::

	enum jtag_tapstate {
		JTAG_STATE_TLRESET,
		JTAG_STATE_IDLE,
		JTAG_STATE_SELECTDR,
		JTAG_STATE_CAPTUREDR,
		JTAG_STATE_SHIFTDR,
		JTAG_STATE_EXIT1DR,
		JTAG_STATE_PAUSEDR,
		JTAG_STATE_EXIT2DR,
		JTAG_STATE_UPDATEDR,
		JTAG_STATE_SELECTIR,
		JTAG_STATE_CAPTUREIR,
		JTAG_STATE_SHIFTIR,
		JTAG_STATE_EXIT1IR,
		JTAG_STATE_PAUSEIR,
		JTAG_STATE_EXIT2IR,
		JTAG_STATE_UPDATEIR
	};

tck: clock counter

Example:
::

	struct jtag_tap_state tap_state;

	tap_state.endstate = JTAG_STATE_IDLE;
	tap_state.reset = 0;
	tap_state.tck = data_p->tck;
	usleep(25 * 1000);
	ioctl(jtag_fd, JTAG_SIOCSTATE, &tap_state);

JTAG_GIOCSTATUS
~~~~~~~~~~~~~~~
Get JTAG TAPC current machine state
::

	unsigned int jtag_fd;
	jtag_tapstate tapstate;
	ioctl(jtag_fd, JTAG_GIOCSTATUS, &tapstate);

JTAG_IOCXFER
~~~~~~~~~~~~
Send SDR/SIR transaction
::

	struct jtag_xfer {
		__u8	type;
		__u8	direction;
		__u8	from;
		__u8	endstate;
		__u32	padding;
		__u32	length;
		__u64	tdio;
	};

type: transfer type - JTAG_SIR_XFER/JTAG_SDR_XFER

direction: xfer direction - JTAG_READ_XFER/JTAG_WRITE_XFER/JTAG_READ_WRITE_XFER

from: jtag_tapstate enum representing the initial tap state of the chain before xfer.

endstate: end state after transaction finish any of jtag_tapstate enum

padding: padding configuration. See the following table with bitfield descriptions.

===============  =========  =======  =====================================================
Bit Field        Bit begin  Bit end  Description
===============  =========  =======  =====================================================
rsvd             25         31       Reserved, not used
pad data         24         24       Value used for pre and post padding. Either 1 or 0.
post pad count   12         23       Number of padding bits to be executed after transfer.
pre pad count    0          11       Number of padding bit to be executed before transfer.
===============  =========  =======  =====================================================

length: xfer data length in bits

tdio : xfer data array

Example:
::

	struct jtag_xfer xfer;
	static char buf[64];
	static unsigned int buf_len = 0;
	[...]
	xfer.type = JTAG_SDR_XFER;
	xfer.tdio = (__u64)buf;
	xfer.length = buf_len;
	xfer.from = JTAG_STATE_TLRESET;
	xfer.endstate = JTAG_STATE_IDLE;

	if (is_read)
		xfer.direction = JTAG_READ_XFER;
	else if (is_write)
		xfer.direction = JTAG_WRITE_XFER;
	else
		xfer.direction = JTAG_READ_WRITE_XFER;

	ioctl(jtag_fd, JTAG_IOCXFER, &xfer);

JTAG_SIOCMODE
~~~~~~~~~~~~~
If hardware driver can support different running modes you can change it.

Example:
::

	struct jtag_mode mode;
	mode.feature = JTAG_XFER_MODE;
	mode.mode = JTAG_XFER_HW_MODE;
	ioctl(jtag_fd, JTAG_SIOCMODE, &mode);

JTAG_IOCBITBANG
~~~~~~~~~~~~~~~
JTAG Bitbang low level operation.

Example:
::

	struct tck_bitbang bitbang
	bitbang.tms = 1;
	bitbang.tdi = 0;
	ioctl(jtag_fd, JTAG_IOCBITBANG, &bitbang);
	tdo = bitbang.tdo;


THANKS TO
---------
Contributors to Linux-JTAG discussions include (in alphabetical order,
by last name):

- Ernesto Corona
- Jiri Pirko
