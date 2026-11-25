// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Motorola NewsCard Data Receiver (Newton Messaging Card), a POCSAG pager
 * PC Card. Protocol: PROTOCOL.md.
 *
 * Copyright 2026 Vasyl Samoilov <vasyl.samoilov@gmail.com>
 *
 * Interfaces:
 *   /sys/class/newscard/newscard0/  battery, switch, clock (rw), alerts (rw), total,
 *                        unread, free_blocks, status, event; write-only
 *                        markread, delete (sequence number)
 *   /dev/newscard0msgs   messages, one per read(), poll() on new pages
 *   /dev/mtdN            the card's 128 KB flash, read-only MTD (ROM);
 *                        /dev/mtdblockN with mtdblock_ro
 *   /dev/newscard0       raw common memory (byte access, any card offset)
 *                        and the XFER/LREAD/EVENT ioctls used by newscard.py
 *   /dev/newscard0attr   attribute memory (4 KB)
 *
 * Every access is a single byte (ioread8/iowrite8).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/sched/signal.h>
#include <linux/kref.h>
#include <linux/pci.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/sort.h>
#include <linux/bsearch.h>
#include <linux/mtd/mtd.h>
#include <linux/unaligned.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ss.h>
#include <pcmcia/ds.h>

#include "newscard_ioctl.h"

#define DRIVER_NAME "newscard"

/* ---- module parameters ---- */

static bool width16;
module_param(width16, bool, 0444);
MODULE_PARM_DESC(width16, "Map common memory as 16 bit (1) or 8 bit (0, default; the card needs 8 bit). Accesses are always bytes.");

/*
 * Ricoh CardBus bridges time 16-bit memory cycles with their own
 * setup/command/hold registers (PCI clocks), not the ExCA wait states.
 * The card's flash needs setup 2..7 and command 0..3 clocks (with command
 * 4 and up, or setup 0..1, 1 bits read as 0). 0x0425 = setup 5, command 2,
 * hold 1.
 */
static ushort ricoh_mem_timing = 0x0425;
module_param(ricoh_mem_timing, ushort, 0444);
MODULE_PARM_DESC(ricoh_mem_timing, "Ricoh bridge 16-bit memory timing register 0x8A (setup | cmd<<4 | hold<<10), 0 = leave unchanged (default 0x0425)");

#define RL5C4XX_16BIT_CTL	0x84
#define  RL5C4XX_16CTL_MEM_TIMING 0x0200
#define RL5C4XX_16BIT_MEM_0	0x8a

static uint flash_reads;
module_param(flash_reads, uint, 0644);
MODULE_PARM_DESC(flash_reads, "Read each flash byte N times and OR the results (flash errors are 1->0 only). 0 = auto: 1 with tuned Ricoh timing, else 5");

static uint poll_ms = 2000;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Check the card's new-page event bit every N ms and notify (sysfs 'event', uevent); 0 = off");

static uint speed_ns = 300;
module_param(speed_ns, uint, 0444);
MODULE_PARM_DESC(speed_ns, "Memory access time in ns (default 300; 0 = socket default)");

static uint win_size = 0x20000;
module_param(win_size, uint, 0444);
MODULE_PARM_DESC(win_size, "Common memory window size (default 128 KB)");

static uint timeout_ms = 20000;
module_param(timeout_ms, uint, 0644);
MODULE_PARM_DESC(timeout_ms, "Mailbox timeout in ms (default 20000)");

static bool allow_raw_write;
module_param(allow_raw_write, bool, 0644);
MODULE_PARM_DESC(allow_raw_write, "Allow write() to card memory (default 0; the mailbox ioctls work without it)");

static bool use_wait = true;
module_param(use_wait, bool, 0444);
MODULE_PARM_DESC(use_wait, "Honour the card's WAIT signal (default 1)");

/* ---- card layout (plain card offsets in common memory) ---- */

#define NC_CMD		0x0C00	/* request buffer */
#define NC_SEM		0x0FF0	/* host writes 0x44 and reads it back */
#define NC_STAT		0x0FFD	/* bit 0: event pending, host clears with 0 */
#define NC_OWN		0x0FFE	/* host writes 0xAA; card changes it when it takes the request */
#define NC_DOORBELL	0x0FFF	/* host writes 0xFF */
#define NC_RSP		0x1000	/* reply buffer */
#define NC_CMD_MAX	(NC_SEM - NC_CMD - 5)

#define SEM_HOST	0x44
#define OWN_HOST	0xAA
#define DOORBELL	0xFF
#define NAK		0x15

#define NC_ATTR_SIZE	0x1000
#define NC_MINORS	3		/* raw, attr, msgs */
#define NC_FLASH	0x02000000
#define NC_FLASH_SIZE	0x20000

/* message store */
#define NC_DIR		0x1400
#define NC_DIR_N	16
#define NC_HEAP		0x1C00
#define NC_HEAP_END	0x20000
#define NC_MAX_RECS	2048
#define NC_MAX_BLOCKS	64

struct newscard_dev {
	struct pcmcia_device *link;
	struct mutex lock;		/* serializes all card access */
	void __iomem *cm;		/* common memory window */
	struct resource *cm_res;
	u32 cm_page;			/* card offset mapped at cm */
	void __iomem *am;		/* attribute memory window, may be NULL */
	struct cdev *cdev;
	struct kref ref;
	int id;
	dev_t devno;
	struct device *dev[NC_MINORS];
	bool event_pending;
	bool present;
	struct pci_dev *bridge;		/* Ricoh bridge whose timing was changed */
	u16 saved_mem_timing;
	struct mtd_info mtd;
	bool mtd_registered;
	char mtd_name[24];
	struct delayed_work poll;
	unsigned long events;		/* new pages seen (sysfs 'event') */
	wait_queue_head_t wq;		/* woken on new pages and on eject */
	int last_total;			/* total count at the last poll, -1 = unknown */
};

/* one formatted message for /dev/newscardNmsgs */
struct nc_msg {
	u32 key;			/* record offset << 8 | sequence number */
	u8 when[6];			/* YY MM DD hh mm seq, for sorting */
	size_t len;
	char *text;
};

/* per open file */
struct nc_file {
	struct newscard_dev *nc;
	/* messages device: records not yet returned, and keys already returned */
	struct nc_msg *q;
	unsigned int qn, qpos;
	u32 *seen;
	unsigned int nseen;
	unsigned long events_seen;
};

#define NCF_DEV(f) (((struct nc_file *)(f)->private_data)->nc)

static struct class *newscard_class;
static dev_t newscard_base;
static DEFINE_IDA(newscard_ida);
/* id -> device, so open() can take a reference; protected by table_lock */
static struct newscard_dev *newscard_table[16];
static DEFINE_MUTEX(table_lock);

static void nc_free(struct kref *ref)
{
	kfree(container_of(ref, struct newscard_dev, ref));
}

