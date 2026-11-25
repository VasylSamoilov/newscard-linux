/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/*
 * Interface between newscard.ko and newscard.py. Keep the layouts in sync.
 * Copyright 2026 Vasyl Samoilov <vasyl.samoilov@gmail.com>
 */
#ifndef _NEWSCARD_IOCTL_H
#define _NEWSCARD_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define NEWSCARD_DATA_MAX	256
/*
 * result <= NEWSCARD_NAK_BASE: the card answered NAK (0x15) with code
 * (NEWSCARD_NAK_BASE - result)
 */
#define NEWSCARD_NAK_BASE	(-1000)

/*
 * One mailbox command: send [cmd, len16, data], then read reply_len bytes
 * of the reply buffer ([cmd, len16, status, data...]); reply_len 0 = no
 * reply expected (command 0x13).
 */
struct newscard_xfer {
	__u8  cmd;
	__u8  pad0;
	__u16 len;
	__u16 reply_len;
	__u16 pad1;
	__s32 result;
	__u8  data[NEWSCARD_DATA_MAX];
	__u8  reply[NEWSCARD_DATA_MAX];
};

/*
 * Read the message store (common memory below the window size) while
 * holding the mailbox lock, retrying if the card takes it meanwhile.
 */
struct newscard_lread {
	__u32 offset;
	__u32 len;
	__s32 result;
	__u32 pad;
	__u8  data[1024];
};

#define NEWSCARD_IOC_XFER	_IOWR('N', 1, struct newscard_xfer)
#define NEWSCARD_IOC_LREAD	_IOWR('N', 2, struct newscard_lread)
/* Clear STAT bit 0 if set; returns (int) whether an event was pending. */
#define NEWSCARD_IOC_EVENT	_IOR('N', 3, int)

#endif
