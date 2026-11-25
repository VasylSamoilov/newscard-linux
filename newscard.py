#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 Vasyl Samoilov <vasyl.samoilov@gmail.com>
"""newscard.py - talk to the Motorola NewsCard through newscard.ko.

Protocol: PROTOCOL.md.

Read-only:   probe, info, show, list, watch, dump  (show/list also work on a
             saved RAM dump: --image ram.bin [--flash flash.bin])
Changes the card (need --yes):  settime, markread, delete, raw
"""

import argparse
import datetime
import fcntl
import os
import struct
import sys
import time

DEV = "/dev/newscard0"
ATTR = "/dev/newscard0attr"

DATA_MAX = 256
NAK_BASE = -1000
XFER_FMT = "=BBHHHi%ds%ds" % (DATA_MAX, DATA_MAX)
LREAD_FMT = "=IIiI1024s"


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("N") << 8) | nr


IOC_XFER = _ioc(3, 1, struct.calcsize(XFER_FMT))
IOC_LREAD = _ioc(3, 2, struct.calcsize(LREAD_FMT))
IOC_EVENT = _ioc(2, 3, 4)

# Card offsets (common memory)
CMD, SEM, STAT, OWN, DOORBELL, RSP, DIR = 0xC00, 0xFF0, 0xFFD, 0xFFE, 0xFFF, 0x1000, 0x1400
FLASH = 0x02000000

# command byte -> (name, reply length incl. header and status)
#   0x12 write config: addrHi addrLo value mask; alerts on/off = 0xD6,
#        value 0xC0/0x00, mask 0xC0 (see cmd_alerts)
#   0x41 alert select: one byte; 0x13 notify: no payload, no reply
COMMANDS = {
    0x01: ("mark read", 5), 0x02: ("delete", 5), 0x11: ("read config", 7),
    0x12: ("write config", 5), 0x13: ("notify", 0), 0x31: ("set time", 5),
    0x32: ("get time", 9), 0x41: ("alert select", 5), 0x44: ("battery", 5),
    0x81: ("unread count", 7), 0x82: ("total count", 7), 0x83: ("status", 24),
}


class CardError(Exception):
    pass