/* ---- low level access (caller holds lock) ---- */

static int nc_page(struct newscard_dev *nc, u32 off)
{
	u32 page = off & ~(win_size - 1);
	int ret;

	if (page == nc->cm_page)
		return 0;
	ret = pcmcia_map_mem_page(nc->link, nc->cm_res, page);
	if (ret)
		return ret;
	nc->cm_page = page;
	return 0;
}

static u8 rd(struct newscard_dev *nc, u32 off)
{
	return ioread8(nc->cm + (off & (win_size - 1)));
}

static void wr(struct newscard_dev *nc, u32 off, u8 v)
{
	iowrite8(v, nc->cm + (off & (win_size - 1)));
}

static bool timed_out(unsigned long deadline)
{
	return time_after(jiffies, deadline);
}

/* Take the mailbox: SEM = 0x44, OWN = 0xAA. Returns 0 or -ETIMEDOUT. */
static int nc_acquire(struct newscard_dev *nc, unsigned long deadline)
{
	int i;

	for (;;) {
		for (i = 0; i < 32000; i++) {
			wr(nc, NC_SEM, SEM_HOST);
			wr(nc, NC_OWN, OWN_HOST);
			if (rd(nc, NC_SEM) == SEM_HOST && rd(nc, NC_OWN) == OWN_HOST)
				return 0;
			if ((i & 255) == 255)
				cpu_relax();
		}
		if (timed_out(deadline))
			return -ETIMEDOUT;
		if (signal_pending(current))
			return -EINTR;
		usleep_range(500, 1000);
	}
}

/* Clear STAT bit 0 (event pending) and remember the event. */
static void nc_clear_status(struct newscard_dev *nc)
{
	u8 s = rd(nc, NC_STAT);

	if (s & 1) {
		wr(nc, NC_STAT, 0);
		nc->event_pending = true;
	}
}

static void nc_diag(struct newscard_dev *nc, const char *what, u8 cmd)
{
	dev_warn(&nc->link->dev, "%s (command %02x): SEM=%02x STAT=%02x OWN=%02x CMD[0]=%02x RSP=%02x %02x %02x %02x %02x\n",
		 what, cmd, rd(nc, NC_SEM), rd(nc, NC_STAT), rd(nc, NC_OWN), rd(nc, NC_CMD),
		 rd(nc, NC_RSP), rd(nc, NC_RSP + 1), rd(nc, NC_RSP + 2),
		 rd(nc, NC_RSP + 3), rd(nc, NC_RSP + 4));
}

/* Send a request: pkt = cmd, lenHi, lenLo, payload; then ring the doorbell. */
static int nc_send(struct newscard_dev *nc, const u8 *pkt, unsigned int n)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned int i;
	int ret;

	for (;;) {
		ret = nc_acquire(nc, deadline);
		if (ret) {
			nc_diag(nc, "send: acquire failed", pkt[0]);
			return ret;
		}
		nc_clear_status(nc);
		for (i = 0; i < n; i++)
			wr(nc, NC_CMD + i, pkt[i]);
		wr(nc, NC_CMD + n, 0xFF);
		wr(nc, NC_CMD + n + 1, 0xAA);
		if (rd(nc, NC_CMD) == pkt[0] && rd(nc, NC_OWN) == OWN_HOST)
			break;
		if (timed_out(deadline)) {
			nc_diag(nc, "send: command buffer readback failed", pkt[0]);
			return -EIO;
		}
		if (signal_pending(current))
			return -EINTR;
		usleep_range(500, 1000);
	}
	wr(nc, NC_DOORBELL, DOORBELL);
	while (rd(nc, NC_OWN) == OWN_HOST) {
		if (timed_out(deadline)) {
			nc_diag(nc, "send: card did not take the command (OWN stayed AA)", pkt[0]);
			return -ETIMEDOUT;
		}
		if (signal_pending(current))
			return -EINTR;
		usleep_range(100, 200);
	}
	return 0;
}

/* Read the reply. Returns 0, -ETIMEDOUT, or NEWSCARD_NAK_BASE - code on NAK. */
static int nc_receive(struct newscard_dev *nc, u8 cmd, u8 *buf, unsigned int n)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned int i;
	int ret;

	for (;;) {
		ret = nc_acquire(nc, deadline);
		if (ret) {
			nc_diag(nc, "receive: acquire failed", cmd);
			return ret;
		}
		nc_clear_status(nc);
		for (i = 0; i < n; i++)
			buf[i] = rd(nc, NC_RSP + i);
		if (buf[0] == cmd && rd(nc, NC_OWN) == OWN_HOST)
			break;
		if (timed_out(deadline)) {
			nc_diag(nc, "receive: no reply for this command", cmd);
			return -ETIMEDOUT;
		}
		if (signal_pending(current))
			return -EINTR;
		usleep_range(500, 1000);
	}
	if (n >= 5 && buf[3] == NAK)
		return NEWSCARD_NAK_BASE - buf[4];
	return 0;
}

/* Read the message store while holding the mailbox. */
static int nc_locked_read(struct newscard_dev *nc, u32 off, u8 *buf, unsigned int n)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned int i;
	int ret;

	for (;;) {
		ret = nc_acquire(nc, deadline);
		if (ret) {
			nc_diag(nc, "store read: acquire failed", 0);
			return ret;
		}
		nc_clear_status(nc);
		for (i = 0; i < n; i++)
			buf[i] = rd(nc, off + i);
		if (rd(nc, NC_OWN) == OWN_HOST)
			return 0;
		if (timed_out(deadline)) {
			nc_diag(nc, "store read: card kept taking the mailbox", 0);
			return -ETIMEDOUT;
		}
		if (signal_pending(current))
			return -EINTR;
		usleep_range(500, 1000);
	}
}

/* ---- flash: OR several reads (errors are 1->0 only) ---- */

static unsigned int nc_flash_n(struct newscard_dev *nc)
{
	if (flash_reads)
		return min(flash_reads, 16u);
	return nc->bridge ? 1 : 5;
}

/* caller holds lock; off is a card offset */
static int nc_read_byte(struct newscard_dev *nc, u32 off, u8 *v)
{
	unsigned int i, n = off >= NC_FLASH ? nc_flash_n(nc) : 1;
	int ret = nc_page(nc, off);
	u8 x = 0;

	if (ret)
		return ret;
	for (i = 0; i < n; i++)
		x |= rd(nc, off);
	*v = x;
	return 0;
}

/* ---- commands and the message store, in the kernel (caller holds lock) ---- */

