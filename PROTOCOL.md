# Motorola NewsCard: host interface (short)

Observed on a working card (Ver 1.0.0), 2026-11-25.

## Access

- 8-bit memory window, single-byte accesses. The card ignores REG#
  (attribute and common memory are the same).
- CIS speeds: RAM 100 ns, flash 200 ns. On a Ricoh RL5C475 bridge the flash
  needs a short read strobe (16-bit memory timing 0x0425) to read reliably.

## Memory map (common memory, plain card offsets)

| offset | what |
|---|---|
| 0x00000-0x0007F | CIS tuples and strings |
| 0x00C00 | request buffer (host -> card) |
| 0x00FF0 | SEM: host writes 0x44 |
| 0x00FFD | STAT: bit 0 = event pending (host clears it); seen 0x00/0x02/0x05 |
| 0x00FFE | OWN: host writes 0xAA, card changes it when it takes a request |
| 0x00FFF | DOORBELL: host writes 0xFF |
| 0x01000 | reply buffer (card -> host) |
| 0x01400-0x01BFF | message directory: 16 buckets, 0x80 apart, 4-byte big-endian head pointer, 0xFFFFFFFF = empty |
| 0x01C00-0x1FFFF | message heap: 64-byte blocks |
| 0x02000000 | flash, 128 KB (repeats every 128 KB) |
| 0x03000000 | the RAM again |

## Mailbox handshake

1. Acquire: write SEM = 0x44 and OWN = 0xAA, read both back; repeat until
   both read back. If STAT bit 0 is set, write STAT = 0 (a new page
   arrived).
2. Write the request at 0xC00: `cmd, lenHi, lenLo, payload..., 0xFF, 0xAA`.
   Check that 0xC00 and OWN read back, then write DOORBELL = 0xFF.
3. Wait until OWN no longer reads 0xAA (the card took the request).
4. Acquire again and read the reply at 0x1000 until its first byte is
   `cmd` and OWN reads 0xAA.

Reply: `cmd, lenHi, lenLo, status, data..., 0xFF, 0xAA`, with
len = 1 + data length. Status 0x06 = ACK, 0x15 = NAK, then the error code
in the first data byte.

Message store reads: acquire, read, and check that OWN still reads 0xAA
(otherwise read again).

## Commands

| cmd | payload | reply data | meaning | tested |
|---|---|---|---|---|
| 0x01 | ptr32, seq | ptr, seq | mark message read | yes |
| 0x02 | ptr32, seq, 0, 0, 0 | ptr, seq | delete message | yes |
| 0x11 | addrHi, addrLo | addrHi, addrLo, value | read config byte; only 0x4A-0xFF, else NAK 8 | yes |
| 0x12 | addrHi, addrLo, value, mask | addrHi, addrLo | write config byte (masked) | yes (0xD6) |
| 0x13 | - | no reply | notify | no |
| 0x31 | YY, MM, DD, hh, mm | - | set clock; YY 0-99 (two-digit year), > 99 gives NAK 10 | yes |
| 0x32 | - | YY, MM, DD, hh, mm | read clock | yes |
| 0x41 | 1 byte | - | alert select | no |
| 0x44 | - | 1 byte | battery: nonzero = OK | yes |
| 0x81 | - | u16 | unread count | yes |
| 0x82 | - | u16 | total count | yes |
| 0x83 | - | 20 bytes | status (below) | yes |

ptr32 is the record's offset, big-endian.

Status block (0x83): [1] switch (0x04 on, 0x02 silent, 0x01 off);
[2..6] and [7..11] two timestamps (meaning unknown); [12..16] the clock;
[18..19] free 64-byte blocks, big-endian.

Config: 0xD6 bits 0xC0 = alerts on/off. With alerts off a page is stored
silently, and its alert sounds once when alerts are switched back on.

The clock starts at 1993-01-01 00:00 when the card is switched on. Years
93-99 mean 1993-1999, 00-92 mean 2000-2092.

## Message store

Directory bucket = source byte = address slot x 8 + function (A=0 ... D=3).
The card has two address (RIC) slots; it does not report the RIC itself.

Record (64 bytes, offsets from the record pointer; mask pointers with
0x3FFFC):

| off | size | meaning |
|---|---|---|
| +0 | 4 | next (older) record in the bucket |
| +4 | 4 | previous (newer) record, or the directory slot |
| +9 | 1 | sequence number |
| +10 | 2 | error count |
| +20 | 4 | first text block |
| +24 | 4 | pointer to the last text byte |
| +36 | 1 | source (slot x 8 + function) |
| +38 | 1 | flags; bit 2 = read |
| +40 | 2 | number of text blocks |
| +45 | 5 | received: YY MM DD hh mm (card clock) |

Text block (64 bytes): +0 next block, +4 previous block (the blocks form a
ring; stop after the block count), +10..+63 = 54 text bytes.

Text length = 54 x blocks - (last block + 64 - text end) + 1, with the
last block's address taken from the first block's +4 and raw (unmasked)
pointers.

Pages are stored as text as received. The longest page the card accepts is
2019 characters; longer pages lock the card up until it is reset.