class NewsCard:
    def __init__(self, path=DEV):
        self.fd = os.open(path, os.O_RDWR)

    # ---- raw access ----
    def read(self, off, n):
        # the driver returns at most one page per call
        out = bytearray()
        while len(out) < n:
            chunk = os.pread(self.fd, n - len(out), off + len(out))
            if not chunk:
                break
            out += chunk
        return bytes(out)

    def write(self, off, data):
        return os.pwrite(self.fd, bytes(data), off)

    # ---- mailbox ----
    def xfer(self, cmd, payload=b"", reply_len=None):
        if reply_len is None:
            reply_len = COMMANDS.get(cmd, ("?", 5))[1]
        buf = bytearray(struct.pack(XFER_FMT, cmd, 0, len(payload), reply_len, 0, 0,
                                    bytes(payload), b""))
        fcntl.ioctl(self.fd, IOC_XFER, buf)
        _, _, _, _, _, result, _, reply = struct.unpack(XFER_FMT, buf)
        reply = reply[:reply_len]
        if result <= NAK_BASE:
            raise CardError("command %02X: card answered NAK, code %d" % (cmd, NAK_BASE - result))
        if result < 0:
            raise CardError("command %02X: %s" % (cmd, os.strerror(-result)))
        return reply

    def data(self, cmd, payload=b""):
        """Reply data after [cmd, lenHi, lenLo, status]."""
        r = self.xfer(cmd, payload)
        return r[4:]

    def lread(self, off, n):
        buf = bytearray(struct.pack(LREAD_FMT, off, n, 0, 0, b""))
        fcntl.ioctl(self.fd, IOC_LREAD, buf)
        _, _, result, _, data = struct.unpack(LREAD_FMT, buf)
        if result < 0:
            raise CardError("locked read %05X: %s" % (off, os.strerror(-result)))
        return data[:n]

    def event(self):
        buf = bytearray(4)
        fcntl.ioctl(self.fd, IOC_EVENT, buf)
        return struct.unpack("=i", buf)[0] != 0

    # ---- commands ----
    def battery_ok(self):
        return self.data(0x44)[0] != 0

    def unread(self):
        d = self.data(0x81)
        return d[0] << 8 | d[1]

    def total(self):
        d = self.data(0x82)
        return d[0] << 8 | d[1]

    def status(self):
        return self.data(0x83)[:20]

    def get_time(self):
        return tuple(self.data(0x32)[:5])

    def read_config(self, addr):
        return self.xfer(0x11, bytes([addr >> 8, addr & 0xFF]))[6]

    # ---- message store ----
    def records(self):
        """All message records, sorted by (timestamp, seq)."""
        recs = []
        for k in range(16):
            p = struct.unpack(">I", self.lread(DIR + k * 0x80, 4))[0]
            guard = 0
            while p != 0xFFFFFFFF and guard < 2000:
                ptr = p & 0x3FFFC
                raw = self.lread(ptr, 64)
                recs.append(parse_record(ptr, raw, k))
                p = struct.unpack(">I", raw[0:4])[0]
                guard += 1
        recs.sort(key=lambda r: (r["raw"][45:50], r["seq"]))
        return recs

    def text(self, rec):
        n = rec["blocks"]
        first = rec["first_block"]
        if n == 0 or first == 0xFFFFFFFF:
            return b""
        blk0 = self.lread(first & 0x3FFFC, 64)
        # Raw (unmasked) pointers; text_end points at the last text byte.
        last = struct.unpack(">I", blk0[4:8])[0]
        length = 54 * n - (last + 64 - rec["text_end"]) + 1
        out = bytearray()
        p = first
        for i in range(n):
            blk = blk0 if i == 0 else self.lread(p & 0x3FFFC, 64)
            out += blk[10:64]
            p = struct.unpack(">I", blk[0:4])[0]
            if p == 0xFFFFFFFF:
                break
        if 0 <= length <= len(out):
            out = out[:length]
        return bytes(out).rstrip(b"\x00")


class Image(NewsCard):
    """A saved RAM dump (card offsets 0..0x1FFFF) instead of the device;
    store reads only, no mailbox commands."""

    def __init__(self, path, flash=None):
        self.d = open(path, "rb").read()
        self.flash = open(flash, "rb").read() if flash else None

    def lread(self, off, n):
        return self.d[off:off + n]

    def read(self, off, n):
        if off >= 0x02000000 and self.flash is not None:
            o = off - 0x02000000
            return self.flash[o:o + n]
        return self.d[off:off + n]

    def xfer(self, cmd, payload=b"", reply_len=None):
        raise CardError("offline image: no mailbox command 0x%02x" % cmd)


def open_card(args):
    if getattr(args, "image", None):
        return Image(args.image, getattr(args, "flash", None))
    return NewsCard(args.dev)


def parse_record(ptr, raw, bucket):
    u16 = lambda o: raw[o] << 8 | raw[o + 1]
    u32 = lambda o: struct.unpack(">I", raw[o:o + 4])[0]
    return {
        "ptr": ptr, "bucket": bucket, "raw": raw,
        "seq": raw[9], "errors": u16(10), "first_block": u32(20), "text_end": u32(24),
        "source": raw[36], "b37": raw[37], "flags": raw[38], "read": bool(raw[38] & 4),
        "blocks": u16(40), "time": tuple(raw[45:50]), "b50": raw[50],
    }


def card_year(y):
    """The card keeps a two-digit year (it NAKs > 99). It starts at 93 (1993)
    when switched on; read 00..92 as 20xx."""
    return 1900 + y if y >= 93 else 2000 + y


def fmt_time(t):
    y, mo, d, h, mi = t
    return "%04d-%02d-%02d %02d:%02d" % (card_year(y), mo, d, h, mi)


SWITCH = {0x04: "on (beep)", 0x02: "silent", 0x01: "off"}
FUNC = "ABCD"