/* send cmd + payload, read rlen reply bytes; 0, -errno or NEWSCARD_NAK_BASE - code */
static int nc_cmd(struct newscard_dev *nc, u8 cmd, const u8 *payload, unsigned int plen,
		  u8 *reply, unsigned int rlen)
{
	u8 pkt[3 + 16];
	int ret;

	if (!nc->present)
		return -ENODEV;
	if (plen > 16)
		return -EINVAL;
	ret = nc_page(nc, 0);
	if (ret)
		return ret;
	pkt[0] = cmd;
	pkt[1] = plen >> 8;
	pkt[2] = plen & 0xFF;
	if (plen)
		memcpy(pkt + 3, payload, plen);
	ret = nc_send(nc, pkt, 3 + plen);
	if (!ret && rlen)
		ret = nc_receive(nc, cmd, reply, rlen);
	return ret;
}

static int nc_errno(int ret)
{
	return ret <= NEWSCARD_NAK_BASE ? -EIO : ret;
}

struct nc_rec {
	u32 ptr;
	u8 raw[64];
};

static bool nc_valid_ptr(u32 p)
{
	p &= 0x3FFFC;
	return p >= NC_HEAP && p + 64 <= NC_HEAP_END;
}

/*
 * Walk all buckets; call fn for each record. Pointers come from the card,
 * so every walk is bounded: masked pointers inside the heap, at most
 * NC_MAX_RECS records in total. Returns 0, an error, or fn's nonzero value.
 */
static int nc_walk(struct newscard_dev *nc,
		   int (*fn)(struct newscard_dev *, struct nc_rec *, void *),
		   void *arg)
{
	struct nc_rec *r;
	unsigned int k, total = 0;
	u8 head[4];
	int ret;

	r = kmalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	ret = nc_page(nc, 0);
	for (k = 0; !ret && k < NC_DIR_N; k++) {
		u32 p;

		ret = nc_locked_read(nc, NC_DIR + k * 0x80, head, 4);
		if (ret)
			break;
		p = get_unaligned_be32(head);
		while (p != 0xFFFFFFFF && nc_valid_ptr(p) && total++ < NC_MAX_RECS) {
			r->ptr = p & 0x3FFFC;
			ret = nc_locked_read(nc, r->ptr, r->raw, 64);
			if (ret)
				break;
			ret = fn(nc, r, arg);
			if (ret)
				break;
			p = get_unaligned_be32(r->raw);
		}
	}
	kfree(r);
	return ret;
}

static int nc_year(u8 y)
{
	return y >= 93 ? 1900 + y : 2000 + y;	/* card keeps a 2-digit year */
}

/* append one message (header lines + text) to the buffer */
struct nc_msgbuf {
	char *buf;
	size_t len, size;
};

static int nc_msg_text(struct newscard_dev *nc, struct nc_rec *r, struct nc_msgbuf *mb)
{
	const u8 *raw = r->raw;
	u32 first = get_unaligned_be32(raw + 20), text_end = get_unaligned_be32(raw + 24);
	unsigned int nblk = get_unaligned_be16(raw + 40), i, len = 0;
	u8 blk[64];
	u32 p = first, last = 0;
	long want;
	char *t;
	int ret, n;

	if (nblk > NC_MAX_BLOCKS)
		nblk = NC_MAX_BLOCKS;
	t = kmalloc(NC_MAX_BLOCKS * 54, GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	for (i = 0; i < nblk && p != 0xFFFFFFFF && nc_valid_ptr(p); i++) {
		ret = nc_locked_read(nc, p & 0x3FFFC, blk, 64);
		if (ret)
			goto out;
		if (i == 0)
			last = get_unaligned_be32(blk + 4);
		memcpy(t + len, blk + 10, 54);
		len += 54;
		p = get_unaligned_be32(blk);
	}
	/* raw (unmasked) pointers; text_end points at the last text byte */
	want = 54L * nblk - ((long)last + 64 - (long)text_end) + 1;
	if (want >= 0 && want <= len)
		len = want;
	while (len && t[len - 1] == 0)
		len--;
	n = snprintf(mb->buf + mb->len, mb->size - mb->len,
		     "message %u\nreceived %04d-%02u-%02u %02u:%02u\nslot %u\nfunction %c\n"
		     "state %s\nerrors %u\nrecord 0x%05x\nlength %u\n",
		     raw[9], nc_year(raw[45]), raw[46], raw[47], raw[48], raw[49],
		     raw[36] >> 3, (raw[36] & 7) < 4 ? "ABCD"[raw[36] & 7] : '?',
		     raw[38] & 4 ? "read" : "new", get_unaligned_be16(raw + 10), r->ptr, len);
	if (n < 0 || mb->len + n + len + 2 > mb->size) {
		ret = -ENOSPC;
		goto out;
	}
	mb->len += n;
	memcpy(mb->buf + mb->len, t, len);
	mb->len += len;
	mb->buf[mb->len++] = '\n';
	mb->buf[mb->len++] = '\n';
	ret = 0;
out:
	kfree(t);
	return ret;
}

struct nc_collect {
	struct nc_msg *m;
	unsigned int n, size;
	const u32 *skip;		/* keys to leave out (already returned) */
	unsigned int nskip;
};

static int nc_key_cmp(const void *a, const void *b)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)b;

	return x < y ? -1 : x > y;
}

static int nc_msg_cmp(const void *a, const void *b)
{
	const struct nc_msg *x = a, *y = b;
	int c = memcmp(x->when, y->when, 6);

	return c ? c : (x->key < y->key ? -1 : x->key > y->key);
}

static int nc_collect_cb(struct newscard_dev *nc, struct nc_rec *r, void *arg)
{
	struct nc_collect *c = arg;
	struct nc_msgbuf mb = { .size = 512 + NC_MAX_BLOCKS * 54 };
	u32 key = r->ptr << 8 | r->raw[9];
	struct nc_msg *m;
	int ret;

	if (c->skip && bsearch(&key, c->skip, c->nskip, sizeof(u32), nc_key_cmp))
		return 0;
	if (c->n == c->size) {
		unsigned int ns = c->size ? c->size * 2 : 32;
		struct nc_msg *nm = krealloc_array(c->m, ns, sizeof(*nm), GFP_KERNEL);

		if (!nm)
			return -ENOMEM;
		c->m = nm;
		c->size = ns;
	}
	mb.buf = kmalloc(mb.size, GFP_KERNEL);
	if (!mb.buf)
		return -ENOMEM;
	ret = nc_msg_text(nc, r, &mb);
	if (ret) {
		kfree(mb.buf);
		return ret;
	}
	m = &c->m[c->n++];
	m->key = key;
	memcpy(m->when, r->raw + 45, 5);
	m->when[0] = (r->raw[45] + 7) % 100;	/* 93..99 (1993..) before 00..92 (2000..) */
	m->when[5] = r->raw[9];
	m->text = mb.buf;
	m->len = mb.len - 1;		/* one record = header + text + one newline */
	return 0;
}

static void nc_free_queue(struct nc_file *nf)
{
	unsigned int i;

	for (i = 0; i < nf->qn; i++)
		kfree(nf->q[i].text);
	kfree(nf->q);
	nf->q = NULL;
	nf->qn = 0;
	nf->qpos = 0;
}

