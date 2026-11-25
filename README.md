# newscard: Linux driver for the Motorola NewsCard

A Linux kernel module and a command line tool for the Motorola NewsCard Data
Receiver, a POCSAG pager on a PC Card (PCMCIA). Apple sold it as the Newton
Messaging Card.

- `newscard.c`, `newscard_ioctl.h`: the kernel module
- `newscard.py`: the command line tool (Python 3, standard library only)
- `99-newscard.rules`: udev rule for access without root
- `dkms.conf`: DKMS configuration
- `PROTOCOL.md`: the card's host interface
- `tests/`: offline tests of the tool

Tested with Linux 6.12 and a Ricoh RL5C475 CardBus bridge.

## What the driver provides

| where | what |
|---|---|
| `/sys/class/newscard/newscard0/battery` | `ok` or `low` |
| `.../switch` | `on`, `silent` or `off`, the switch on the card |
| `.../clock` | the card's clock, `YYYY-MM-DD hh:mm`; write the same format to set it |
| `.../alerts` | `on` or `off`: beep on new pages; write to change. With alerts off, pages are stored silently and their alert sounds once when alerts are switched back on |
| `.../total`, `.../unread`, `.../free_blocks` | message counts and free memory (64-byte blocks) |
| `.../status` | the card's 20-byte status block, in hex |
| `.../event` | number of new pages since the module was loaded; `poll()` on it, and a udev `change` event for each new page |
| `.../markread`, `.../delete` | write a message number (or a record offset `0x...`) |
| `/dev/newscard0msgs` | the messages, like `/dev/kmsg`: each `read()` returns one message, oldest first; at the end `read()` waits for the next page (`EAGAIN` with `O_NONBLOCK`), `poll()` wakes on new pages, `lseek(fd, 0, SEEK_SET)` starts again, `SEEK_END` skips to new pages |
| `/dev/mtd0ro`, `/dev/mtdblock0` | the card's 128 KB flash, read-only (MTD, name `newscard0-flash`) |
| `/dev/newscard0`, `/dev/newscard0attr` | raw card memory and the ioctls `newscard.py` uses |

A message read from `/dev/newscard0msgs`:

```
message 51
received 2026-11-25 12:30
slot 0
function A
state new
errors 0
record 0x06d00
length 24
SINGLE PAGE TEST 3 12:30
```

`slot` is the card's address (RIC) slot, 0 or 1; the card does not report
the RIC itself. `function` is the POCSAG function, A to D. `length` is the
number of text bytes after the header.

## Requirements

- A kernel with PC Card support (`CONFIG_PCMCIA`, and a socket driver such
  as `yenta_socket`) and MTD (`CONFIG_MTD`; `CONFIG_MTD_BLOCK_RO` for
  `/dev/mtdblockN`).
- The headers of the running kernel (Debian/Ubuntu:
  `linux-headers-$(uname -r)`), `make`, a C compiler.
- Python 3 for `newscard.py`.

## Build and install

Build and load without installing:

```
make
sudo modprobe mtd
sudo insmod ./newscard.ko
```

Install, so that the module loads by itself when the card is inserted:

```
make
sudo make install
sudo modprobe newscard
```

With DKMS (rebuilt for each new kernel):

```
sudo cp -r . /usr/src/newscard-0.1
sudo dkms add newscard/0.1
sudo dkms install newscard/0.1
```

For `/dev/mtdblock0`, also `sudo modprobe mtdblock_ro`.

To reload the module while the card is inserted, unbind the card first:

```
echo 0.0 | sudo tee /sys/bus/pcmcia/drivers/newscard/unbind
sudo rmmod newscard
```

The driver binds only to cards whose CIS reads "Motorola, Inc." /
"NewsCard Data Receiver".

### Access without root

```
sudo cp 99-newscard.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

Members of the `plugdev` group can then use the devices and write the sysfs
files `clock`, `alerts`, `markread` and `delete`.

## Module parameters

| parameter | default | meaning |
|---|---|---|
| `width16` | 0 | 16-bit memory window; the card needs 8 bit |
| `speed_ns` | 300 | memory access time requested from the socket driver |
| `ricoh_mem_timing` | 0x0425 | Ricoh bridges: 16-bit memory timing (setup, command, hold); 0 = leave unchanged |
| `flash_reads` | 0 | read each flash byte N times and OR the results; 0 = 1 with Ricoh timing set, else 5 |
| `poll_ms` | 2000 | interval for the new-page check; 0 = off |
| `timeout_ms` | 20000 | mailbox timeout |
| `win_size` | 0x20000 | memory window size |
| `use_wait` | 1 | honour the card's WAIT signal |
| `allow_raw_write` | 0 | allow `write()` to `/dev/newscard0` |

## PC Card adapters

The driver requests the access time from the socket driver (`speed_ns`),
which sets the adapter's wait states. Ricoh CardBus bridges time 16-bit
cycles with their own registers instead; the driver sets them
(`ricoh_mem_timing`) and restores the old value when the card is removed.

The card's flash reads 1 bits as 0 when the read strobe is too long. On
adapters other than Ricoh the driver reads each flash byte five times and
ORs the results. `newscard.py probe` reads the flash three times and reports
differences; please report the result and the adapter (`lspci -nn`) for
adapters other than the Ricoh RL5C475.

## The tool

```
sudo ./newscard.py probe            # card memory, mailbox, flash read check
sudo ./newscard.py info             # battery, counts, status, clock, config
sudo ./newscard.py show             # status, messages per address and function, memory maps
sudo ./newscard.py list             # messages
sudo ./newscard.py watch            # print new pages as they arrive
sudo ./newscard.py alerts           # alerts on or off
sudo ./newscard.py dump file.bin --offset 0x02000000 --length 0x20000
```

Commands that change the card need `--yes`: `settime now`, `markread N`,
`delete N`, `alerts on|off`, `raw CMD [BYTES...]`.

`show` and `list` also work on a saved RAM dump (`--image ram.bin
[--flash flash.bin]`). `--rics 1234567,7654321` (or `$NEWSCARD_RICS`) names
the card's two address slots. `list` shows pages of the form
`from|subject|body` as From, Subject and Message.

Offline tests: `python3 tests/test_tool.py`.

## About the card

- 128 KB RAM (mailbox and message store) and 128 KB flash. The flash holds
  the card's own Newton software and a small Newton store; the driver only
  reads it. A dump of a card is at
  https://archive.org/details/motorola-newscard-newton-messaging-card-dump.
- The card has two address (RIC) slots and stores the POCSAG function (A to
  D) of each page.
- The longest page the card stores is 2019 characters.
- The card's clock keeps a two-digit year; the driver reads 93-99 as
  1993-1999 and 00-92 as 2000-2092, and writes `year % 100`. The clock
  starts at 1993-01-01 00:00 when the card is switched on.
- The switch has three positions: on (beep), silent and off (sysfs
  `switch`).

## License

GNU General Public License, version 2 or (at your option) any later
version (GPL-2.0-or-later, `LICENSE`). `newscard_ioctl.h` is
GPL-2.0-or-later WITH Linux-syscall-note.
Copyright 2026 Vasyl Samoilov.