def parse_rics(spec):
    """'1234567,7654321' -> {0: 1234567, 1: 7654321} (address slot order)"""
    if not spec:
        return {}
    return {i: int(x) for i, x in enumerate(spec.split(",")) if x}


def who(rec, rics):
    slot, fn = rec["source"] >> 3, rec["source"] & 7
    name = ("RIC %07d" % rics[slot]) if slot in rics else ("slot %d" % slot)
    return "%s func %s" % (name, FUNC[fn] if fn < 4 else "?%d" % fn)


def hexs(b):
    return " ".join("%02x" % x for x in b)


# ---- commands ----

def cmd_probe(args):
    """Read-only checks for first contact: no mailbox command is sent."""
    card = NewsCard(args.dev)
    print("== attribute memory (CIS at even bytes)")
    try:
        with open(ATTR, "rb") as f:
            a = f.read(0x80)
        print("  ", hexs(a[0:32:2]))
    except OSError as e:
        print("   not available:", e)
    print("== common 0x30.. (CIS strings, every byte)")
    c = card.read(0x30, 0x50)
    print("  ", c[0x15:0x4a].decode("latin1", "replace"))
    print("== mailbox registers 0xFF0..0xFFF")
    print("  ", hexs(card.read(SEM, 16)))
    print("   STAT=%02x OWN=%02x" % (card.read(STAT, 1)[0], card.read(OWN, 1)[0]))
    print("== reply buffer 0x1000")
    r = card.read(RSP, 16)
    print("  ", hexs(r), "  (cmd %02x, len %d, status %02x)" % (r[0], r[1] << 8 | r[2], r[3]))
    print("== command buffer 0xC00, odd vs even bytes")
    cb = card.read(CMD, 64)
    print("  ", hexs(cb[:32]))
    print("   nonzero even bytes: %d/32, odd bytes: %d/32" %
          (sum(1 for x in cb[0::2] if x), sum(1 for x in cb[1::2] if x)))
    print("== directory heads 0x1400 + k*0x80")
    heads = [struct.unpack(">I", card.read(DIR + k * 0x80, 4))[0] for k in range(16)]
    print("  ", " ".join("%08x" % h for h in heads))
    print("== flash package header at 0x02000080 (stored word-reversed)")
    f = card.read(FLASH + 0x80, 16)
    unswapped = b"".join(f[i:i + 4][::-1] for i in range(0, 16, 4))
    print("   raw %s -> %r" % (hexs(f), unswapped))
    print("== read stability: flash 0x02000000..+0x2000, 3 reads")
    reads = [card.read(FLASH, 0x2000) for _ in range(3)]
    diff = sum(1 for i in range(0x2000) if len({r[i] for r in reads}) > 1)
    ored = bytes(a | b | c for a, b, c in zip(*reads))
    drops = sum(bin(ored[i] & ~reads[j][i] & 0xFF).count("1") for j in range(3) for i in range(0x2000))
    print("   bytes that differ between reads: %d, bits read as 0 vs the OR: %d" % (diff, drops))
    print("   (both 0 = reads OK; otherwise check ricoh_mem_timing / the bridge timing, see README)")


def cmd_info(args):
    card = NewsCard(args.dev)
    for label, fn in (("battery OK", card.battery_ok), ("total", card.total),
                      ("unread", card.unread)):
        try:
            print("%-12s %s" % (label, fn()))
        except CardError as e:
            print("%-12s ERROR %s" % (label, e))
    try:
        s = card.status()
        print("%-12s %s" % ("status", hexs(s)))
        print("%-12s %d bytes (%d blocks)" % ("free", (s[18] << 8 | s[19]) * 64, s[18] << 8 | s[19]))
    except CardError as e:
        print("status       ERROR", e)
    try:
        print("%-12s %s" % ("pager time", fmt_time(card.get_time())))
    except CardError as e:
        print("pager time   ERROR", e)
    for addr, name in ((0xD6, "alerts (0xC0=on)"), (0xE7, "alert type"), (0xBF, "config 0xBF")):
        try:
            print("%-12s %02x  %s" % ("cfg %02X" % addr, card.read_config(addr), name))
        except CardError as e:
            print("cfg %02X       ERROR %s" % (addr, e))