/*
 * Load the messages not returned yet into nf->q, oldest first (by
 * timestamp, then sequence number).
 */
static int nc_msgs_refresh(struct nc_file *nf)
{
	struct newscard_dev *nc = nf->nc;
	struct nc_collect c = { .skip = nf->seen, .nskip = nf->nseen };
	unsigned int i;
	u32 *ns;
	int ret;

	mutex_lock(&nc->lock);
	nf->events_seen = nc->events;
	ret = nc->present ? nc_walk(nc, nc_collect_cb, &c) : -ENODEV;
	mutex_unlock(&nc->lock);
	if (ret < 0) {
		for (i = 0; i < c.n; i++)
			kfree(c.m[i].text);
		kfree(c.m);
		return ret;
	}
	sort(c.m, c.n, sizeof(*c.m), nc_msg_cmp, NULL);
	ns = krealloc_array(nf->seen, nf->nseen + c.n + 1, sizeof(u32), GFP_KERNEL);
	if (!ns) {
		for (i = 0; i < c.n; i++)
			kfree(c.m[i].text);
		kfree(c.m);
		return -ENOMEM;
	}
	nf->seen = ns;
	for (i = 0; i < c.n; i++)
		nf->seen[nf->nseen++] = c.m[i].key;
	sort(nf->seen, nf->nseen, sizeof(u32), nc_key_cmp, NULL);
	nc_free_queue(nf);
	nf->q = c.m;
	nf->qn = c.n;
	return 0;
}

/* like /dev/kmsg: one message per read(); blocks for new pages */
static ssize_t nc_msgs_read(struct file *f, char __user *buf, size_t count, loff_t *ppos)
{
	struct nc_file *nf = f->private_data;
	struct newscard_dev *nc = nf->nc;
	struct nc_msg *m;
	int ret;

	while (nf->qpos >= nf->qn) {
		if (READ_ONCE(nc->events) == nf->events_seen) {
			if (!READ_ONCE(nc->present))
				return 0;
			if (f->f_flags & O_NONBLOCK)
				return -EAGAIN;
			ret = wait_event_interruptible(nc->wq,
						       READ_ONCE(nc->events) != nf->events_seen ||
						       !READ_ONCE(nc->present));
			if (ret)
				return ret;
			continue;
		}
		ret = nc_msgs_refresh(nf);
		if (ret)
			return ret;
	}
	m = &nf->q[nf->qpos];
	if (count < m->len)
		return -EINVAL;		/* as /dev/kmsg: the buffer must hold a whole message */
	if (copy_to_user(buf, m->text, m->len))
		return -EFAULT;
	nf->qpos++;
	*ppos += m->len;
	return m->len;
}

static __poll_t nc_msgs_poll(struct file *f, poll_table *wait)
{
	struct nc_file *nf = f->private_data;
	struct newscard_dev *nc = nf->nc;
	__poll_t mask = 0;

	poll_wait(f, &nc->wq, wait);
	if (nf->qpos < nf->qn || READ_ONCE(nc->events) != nf->events_seen)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!READ_ONCE(nc->present))
		mask |= EPOLLHUP;
	return mask;
}

/* lseek(fd, 0, SEEK_SET): start again with all messages on the card */
static loff_t nc_msgs_llseek(struct file *f, loff_t off, int whence)
{
	struct nc_file *nf = f->private_data;
	int ret;

	if (off != 0 || (whence != SEEK_SET && whence != SEEK_END))
		return -ESPIPE;
	if (whence == SEEK_SET) {
		kfree(nf->seen);
		nf->seen = NULL;
		nf->nseen = 0;
		ret = nc_msgs_refresh(nf);
		if (ret)
			return ret;
	} else {
		/* SEEK_END: skip what is there now, return only new pages */
		ret = nc_msgs_refresh(nf);
		if (ret)
			return ret;
		nf->qpos = nf->qn;
	}
	f->f_pos = 0;
	return 0;
}

/* find a record by sequence number or record offset */
struct nc_find {
	u32 key;
	struct nc_rec rec;
	bool found;
};

static int nc_find_cb(struct newscard_dev *nc, struct nc_rec *r, void *arg)
{
	struct nc_find *f = arg;

	if (r->raw[9] == f->key || r->ptr == f->key) {
		f->rec = *r;
		f->found = true;
		return 1;
	}
	return 0;
}

/* ---- new-page events ---- */

static void nc_poll_work(struct work_struct *w)
{
	struct newscard_dev *nc = container_of(to_delayed_work(w), struct newscard_dev, poll);
	bool ev = false;

	u8 r[6];

	mutex_lock(&nc->lock);
	if (nc->present && !nc_page(nc, 0)) {
		/* STAT bit 0: event pending */
		if (rd(nc, NC_STAT) & 1) {
			nc_clear_status(nc);	/* also sets event_pending for the EVENT ioctl */
			ev = true;
		}
		/* reliable: the total message count went up (command 0x82) */
		if (!nc_cmd(nc, 0x82, NULL, 0, r, 6)) {
			int total = get_unaligned_be16(r + 4);

			if (nc->last_total >= 0 && total > nc->last_total) {
				nc->events += total - nc->last_total;
				nc->event_pending = true;
				ev = true;
			}
			nc->last_total = total;
		}
	}
	mutex_unlock(&nc->lock);
	if (ev)
		wake_up_interruptible(&nc->wq);
	if (ev && nc->dev[0]) {
		sysfs_notify(&nc->dev[0]->kobj, NULL, "event");
		kobject_uevent(&nc->dev[0]->kobj, KOBJ_CHANGE);
	}
	if (poll_ms && nc->present)
		schedule_delayed_work(&nc->poll, msecs_to_jiffies(poll_ms));
}

/* ---- file operations ---- */

static int nc_open(struct inode *inode, struct file *f)
{
	unsigned int minor = iminor(inode) - MINOR(newscard_base);
	struct newscard_dev *nc;
	struct nc_file *nf;
	int ret;

	mutex_lock(&table_lock);
	nc = newscard_table[minor / NC_MINORS];
	if (nc)
		kref_get(&nc->ref);
	mutex_unlock(&table_lock);
	if (!nc)
		return -ENODEV;
	nf = kzalloc(sizeof(*nf), GFP_KERNEL);
	if (!nf) {
		ret = -ENOMEM;
		goto err;
	}
	nf->nc = nc;
	f->private_data = nf;
	/* minor 1 = attribute memory */
	if (minor % NC_MINORS == 1 && !nc->am) {
		ret = -ENODEV;
		goto err;
	}
	/* minor 2 = messages: load what is on the card now */
	if (minor % NC_MINORS == 2) {
		ret = nc_msgs_refresh(nf);
		if (ret < 0)
			goto err;
		f->f_mode &= ~(FMODE_PREAD | FMODE_PWRITE);
	}
	return 0;
err:
	kfree(nf);
	f->private_data = NULL;
	kref_put(&nc->ref, nc_free);
	return ret;
}

static int nc_release(struct inode *inode, struct file *f)
{
	struct nc_file *nf = f->private_data;

	nc_free_queue(nf);
	kfree(nf->seen);
	kref_put(&nf->nc->ref, nc_free);
	kfree(nf);
	return 0;
}

static unsigned int nc_minor_kind(struct file *f)
{
	struct newscard_dev *nc = NCF_DEV(f);

	return iminor(file_inode(f)) - MINOR(nc->devno);
}

static bool is_attr(struct file *f)
{
	return nc_minor_kind(f) == 1;
}

static ssize_t nc_rw(struct file *f, char __user *ubuf, size_t count, loff_t *ppos, bool write)
{
	struct newscard_dev *nc = NCF_DEV(f);
	bool attr = is_attr(f);
	u8 *kbuf;
	size_t i;
	ssize_t ret = 0;
	loff_t pos = *ppos;

	if (pos < 0)
		return -EINVAL;
	if (write && !allow_raw_write)
		return -EPERM;
	if (attr) {
		if (pos >= NC_ATTR_SIZE)
			return 0;
		count = min_t(size_t, count, NC_ATTR_SIZE - pos);
	} else if (pos > 0x03FFFFFF) {
		return 0;
	}
	count = min_t(size_t, count, PAGE_SIZE);
	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (write && copy_from_user(kbuf, ubuf, count)) {
		ret = -EFAULT;
		goto out;
	}

	mutex_lock(&nc->lock);
	if (!nc->present) {
		ret = -ENODEV;
		goto unlock;
	}
	for (i = 0; i < count; i++) {
		u32 off = pos + i;

		if (attr) {
			if (!nc->am) {
				ret = -ENODEV;
				goto unlock;
			}
			if (write)
				iowrite8(kbuf[i], nc->am + off);
			else
				kbuf[i] = ioread8(nc->am + off);
			continue;
		}
		if (write) {
			ret = nc_page(nc, off);
			if (!ret)
				wr(nc, off, kbuf[i]);
		} else {
			ret = nc_read_byte(nc, off, &kbuf[i]);
		}
		if (ret)
			goto unlock;
	}
	ret = count;
unlock:
	mutex_unlock(&nc->lock);
	if (ret > 0) {
		if (!write && copy_to_user(ubuf, kbuf, count))
			ret = -EFAULT;
		else
			*ppos = pos + count;
	}
out:
	kfree(kbuf);
	return ret;
}

static ssize_t nc_read(struct file *f, char __user *b, size_t c, loff_t *p)
{
	if (nc_minor_kind(f) == 2)
		return nc_msgs_read(f, b, c, p);
	return nc_rw(f, b, c, p, false);
}

static ssize_t nc_write(struct file *f, const char __user *b, size_t c, loff_t *p)
{
	if (nc_minor_kind(f) == 2)
		return -EPERM;
	return nc_rw(f, (char __user *)b, c, p, true);
}

static long nc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct newscard_dev *nc = NCF_DEV(f);
	long ret;

	switch (cmd) {
	case NEWSCARD_IOC_XFER: {
		struct newscard_xfer *x;
		u8 pkt[3 + NEWSCARD_DATA_MAX];

		x = memdup_user((void __user *)arg, sizeof(*x));
		if (IS_ERR(x))
			return PTR_ERR(x);
		if (x->len > NEWSCARD_DATA_MAX || x->len > NC_CMD_MAX - 3 ||
		    x->reply_len > NEWSCARD_DATA_MAX) {
			kfree(x);
			return -EINVAL;
		}
		pkt[0] = x->cmd;
		pkt[1] = x->len >> 8;
		pkt[2] = x->len & 0xFF;
		memcpy(pkt + 3, x->data, x->len);

		mutex_lock(&nc->lock);
		ret = nc->present ? nc_page(nc, 0) : -ENODEV;
		if (!ret)
			ret = nc_send(nc, pkt, 3 + x->len);
		if (!ret && x->reply_len)
			ret = nc_receive(nc, x->cmd, x->reply, x->reply_len);
		mutex_unlock(&nc->lock);

		x->result = ret;
		if (copy_to_user((void __user *)arg, x, sizeof(*x)))
			ret = -EFAULT;
		else if (ret <= NEWSCARD_NAK_BASE)
			ret = 0;	/* NAK: reported in x->result */
		kfree(x);
		return ret;
	}
	case NEWSCARD_IOC_LREAD: {
		struct newscard_lread *r;

		r = memdup_user((void __user *)arg, sizeof(*r));
		if (IS_ERR(r))
			return PTR_ERR(r);
		if (r->len > sizeof(r->data) || r->offset >= win_size ||
		    r->offset + r->len > win_size) {
			kfree(r);
			return -EINVAL;
		}
		mutex_lock(&nc->lock);
		ret = nc->present ? nc_page(nc, 0) : -ENODEV;
		if (!ret)
			ret = nc_locked_read(nc, r->offset, r->data, r->len);
		mutex_unlock(&nc->lock);
		r->result = ret;
		if (copy_to_user((void __user *)arg, r, sizeof(*r)))
			ret = -EFAULT;
		kfree(r);
		return ret;
	}
	case NEWSCARD_IOC_EVENT: {
		int ev;

		mutex_lock(&nc->lock);
		ret = nc->present ? nc_page(nc, 0) : -ENODEV;
		if (!ret)
			nc_clear_status(nc);
		ev = nc->event_pending;
		nc->event_pending = false;
		mutex_unlock(&nc->lock);
		if (ret)
			return ret;
		return put_user(ev, (int __user *)arg);
	}
	}
	return -ENOTTY;
}

static __poll_t nc_poll(struct file *f, poll_table *wait)
{
	if (nc_minor_kind(f) == 2)
		return nc_msgs_poll(f, wait);
	return EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM;
}

static loff_t nc_llseek(struct file *f, loff_t off, int whence)
{
	if (nc_minor_kind(f) == 2)
		return nc_msgs_llseek(f, off, whence);
	return default_llseek(f, off, whence);
}

static const struct file_operations nc_fops = {
	.owner = THIS_MODULE,
	.open = nc_open,
	.release = nc_release,
	.read = nc_read,
	.write = nc_write,
	.poll = nc_poll,
	.llseek = nc_llseek,
	.unlocked_ioctl = nc_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

/* ---- sysfs: /sys/class/newscard/newscardN/ ---- */

static int nc_query(struct device *dev, u8 cmd, const u8 *pl, unsigned int plen,
		    u8 *rep, unsigned int rlen)
{
	struct newscard_dev *nc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&nc->lock);
	ret = nc_cmd(nc, cmd, pl, plen, rep, rlen);
	mutex_unlock(&nc->lock);
	return nc_errno(ret);
}