def cmd_list(args):
    card = open_card(args)
    rics = parse_rics(args.rics)
    recs = card.records()
    print("%d message(s)" % len(recs))
    for r in recs:
        print("--- #%d  received %s  %s  %s  record %05x  errors %d  flags %02x" % (
            r["seq"], fmt_time(r["time"]), who(r, rics), "read" if r["read"] else "new",
            r["ptr"], r["errors"], r["flags"]))
        if args.raw:
            print("    record:", hexs(r["raw"]))
        try:
            t = card.text(r).decode("latin1", "replace")
        except CardError as e:
            print("    text ERROR", e)
            continue
        # pages of the form from|subject|body
        f = t.split("|", 2)
        if len(f) == 3:
            print("    From:    %s" % f[0][:27])
            print("    Subject: %s" % f[1][:30])
            print("    Message: %s" % f[2])
        else:
            print("   ", t)


# ---- show: status panel and memory maps ----

HEAP_START, HEAP_END = 0x1C00, 0x20000     # 64-byte blocks after the directory


def _tty():
    return sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def _c(code, text):
    return "\033[%sm%s\033[0m" % (code, text) if _tty() else text


def store_map(card):
    """block index -> (kind, source) for every block reachable from the directory"""
    used = {}
    for r in card.records():
        used[(r["ptr"] - HEAP_START) // 64] = ("R", r["source"], r["read"])
        p, n = r["first_block"], 0
        while p != 0xFFFFFFFF and n < r["blocks"] and n < 64:
            b = ((p & 0x3FFFC) - HEAP_START) // 64
            used[b] = ("T", r["source"], r["read"])
            p = struct.unpack(">I", card.lread(p & 0x3FFFC, 4))[0]
            n += 1
    return used


def flash_regions(fl):
    """1 KB chunks of the flash -> class letter"""
    plen = 0
    hdr = bytes(fl[0x80 + i ^ 3] for i in range(0x20))      # words stored reversed
    if hdr[:8] == b"package0":
        plen = struct.unpack(">I", hdr[0x1C:0x20])[0]
    out = []
    for k in range(len(fl) // 1024):
        c = fl[k * 1024:(k + 1) * 1024]
        lo, hi = k * 1024, k * 1024 + 1024
        if lo < 0x80:
            out.append("C")
        elif plen and lo < 0x80 + plen:
            out.append("P")
        elif all(b == 0xFF for b in c):
            out.append("-")
        elif sum(1 for b in c if (b & ~0x55) == 0 or (b & ~0xAA) == 0) > 0.9 * len(c):
            out.append("~")     # 0x55/0xAA fill with some bits programmed to 0
        else:
            out.append("S")
    return out, plen


def cmd_show(args):
    card = open_card(args)
    rics = parse_rics(args.rics)
    offline = isinstance(card, Image)
    print(_c("1", "NewsCard") + ("  (offline image %s)" % args.image if offline else "  (%s)" % args.dev))
    # status panel
    if not offline:
        def get(fn):
            try:
                return fn()
            except CardError as e:
                return None
        bat, tot, unr, st, tm = (get(card.battery_ok), get(card.total), get(card.unread),
                                 get(card.status), get(card.get_time))
        sw = SWITCH.get(st[1], "unknown %02x" % st[1]) if st else "?"
        free = (st[18] << 8 | st[19]) if st else None
        print("  battery  %s" % ("?" if bat is None else _c("32", "OK") if bat else _c("31", "LOW")))
        print("  switch   %s" % sw)
        print("  clock    %s" % (fmt_time(tm) if tm else "?"))
        print("  messages %s total, %s unread" % (tot, unr))
        if free is not None:
            print("  free     %d blocks (%d bytes)" % (free, free * 64))
    recs = card.records()
    used = store_map(card)
    nblk = (HEAP_END - HEAP_START) // 64
    print("  store    %d messages, %d of %d blocks in use" % (len(recs), len(used), nblk))
    # per address / function table
    print()
    print(_c("1", "Messages by address and function"))
    rows = {}
    for r in recs:
        k = (r["source"] >> 3, r["source"] & 7)
        e = rows.setdefault(k, [0, 0, None, None])
        e[0] += 1
        e[1] += 0 if r["read"] else 1
        t = fmt_time(r["time"])
        e[2] = t if e[2] is None or t < e[2] else e[2]
        e[3] = t if e[3] is None or t > e[3] else e[3]
    print("  %-13s %-4s %5s %6s  %-16s  %-16s" % ("address", "func", "msgs", "unread", "first received", "last received"))
    for (slot, fn), (n, u, a, b) in sorted(rows.items()):
        name = ("RIC %07d" % rics[slot]) if slot in rics else "slot %d" % slot
        print("  %-13s %-4s %5d %6d  %-16s  %-16s" % (name, FUNC[fn] if fn < 4 else fn, n, u, a, b))
    # RAM map
    print()
    print(_c("1", "RAM 0x00000-0x1FFFF") + "  (fixed areas, then one character per 64-byte block)")
    for lo, hi, what in ((0x0000, 0x002F, "CIS tuples (also seen as attribute memory)"),
                         (0x0030, 0x0BFF, "CIS strings, card data"),
                         (0x0C00, 0x0FEF, "command buffer (host -> card)"),
                         (0x0FF0, 0x0FFF, "mailbox registers: SEM FF0, STAT FFD, OWN FFE, DOORBELL FFF"),
                         (0x1000, 0x13D7, "reply buffer (card -> host)"),
                         (0x13D8, 0x13FF, "store pointers (changed by mark read / delete)"),
                         (0x1400, 0x1BFF, "directory: 16 buckets x 0x80, bucket = slot*8 + function"),
                         (HEAP_START, HEAP_END - 1, "message heap: records and 54-byte text blocks")):
        print("  %05x-%05x  %s" % (lo, hi, what))
    colors = {0: "36", 1: "33"}
    legend = "  legend: R record, A-D text (slot 0), a-d text (slot 1), lowercase/dim = read, . free"
    print(legend)
    per = 64
    for row in range(0, nblk, per):
        line = ""
        for b in range(row, min(row + per, nblk)):
            if b in used:
                kind, src, rd = used[b]
                ch = "R" if kind == "R" else FUNC[src & 3]
                if src >> 3 == 1 and kind == "T":
                    ch = ch.lower()
                line += _c(("2;" if rd else "") + colors.get(src >> 3, "35"), ch)
            else:
                line += "."
        print("  %05x %s" % (HEAP_START + row * 64, line))
    # flash map
    fl = None
    if offline and card.flash is not None:
        fl = card.flash
    elif not offline and not args.no_flash:
        fl = card.read(0x02000000, 0x20000)
    if fl:
        print()
        regions, plen = flash_regions(fl)
        print(_c("1", "Flash 0x02000000-0x0201FFFF") + "  (one character per 1 KB)")
        print("  C CIS/link target  P Newton package (%d bytes)  S Newton store  ~ 55/AA fill, some bits 0  - erased (FF)" % plen)
        fc = {"C": "35", "P": "32", "S": "36", "~": "2", "-": "2"}
        for row in range(0, len(regions), 64):
            print("  +%05x %s" % (row * 1024, "".join(_c(fc[x], x) for x in regions[row:row + 64])))


def cmd_watch(args):
    card = NewsCard(args.dev)
    seen = {(r["ptr"], r["seq"]) for r in card.records()}
    print("watching, %d message(s) on the card" % len(seen))
    while True:
        time.sleep(args.interval)
        ev = card.event()
        for r in card.records():
            key = (r["ptr"], r["seq"])
            if key not in seen:
                seen.add(key)
                print("%s  #%d  %s" % (fmt_time(r["time"]), r["seq"],
                                       card.text(r).decode("latin1", "replace")))
        if ev and args.verbose:
            print("(event bit was set)")


def cmd_dump(args):
    card = NewsCard(args.dev)
    data = bytearray()
    off = args.offset
    while len(data) < args.length:
        data += card.read(off + len(data), min(4096, args.length - len(data)))
    with open(args.file, "wb") as f:
        f.write(data)
    print("wrote %d bytes from card offset 0x%x to %s" % (len(data), args.offset, args.file))


def cmd_diff(args):
    """Compare two dumps; name the regions the protocol doc knows."""
    a = open(args.before, "rb").read()
    b = open(args.after, "rb").read()

    def region(o):
        if o < CMD: return "CIS/low"
        if o < SEM: return "command buffer"
        if o < RSP: return "mailbox regs"
        if o < DIR: return "reply buffer"
        if o < 0x1C00: return "directory (bucket %d +%d)" % ((o - DIR) // 0x80, (o - DIR) % 0x80)
        return "heap (64-byte block 0x%05x +%d)" % (o & ~63, o & 63)

    runs, start = [], None
    for i in range(min(len(a), len(b)) + 1):
        d = i < min(len(a), len(b)) and a[i] != b[i]
        if d and start is None:
            start = i
        elif not d and start is not None:
            runs.append((start, i))
            start = None
    for s0, e0 in runs:
        print("%05x..%05x  %-34s %s -> %s" % (s0, e0 - 1, region(s0), hexs(a[s0:e0][:24]), hexs(b[s0:e0][:24])))
    print("%d changed range(s)" % len(runs))


def cmd_config(args):
    """Read the card's config space byte by byte with command 0x11 (read-only)."""
    card = NewsCard(args.dev)
    if args.start < 0x20 and not args.yes:
        sys.exit("0x00-0x1F may be microcontroller I/O registers (reads can clear flags); add --yes")
    data = bytearray()
    for a in range(args.start, args.end + 1):
        try:
            r = card.xfer(0x11, bytes([a >> 8, a & 0xFF]))
        except CardError as e:
            print("0x%04x: %s; stopping" % (a, e))
            break
        if (r[4] << 8 | r[5]) != a:
            print("0x%04x: reply echoes address %02x%02x" % (a, r[4], r[5]))
        data.append(r[6])
    for o in range(0, len(data), 16):
        a = args.start + o
        print("%04x  %s" % (a, hexs(data[o:o + 16])))
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
        print("saved to", args.out, "(first byte = address 0x%04x)" % args.start)


def need_yes(args):
    if not args.yes:
        sys.exit("this changes the card; add --yes")


def find_record(card, seq_or_ptr):
    v = int(seq_or_ptr, 0)
    for r in card.records():
        if r["ptr"] == v or r["seq"] == v:
            return r
    sys.exit("no such message")


def cmd_settime(args):
    need_yes(args)
    t = datetime.datetime.now() if args.when == "now" else datetime.datetime.strptime(args.when, "%Y-%m-%d %H:%M")
    # two-digit year: the card NAKs year bytes > 99 (code 10)
    payload = bytes([t.year % 100, t.month, t.day, t.hour, t.minute])
    card = NewsCard(args.dev)
    card.xfer(0x31, payload)
    print("set; pager now says", fmt_time(card.get_time()))


def cmd_markread(args):
    need_yes(args)
    card = NewsCard(args.dev)
    r = find_record(card, args.message)
    card.xfer(0x01, struct.pack(">IB", r["ptr"], r["seq"]))
    print("marked", r["seq"])


def cmd_delete(args):
    need_yes(args)
    card = NewsCard(args.dev)
    r = find_record(card, args.message)
    card.xfer(0x02, struct.pack(">IB", r["ptr"], r["seq"]) + b"\0\0\0")
    print("deleted", r["seq"])


def cmd_alerts(args):
    """Alerts on/off: config byte 0xD6, bits 0xC0."""
    card = NewsCard(args.dev)
    if args.state:
        need_yes(args)
        card.xfer(0x12, bytes([0x00, 0xD6, 0xC0 if args.state == "on" else 0x00, 0xC0]))
    print("alerts", "on" if card.read_config(0xD6) & 0xC0 else "off")


def cmd_raw(args):
    cmd = int(args.cmd, 16)
    payload = bytes(int(x, 16) for x in args.payload)
    if cmd not in (0x32, 0x44, 0x81, 0x82, 0x83, 0x11):
        need_yes(args)
    card = NewsCard(args.dev)
    r = card.xfer(cmd, payload, args.reply)
    print(hexs(r))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dev", default=DEV)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("probe", help="read-only first-contact checks (no commands sent)").set_defaults(fn=cmd_probe)
    sub.add_parser("info", help="battery, counts, status, time, config (read-only commands)").set_defaults(fn=cmd_info)
    p = sub.add_parser("show", help="status panel, messages per address/function, RAM and flash maps")
    p.add_argument("--image", help="RAM dump to show instead of the card")
    p.add_argument("--flash", help="flash dump (with --image)")
    p.add_argument("--rics", default=os.environ.get("NEWSCARD_RICS", ""),
                   help="RICs in address slot order, e.g. 1234567,7654321 (or $NEWSCARD_RICS)")
    p.add_argument("--no-flash", action="store_true", help="don't read the flash")
    p.set_defaults(fn=cmd_show)
    p = sub.add_parser("list", help="list messages (read-only)")
    p.add_argument("--raw", action="store_true", help="also print raw record bytes")
    p.add_argument("--image", help="RAM dump to list instead of the card")
    p.add_argument("--rics", default=os.environ.get("NEWSCARD_RICS", ""),
                   help="RICs in address slot order, e.g. 1234567,7654321 (or $NEWSCARD_RICS)")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("watch", help="print new messages as they arrive")
    p.add_argument("--interval", type=float, default=5)
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_watch)
    p = sub.add_parser("dump", help="save raw common memory")
    p.add_argument("file")
    p.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    p.add_argument("--length", type=lambda s: int(s, 0), default=0x20000)
    p.set_defaults(fn=cmd_dump)
    num = lambda x: int(x, 0)
    p = sub.add_parser("config", help="read the config bytes with command 0x11 (read-only)")
    p.add_argument("--start", type=num, default=0x20)
    p.add_argument("--end", type=num, default=0xFF)
    p.add_argument("--out", help="save the bytes to a file")
    p.add_argument("--yes", action="store_true", help="allow --start below 0x20")
    p.set_defaults(fn=cmd_config)
    p = sub.add_parser("diff", help="compare two dumps (e.g. before/after a change)")
    p.add_argument("before")
    p.add_argument("after")
    p.set_defaults(fn=cmd_diff)
    for name, fn, extra in (("settime", cmd_settime, "when"), ("markread", cmd_markread, "message"),
                            ("delete", cmd_delete, "message")):
        p = sub.add_parser(name)
        p.add_argument(extra, help="'now' or 'YYYY-MM-DD HH:MM'" if extra == "when" else "sequence number or record offset")
        p.add_argument("--yes", action="store_true")
        p.set_defaults(fn=fn)
    p = sub.add_parser("alerts", help="show or set alerts (beep on new pages)")
    p.add_argument("state", nargs="?", choices=("on", "off"))
    p.add_argument("--yes", action="store_true")
    p.set_defaults(fn=cmd_alerts)
    p = sub.add_parser("raw", help="send a command: raw <cmd hex> [payload hex bytes...]")
    p.add_argument("cmd")
    p.add_argument("payload", nargs="*")
    p.add_argument("--reply", type=int, default=None, help="reply bytes to read")
    p.add_argument("--yes", action="store_true")
    p.set_defaults(fn=cmd_raw)
    args = ap.parse_args()
    try:
        args.fn(args)
    except CardError as e:
        sys.exit("card error: %s" % e)
    except OSError as e:
        sys.exit("%s: %s" % (args.dev, e))


if __name__ == "__main__":
    main()