static ssize_t battery_show(struct device *dev, struct device_attribute *a, char *buf)
{
	u8 r[5];
	int ret = nc_query(dev, 0x44, NULL, 0, r, 5);

	return ret ? ret : sysfs_emit(buf, "%s\n", r[4] ? "ok" : "low");
}
static DEVICE_ATTR_RO(battery);

static ssize_t nc_u16(struct device *dev, u8 cmd, char *buf)
{
	u8 r[6];
	int ret = nc_query(dev, cmd, NULL, 0, r, 6);

	return ret ? ret : sysfs_emit(buf, "%u\n", get_unaligned_be16(r + 4));
}

static ssize_t total_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return nc_u16(dev, 0x82, buf);
}
static DEVICE_ATTR_RO(total);

static ssize_t unread_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return nc_u16(dev, 0x81, buf);
}
static DEVICE_ATTR_RO(unread);

static int nc_status(struct device *dev, u8 *st)
{
	u8 r[24];
	int ret = nc_query(dev, 0x83, NULL, 0, r, 24);

	if (!ret)
		memcpy(st, r + 4, 20);
	return ret;
}

static ssize_t status_show(struct device *dev, struct device_attribute *a, char *buf)
{
	u8 st[20];
	int ret = nc_status(dev, st);

	return ret ? ret : sysfs_emit(buf, "%20ph\n", st);
}
static DEVICE_ATTR_RO(status);

/* status byte 1: 04 on (beep), 02 silent, 01 off */
static ssize_t switch_show(struct device *dev, struct device_attribute *a, char *buf)
{
	u8 st[20];
	int ret = nc_status(dev, st);

	if (ret)
		return ret;
	switch (st[1]) {
	case 0x04: return sysfs_emit(buf, "on\n");
	case 0x02: return sysfs_emit(buf, "silent\n");
	case 0x01: return sysfs_emit(buf, "off\n");
	}
	return sysfs_emit(buf, "unknown %02x\n", st[1]);
}
static DEVICE_ATTR_RO(switch);

static ssize_t free_blocks_show(struct device *dev, struct device_attribute *a, char *buf)
{
	u8 st[20];
	int ret = nc_status(dev, st);

	return ret ? ret : sysfs_emit(buf, "%u\n", get_unaligned_be16(st + 18));
}
static DEVICE_ATTR_RO(free_blocks);

static ssize_t clock_show(struct device *dev, struct device_attribute *a, char *buf)
{
	u8 r[9];
	int ret = nc_query(dev, 0x32, NULL, 0, r, 9);

	return ret ? ret : sysfs_emit(buf, "%04d-%02u-%02u %02u:%02u\n",
				      nc_year(r[4]), r[5], r[6], r[7], r[8]);
}

/* write "YYYY-MM-DD hh:mm" (local time is the caller's business) */
static ssize_t clock_store(struct device *dev, struct device_attribute *a,
			   const char *buf, size_t n)
{
	unsigned int y, mo, d, h, mi;
	u8 pl[5], r[5];
	int ret;

	if (sscanf(buf, "%u-%u-%u %u:%u", &y, &mo, &d, &h, &mi) != 5 ||
	    y < 1993 || y > 2092 || !mo || mo > 12 || !d || d > 31 || h > 23 || mi > 59)
		return -EINVAL;
	pl[0] = y % 100;	/* the card NAKs year bytes > 99 */
	pl[1] = mo; pl[2] = d; pl[3] = h; pl[4] = mi;
	ret = nc_query(dev, 0x31, pl, 5, r, 5);
	return ret ? ret : n;
}
static DEVICE_ATTR_RW(clock);

/*
 * alerts: config byte 0xD6, bits 0xC0 (on = 0xC0, off = 0x00). With
 * alerts off a page is stored silently; its alert sounds once when alerts
 * are switched back on.
 */
static ssize_t alerts_show(struct device *dev, struct device_attribute *a, char *buf)
{
	u8 pl[2] = { 0x00, 0xD6 }, r[7];
	int ret = nc_query(dev, 0x11, pl, 2, r, 7);

	return ret ? ret : sysfs_emit(buf, "%s\n", (r[6] & 0xC0) ? "on" : "off");
}

static ssize_t alerts_store(struct device *dev, struct device_attribute *a,
			    const char *buf, size_t n)
{
	u8 pl[4] = { 0x00, 0xD6, 0x00, 0xC0 }, r[5];
	bool on;
	int ret;

	if (sysfs_streq(buf, "on"))
		on = true;
	else if (sysfs_streq(buf, "off"))
		on = false;
	else if (kstrtobool(buf, &on))
		return -EINVAL;
	pl[2] = on ? 0xC0 : 0x00;
	ret = nc_query(dev, 0x12, pl, 4, r, 5);
	return ret ? ret : n;
}
static DEVICE_ATTR_RW(alerts);

static ssize_t event_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct newscard_dev *nc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(nc->events));
}
static DEVICE_ATTR_RO(event);

/* markread / delete: write a sequence number (or record offset 0x...) */
static ssize_t nc_msg_op(struct device *dev, const char *buf, size_t n, bool del)
{
	struct newscard_dev *nc = dev_get_drvdata(dev);
	struct nc_find *fd;
	u8 pl[8], r[5];
	u32 key;
	int ret;

	if (kstrtou32(buf, 0, &key))
		return -EINVAL;
	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;
	fd->key = key;
	mutex_lock(&nc->lock);
	ret = nc->present ? nc_walk(nc, nc_find_cb, fd) : -ENODEV;
	if (ret >= 0 && !fd->found)
		ret = -ENOENT;
	if (ret >= 0) {
		put_unaligned_be32(fd->rec.ptr, pl);
		pl[4] = fd->rec.raw[9];
		memset(pl + 5, 0, 3);
		ret = nc_cmd(nc, del ? 0x02 : 0x01, pl, del ? 8 : 5, r, 5);
	}
	mutex_unlock(&nc->lock);
	kfree(fd);
	ret = nc_errno(ret);
	return ret ? ret : n;
}

static ssize_t markread_store(struct device *dev, struct device_attribute *a,
			      const char *buf, size_t n)
{
	return nc_msg_op(dev, buf, n, false);
}
static DEVICE_ATTR_WO(markread);

static ssize_t delete_store(struct device *dev, struct device_attribute *a,
			    const char *buf, size_t n)
{
	return nc_msg_op(dev, buf, n, true);
}
static DEVICE_ATTR_WO(delete);

static struct attribute *nc_attrs[] = {
	&dev_attr_battery.attr, &dev_attr_switch.attr, &dev_attr_clock.attr, &dev_attr_alerts.attr,
	&dev_attr_total.attr, &dev_attr_unread.attr, &dev_attr_free_blocks.attr,
	&dev_attr_status.attr, &dev_attr_event.attr,
	&dev_attr_markread.attr, &dev_attr_delete.attr,
	NULL,
};
ATTRIBUTE_GROUPS(nc);

/* ---- MTD: the flash, read-only ---- */

static int nc_mtd_read(struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	struct newscard_dev *nc = mtd->priv;
	size_t i;
	int ret = 0;

	*retlen = 0;
	mutex_lock(&nc->lock);
	for (i = 0; i < len; i++) {
		if (!nc->present) {
			ret = -ENODEV;
			break;
		}
		ret = nc_read_byte(nc, NC_FLASH + from + i, &buf[i]);
		if (ret)
			break;
		if ((i & 4095) == 4095) {
			mutex_unlock(&nc->lock);
			cond_resched();
			mutex_lock(&nc->lock);
		}
	}
	mutex_unlock(&nc->lock);
	*retlen = i;
	return ret;
}

/* keep nc alive while the MTD is open (the card may be ejected meanwhile) */
static int nc_mtd_get(struct mtd_info *mtd)
{
	struct newscard_dev *nc = mtd->priv;

	kref_get(&nc->ref);
	return 0;
}

static void nc_mtd_put(struct mtd_info *mtd)
{
	struct newscard_dev *nc = mtd->priv;

	/* last call in __put_mtd_device; it does not touch mtd afterwards */
	kref_put(&nc->ref, nc_free);
}

static void nc_mtd_register(struct newscard_dev *nc)
{
	struct mtd_info *m = &nc->mtd;

	snprintf(nc->mtd_name, sizeof(nc->mtd_name), "newscard%d-flash", nc->id);
	m->name = nc->mtd_name;
	m->type = MTD_ROM;
	m->flags = MTD_CAP_ROM | MTD_NO_ERASE;	/* read-only: no _erase, no _write */
	m->size = NC_FLASH_SIZE;
	m->erasesize = NC_FLASH_SIZE;
	m->writesize = 1;
	m->writebufsize = 1;
	m->owner = THIS_MODULE;
	m->priv = nc;
	m->dev.parent = &nc->link->dev;
	m->_read = nc_mtd_read;
	m->_get_device = nc_mtd_get;
	m->_put_device = nc_mtd_put;
	if (mtd_device_register(m, NULL, 0))
		dev_warn(&nc->link->dev, "flash: MTD registration failed\n");
	else
		nc->mtd_registered = true;
}

/* ---- PCMCIA ---- */

static void nc_set_bridge_timing(struct newscard_dev *nc)
{
	struct device *parent = nc->link->socket->dev.parent;
	struct pci_dev *pdev;
	u16 ctl, old;

	if (!ricoh_mem_timing)
		return;
	if (!parent || !dev_is_pci(parent)) {
		dev_warn(&nc->link->dev, "bridge is not PCI; flash reads may drop bits\n");
		return;
	}
	pdev = to_pci_dev(parent);
	if (pdev->vendor != PCI_VENDOR_ID_RICOH) {
		dev_warn(&nc->link->dev,
			 "bridge %04x:%04x is not Ricoh; timing unchanged, flash reads may drop bits (check with probe)\n",
			 pdev->vendor, pdev->device);
		return;
	}
	pci_read_config_word(pdev, RL5C4XX_16BIT_CTL, &ctl);
	if (!(ctl & RL5C4XX_16CTL_MEM_TIMING)) {
		dev_warn(&nc->link->dev,
			 "Ricoh 16-bit memory timing not enabled (ctl %04x); unchanged\n", ctl);
		return;
	}
	pci_read_config_word(pdev, RL5C4XX_16BIT_MEM_0, &old);
	pci_write_config_word(pdev, RL5C4XX_16BIT_MEM_0, ricoh_mem_timing);
	nc->bridge = pci_dev_get(pdev);
	nc->saved_mem_timing = old;
	dev_info(&nc->link->dev, "Ricoh 16-bit memory timing %04x -> %04x\n",
		 old, ricoh_mem_timing);
}

static void nc_restore_bridge_timing(struct newscard_dev *nc)
{
	if (!nc->bridge)
		return;
	pci_write_config_word(nc->bridge, RL5C4XX_16BIT_MEM_0, nc->saved_mem_timing);
	pci_dev_put(nc->bridge);
	nc->bridge = NULL;
}

/*
 * Release both memory windows explicitly before pcmcia_disable_device().
 * (An attribute window requested at more than 1000 ns used to fail half way
 * and stay allocated; it is clamped now, and this also covers error paths.)
 */
static void nc_release_windows(struct pcmcia_device *link)
{
	int i;

	for (i = 2; i <= 3; i++)
		if (link->resource[i]->flags & WIN_FLAGS_REQ)
			pcmcia_release_window(link, link->resource[i]);
}

static int nc_probe(struct pcmcia_device *link)
{
	struct newscard_dev *nc;
	struct resource *am_res;
	int id, ret;

	/*
	 * NewsCard only. The ID table already matches these two strings by
	 * hash; check them exactly as well, before anything is mapped or
	 * written, so a manual bind or new_id can never make this driver write
	 * its mailbox and paging registers into another card.
	 */
	if (!link->prod_id[0] || !link->prod_id[1] ||
	    strcmp(link->prod_id[0], "Motorola, Inc.") ||
	    strcmp(link->prod_id[1], "NewsCard Data Receiver")) {
		dev_info(&link->dev, "not a Motorola NewsCard (\"%s\" / \"%s\"), not claimed\n",
			 link->prod_id[0] ? link->prod_id[0] : "",
			 link->prod_id[1] ? link->prod_id[1] : "");
		return -ENODEV;
	}

	if (!win_size || (win_size & (win_size - 1)))
		return -EINVAL;

	nc = kzalloc(sizeof(*nc), GFP_KERNEL);
	if (!nc)
		return -ENOMEM;
	nc->link = link;
	link->priv = nc;
	mutex_init(&nc->lock);
	kref_init(&nc->ref);
	init_waitqueue_head(&nc->wq);

	/* common memory window */
	nc->cm_res = link->resource[2];
	nc->cm_res->flags |= WIN_MEMORY_TYPE_CM | WIN_ENABLE |
		(width16 ? WIN_DATA_WIDTH_16 : WIN_DATA_WIDTH_8) |
		(use_wait ? WIN_USE_WAIT : 0);
	nc->cm_res->start = 0;
	nc->cm_res->end = win_size;
	ret = pcmcia_request_window(link, nc->cm_res, speed_ns);
	if (ret) {
		dev_err(&link->dev, "common memory window: %d\n", ret);
		goto err_free;
	}
	ret = pcmcia_map_mem_page(link, nc->cm_res, 0);
	if (ret)
		goto err_free;
	nc->cm = ioremap(nc->cm_res->start, resource_size(nc->cm_res));
	if (!nc->cm) {
		ret = -ENOMEM;
		goto err_free;
	}
	if (resource_size(nc->cm_res) < win_size)
		dev_warn(&link->dev, "window is only %llu bytes\n",
			 (unsigned long long)resource_size(nc->cm_res));

	/* attribute memory window (optional, for testing) */
	am_res = link->resource[3];
	am_res->flags |= WIN_MEMORY_TYPE_AM | WIN_ENABLE | WIN_DATA_WIDTH_8;
	am_res->start = 0;
	am_res->end = NC_ATTR_SIZE;
	/* yenta rejects window speeds above 1000 ns */
	ret = pcmcia_request_window(link, am_res, speed_ns ? min(speed_ns * 2, 1000u) : 0);
	if (!ret)
		ret = pcmcia_map_mem_page(link, am_res, 0);
	if (!ret)
		nc->am = ioremap(am_res->start, NC_ATTR_SIZE);
	if (!nc->am && (am_res->flags & WIN_FLAGS_REQ))
		pcmcia_release_window(link, am_res);
	if (!nc->am)
		dev_info(&link->dev, "no attribute memory window (%d, %pR)\n",
			 ret, am_res);

	ret = pcmcia_enable_device(link);
	if (ret) {
		dev_err(&link->dev, "pcmcia_enable_device: %d\n", ret);
		goto err_unmap;
	}
	nc_set_bridge_timing(nc);

	id = ida_alloc_max(&newscard_ida, 15, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto err_disable;
	}
	nc->id = id;
	nc->devno = MKDEV(MAJOR(newscard_base), MINOR(newscard_base) + id * NC_MINORS);
	nc->cdev = cdev_alloc();
	if (!nc->cdev) {
		ret = -ENOMEM;
		goto err_ida;
	}
	nc->cdev->ops = &nc_fops;
	nc->cdev->owner = THIS_MODULE;
	ret = cdev_add(nc->cdev, nc->devno, NC_MINORS);
	if (ret) {
		kobject_put(&nc->cdev->kobj);
		goto err_ida;
	}
	mutex_lock(&table_lock);
	newscard_table[id] = nc;
	mutex_unlock(&table_lock);
	nc->present = true;
	INIT_DELAYED_WORK(&nc->poll, nc_poll_work);
	nc->last_total = -1;
	nc->dev[0] = device_create_with_groups(newscard_class, &link->dev, nc->devno, nc,
					       nc_groups, "newscard%d", id);
	nc->dev[1] = device_create(newscard_class, &link->dev, nc->devno + 1, nc,
				   "newscard%dattr", id);
	nc->dev[2] = device_create(newscard_class, &link->dev, nc->devno + 2, nc,
				   "newscard%dmsgs", id);
	if (IS_ERR(nc->dev[0]))
		nc->dev[0] = NULL;
	nc_mtd_register(nc);
	if (poll_ms)
		schedule_delayed_work(&nc->poll, msecs_to_jiffies(poll_ms));

	dev_info(&link->dev,
		 "NewsCard: common window %pR (%s, %u ns), attr %s, flash reads x%u%s; mailbox STAT=%02x OWN=%02x reply=%02x %02x %02x %02x\n",
		 nc->cm_res, width16 ? "16 bit" : "8 bit", speed_ns,
		 nc->am ? "mapped" : "none", nc_flash_n(nc),
		 nc->mtd_registered ? ", MTD" : "",
		 rd(nc, NC_STAT), rd(nc, NC_OWN), rd(nc, NC_RSP), rd(nc, NC_RSP + 1),
		 rd(nc, NC_RSP + 2), rd(nc, NC_RSP + 3));
	return 0;

err_ida:
	ida_free(&newscard_ida, id);
err_disable:
	nc_restore_bridge_timing(nc);
	nc_release_windows(link);
	pcmcia_disable_device(link);
err_unmap:
	if (nc->am)
		iounmap(nc->am);
	iounmap(nc->cm);
err_free:
	nc_release_windows(link);
	kfree(nc);
	link->priv = NULL;
	return ret;
}

static void nc_remove(struct pcmcia_device *link)
{
	struct newscard_dev *nc = link->priv;
	int i;

	if (!nc)
		return;
	mutex_lock(&table_lock);
	newscard_table[nc->id] = NULL;
	mutex_unlock(&table_lock);
	if (nc->mtd_registered)
		mtd_device_unregister(&nc->mtd);	/* open users keep nc via _get_device */
	/* open files keep nc alive but see present == false */
	mutex_lock(&nc->lock);
	nc->present = false;
	mutex_unlock(&nc->lock);
	wake_up_interruptible_all(&nc->wq);	/* blocked readers return EOF */
	cancel_delayed_work_sync(&nc->poll);
	mutex_lock(&nc->lock);
	if (nc->am)
		iounmap(nc->am);
	nc->am = NULL;
	iounmap(nc->cm);
	nc->cm = NULL;
	mutex_unlock(&nc->lock);
	for (i = 0; i < NC_MINORS; i++)
		if (!IS_ERR_OR_NULL(nc->dev[i]))
			device_destroy(newscard_class, nc->devno + i);
	cdev_del(nc->cdev);
	ida_free(&newscard_ida, nc->id);
	nc_restore_bridge_timing(nc);
	nc_release_windows(link);
	pcmcia_disable_device(link);
	link->priv = NULL;
	kref_put(&nc->ref, nc_free);
}

static const struct pcmcia_device_id nc_ids[] = {
	PCMCIA_DEVICE_PROD_ID12("Motorola, Inc.", "NewsCard Data Receiver",
				0x67bf97be, 0x7e40b37d),
	PCMCIA_DEVICE_NULL,
};
MODULE_DEVICE_TABLE(pcmcia, nc_ids);

static struct pcmcia_driver nc_driver = {
	.owner = THIS_MODULE,
	.name = DRIVER_NAME,
	.probe = nc_probe,
	.remove = nc_remove,
	.id_table = nc_ids,
};

static int __init nc_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&newscard_base, 0, 16 * NC_MINORS, DRIVER_NAME);
	if (ret)
		return ret;
	newscard_class = class_create(DRIVER_NAME);
	if (IS_ERR(newscard_class)) {
		unregister_chrdev_region(newscard_base, 16 * NC_MINORS);
		return PTR_ERR(newscard_class);
	}
	ret = pcmcia_register_driver(&nc_driver);
	if (ret) {
		class_destroy(newscard_class);
		unregister_chrdev_region(newscard_base, 16 * NC_MINORS);
	}
	return ret;
}

static void __exit nc_exit(void)
{
	pcmcia_unregister_driver(&nc_driver);
	class_destroy(newscard_class);
	unregister_chrdev_region(newscard_base, 16 * NC_MINORS);
}

module_init(nc_init);
module_exit(nc_exit);

MODULE_DESCRIPTION("Motorola NewsCard (Newton Messaging Card) POCSAG pager PC Card");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");
