# Hub RF via `/dev/rfspi` (CC2544)

Our own description of how the stock Harmony Hub talks to its CC2544 radio,
derived from static analysis of the stock `cc2544.ko` and `/usr/bin/hal`.
**Nothing here has been exercised from our own code yet** — no frame has been
sent or received by anything we wrote, and the on-device probe set (section 8)
has not been run. Treat every length, header value and status byte below as
unvalidated until that happens.

Tags: **[CODE]** = read directly from disassembly (offset given);
**[INFERRED]** = conclusion not visible in code; **UNKNOWN** = gap.

Binaries analysed (read-only copies out of
`backups/phase1/hub/20261116T234902Z/03_root.raw.img`, md5 matching the table
in `hub-ir-i2s.md`, not committed):

| file | md5 |
|------|-----|
| `/lib/modules/2.6.31-g89d565c/cc2544.ko` | `45201b7f2662cb9d9a4347cb810fadbf` |
| `/usr/bin/hal` | `a3fdd823440c43f36e235fe51c45f008` |

Addresses are `hal` VMAs or `cc2544.ko` `.text` offsets. The stock
`unsquashfs` 4.6.1 cannot read this squashfs ("lzma uncompress failed") — it
uses old-style lzma blocks with a 5-byte props header and no 8-byte size
field; prepend `ff * 8` and decode with Python `lzma.FORMAT_ALONE`. System
`objdump` has no MIPS support; use a cross `objdump`.

## 1. Summary, and the three things that break the obvious approach

RF is **not** an ioctl interface. `/dev/rfspi` exposes only
`open`/`read`/`write`/`release`, and three of its semantics are the opposite of
what idiomatic code assumes:

1. **`write()` returns the received frame length, not the bytes written.** The
   usual `if (write(fd, buf, n) != n) error` is actively wrong here.
2. **`read()` ignores its `count` argument.** There is no bounds check in the
   kernel; it copies a whole queued entry whatever the caller asked for.
3. **`read()` blocks forever.** No timeout, no `poll`, and `O_NONBLOCK` is
   ignored. A blocked reader is woken only by a signal — closing the fd from
   another thread will not do it.

## 2. Device nodes and open semantics

- `/dev/rfspi`, char 10:59, misc device `cc2544_spi` **[CODE]** (`.data+0x90`:
  minor `0x3b`, name at `.str+0x1e0`) — the data/command channel.
- `/dev/rffw`, char 10:60, misc device `cc2544_fw` **[CODE]** (`.data+0x60`:
  minor `0x3c`) — the firmware flasher. **Do not open it.** Its `release`
  (@0xe8c) erases and programs chip flash through the debug interface
  (`cc2544_erase_flash_page`, `cc2544_program_page`, chip-id check @0x0444).
  Stock drives it as `system("cat /lib/firmware/cc2544.bin > /dev/rffw")`
  (string @0x44a58c, in `libhal_rf_init`); `cc2544.bin` is 20929 bytes and
  `cc2544.version` reads `39.00.0010`, parsed `"%d.%d.%x"`. `hal` reflashes on
  a version mismatch.
- Both nodes are static inodes in the squashfs `/dev`, and their names differ
  from the kernel's misc names. `etc/modules` loads `cc2544` at boot. The name
  `rfspi` at `.str+0x1b0` is only the IRQ label.
- `cc2544_spi_fops` @`.rodata+0x30` **[CODE]**: `read` @0x1628, `write`
  @0x152c, `open` @0x11d0, `release` @0x11e8. **Every other slot is zero** —
  `.ioctl` (+0x20), `.unlocked_ioctl`, `.poll`, `.mmap`, `.llseek`, `.flush`.
  So there are no request codes; any ioctl returns `ENOTTY`/`EINVAL`.
- `open`/`release` only increment and decrement a global counter
  (`.bss+0x10`) **[CODE]**. Opens are **not exclusive** — the kernel will not
  stop a second opener. Received frames are queued only while that counter is
  non-zero.
- `hal` opens with `O_RDWR` in `libhal_rf_init` @0x426448 (open @0x42656c) and
  `rf_msg_init` @0x42b86c (open @0x42b8b4). No `O_NONBLOCK`, no poll/select
  anywhere.

**Contention.** Because the kernel enforces nothing, *we* must: our `claim`
is the only guard, and we must not run while stock `hal` holds the node.

## 3. Electrical layer (why a transfer can fail)

Bit-banged SPI, MSB first, through the AR7240 controller in IO mode
**[CODE]** (`cc2544_spi_init` @0x184c, `cc2544_spi_readwrite` @0x1200,
`cc2544_spi_raw_output_u8` @0x1180):

- `0xbf000000` = 1 (SPI_FS, IO mode), `0xbf000004` = `0x63` (clock; the old
  value is saved and restored), `0xbf000008` = IO_CTRL (bit0 MOSI, bit8 CLK),
  `0xbf00000c` read for MISO.
- GPIO base `0xb8040000` (OE +0, IN +4, OUT +8). **GPIO 8** is chip select,
  active low, idle high. **GPIO 6** is an input the driver waits to go low
  before a transfer — probably the chip's "ready to be clocked" line
  **[INFERRED]**; the wait times out after 250 jiffies and yields `-EIO`.
  **GPIO 14** is data-ready, IRQ 46 (`32+14`), via
  `request_threaded_irq(46, handler=0x17dc, ..., "rfspi")`.
- Timing: 20 µs after CS low, 5 µs between bytes, 5 µs before CS high.
- Each transfer brackets itself with `ar7240_flash_spi_down` (@0x12b4) and
  `ar7240_flash_spi_up` (@0x1488) — **this is the shared NOR-flash SPI bus.**
  Consequence **[INFERRED]**: mtd reads (our own partition backups) contend
  with RF traffic on the same bus. Do not image flash and drive the radio at
  the same time.

## 4. Transfer semantics

**Receive path [CODE]:** the IRQ handler (@0x17dc) calls `disable_irq(46)` and
`schedule_work`; the worker (@0x1818) calls `cc2544_spi_readwrite(NULL, 0)` —
a pure clock-in poll — then re-enables the IRQ.

**`write(fd, buf, n)` [CODE]** performs a full-duplex transaction. The first
byte clocked back ("rx0") selects the frame length from a table @0x1324-0x1380:

| rx0 | frame length |
|---|---|
| `0x00` | none |
| `0x01`, `0x02` | 7 |
| `0x03` | 4 |
| `0x04`, `0x08` | 1 |
| `0x10` | 7 |
| `0x11` | 20 |
| `0x12` | 30 |
| `0x20` | 32 |
| anything else | 0, plus a `printk` "Payload length 0x%x is not detected" |

The transfer then clocks `max(rx_len, n)` bytes, sending 0 beyond `n`. The host
report ID and the chip's reply length are therefore independent. Return value
is the **received** length (0 if none). Errors: `-EIO` (GPIO6 still high),
`-ENOMEM` on `kmalloc` failure, `-EFAULT` on a `copy_from_user` fault.

A frame received during a write — including the reply to that write — is **not**
returned from `write()`. It is queued as a 68-byte entry (64-byte buffer plus
length at +0x40) and comes out of `read()`.

**`read(fd, buf, count)` [CODE]** blocks interruptibly until the queue is
non-empty, then copies one whole entry and returns its length. `count` is never
checked (@0x1710-0x1750), so **pass at least 64 bytes.** A signal returns
`-ERESTARTSYS`; stock wraps command reads in `alarm(1)` with a SIGALRM handler
(sigaction 14).

**Report ID → length** (agrees with upstream's `HARMONY_RF_PROTOCOL.md` §3):
`0x10` = 7 (short), `0x11` = 20 (long), `0x12` = 30 (hub data),
`0x20` = 32 (remote data).

## 5. Command frames stock uses

`rf_msg_write(buf, len)` @0x42add4 takes a mutex (@`...6f34`), bumps a 16-bit
counter at +0x6f6c, and calls plain `write()`, treating only `<0` as an error.

The 7-byte short-command round trip (static function @0x424e7c) is:
`write(fd, cmd, 7)`, then loop `{alarm(1); read(fd, rep, 10); alarm(0)}` until
`rep[0..4] == cmd[0..4]`. Commands seen **[CODE]** (rodata 0x44a620, 0x44a62c,
0x44b98c):

| frame | meaning |
|---|---|
| `10 FF 80 00 00 01 00` | init, sent at the end of `rf_msg_init` |
| `10 FF 81 F1 01 00 00` | version major/minor (reply bytes 5, 6) |
| `10 FF 81 F1 02 00 00` | version build (reply bytes 5, 6 as a u16) |
| `20 <mode> 00 00 00 00 00` | power mode (`hot_set_powermode` @0x42e004) |
| `10 FF 80 B2 01 00 <secs>` | open the pairing lock |
| `10 FF 80 B2 02 00 00` | close the pairing lock |
| `10 FF 80 B2 03 <slot> 00` | unpair a slot |
| `10 FF 83 B5 <pipe> 00 00` | pipe-table query (20-byte `11 FF 83 B5 ...` reply) |

**Reader thread [CODE]:** @0x42b4d8, created by `rf_msg_init`, loops
`read(fd, buf, 32)` and dispatches on a 16-bit key `(byte1<<8)|X`, where X is
`byte2` for report `0x12`; `byte4` for `0x20` with `byte2 == 0x12`; `byte4` for
an error report (`byte2 == 0x8f`); `byte3` when the top bit of `byte2` is set;
else `byte2`. It matches that key against a subscriber table
(`rf_msg_subscribe` @0x42b008) per report ID. The mask semantics are
**UNKNOWN**.

Link and status notifications, per upstream prose, consistent with `hal`'s use
of `0x8f` in its dispatch key: `10 <slot> 41 04 <status> <wpid>` link status,
`20 <slot> 42 <status>` TX status, `10 <slot> 4A ...` pairing notification,
`10 <slot> 8F ...` error/NAK.

## 6. Data frames, and the 30-vs-32 correction

`hot_send_message_and_data` @0x42c3c8 (TX) and `hot_recv_parse_packet`
@0x42cbbc (RX). The global at +0x6f70 selects mode (0 = host/hub) **[CODE]**:

- **Hub mode:** `0x12, slot, <28-byte inner block>` = **30 bytes** (`li 18`
  @0x42c8ac, length `0x1e` @0x42c8bc). Header byte at frame[2], message type at
  frame[3]; on RX these land in object fields +0x7d and +0x7c.
- **Remote mode:** `0x20, pipe|powermode, 0x12, slot, <28 bytes>` =
  **32 bytes**.

**The design spec's "30 B hub→remote, 32 B remote→hub" is wrong because it is
not directional.** 30 is the `0x12` report on the *hub's* SPI in both
directions — the hub's chip runs host mode — and 32 is the `0x20` report on the
*remote's* own SPI. On the hub side, everything is 30 bytes.

Inner block (28 bytes, both modes): `hdr/seq, type, flags, payload[25]`.

Header byte **[CODE]**, a deviation from upstream's doc §3.1 (which describes a
fixed `0x50|seq`):

- low nibble = sequence;
- `hdr & 0xe0` selects the kind — `0x40` ACK, `0x60` windowed/continuation,
  `0x20` ordinary object frame; anything else is logged as bad;
- `hdr & 0x10` is a separate flag, OR'd into the first frame of an object
  (@0x42c584).

TX sets `0x40|seq` for ACKs (@0x42c494), `0x20|seq` for objects (@0x42c79c),
`0x60|seq` for continuations (@0x42c9b4). So stock emits `0x20`, `0x30`,
`0x40`, `0x50`, `0x60`; upstream's single `0x50` is one combination
(`0x40|0x10`). **[INFERRED]** from code, never seen on air — probe 5.

No checksum or CRC appears in these frames; integrity is the radio's job.
Message-level CRC16 shows up only in icon and OTA payloads (upstream).

## 7. Pairing

Pairing state lives **inside the CC2544**, not in a hub file **[CODE/INFERRED]**
— nothing for it was found in the squashfs, in `/data` (jffs2,
`04_data.raw.img`) or in the stock tar. A driver therefore never handles RF
addresses itself: it sends the section-5 commands and reads the table back.

Pipe-table contents by pipe (upstream §4.3-4.4, agreeing with `hal` strings):
`0x03` hub RF address (4 B); `0x20..0x25` EquadID (`0x4021` = the Juniper
family, which includes the Elite); `0x30..0x35` remote RF address (4 B);
`0x40..0x45` remote name.

Relevant `hal` symbols: `libhal_rf_host_open_lock` @0x427718, `_close_lock`
@0x426b14, `_unpair` @0x427320, `libhal_get_paired_device_info` @0x425c30,
`hot_get_juniper_index`, `hot_update_pair_status`; strings "Paired Remote is not
Juniper", "SkinId write %s RFID %u"; API paths `/rf/pairing`, `/rf/unpair`,
`/rf/deviceinfo`. The "RFID" in the logs is a hub-side index mapped to a slot
and device index (`libhal_dongle_get_devid`), not a persistent address; where
it is stored is **UNKNOWN**. The Lua `rf.lua` only forwards `rf.pairing` and
`getRFInfo` over hbus.

## 8. On-device probes (not yet run)

All require stock `hal` stopped (`tools/deploy_hub.py quiesce`), no other
opener of `/dev/rfspi`, and no concurrent flash imaging (section 3).
`tools/rf_probe.c` implements 1, 3 and the 6 canary.

1. Does `write()` return the rx length (30) or 0 for a data frame, and what
   comes back after the init command? → `rf_probe --version`.
2. Does the hub's chip ever deliver `0x20`/32-byte reports, or only `0x12`/30?
   → `rf_probe --listen`.
3. Pipe table: confirm the layout and that the Elite is slot 1, EquadID
   `0x4021`. → `rf_probe --pipes`.
4. Link-event bytes on remote wake/sleep: confirm status `0x87` up, `0x4x`
   lost.
5. Does stock really emit `0x30`/`0x50` headers? Capture a `0x12` frame from
   `hal` mid-sync and compare byte 2 against section 6 (strace the running
   `hal`; never a second opener).
6. Does `read()` ever return more than 32 bytes, or write past the reported
   length? 64-byte buffer pre-filled `0xAA`, check what changed.
7. Confirm power mode `0x30` vs `0x20` raises the frame rate (upstream
   measured ~250/s vs ~7/s).
8. Does the GPIO6 wait ever time out in normal use (`-EIO`)? Count `write()`
   failures over a long idle period.

**Blocked until Stage D:** probes 2, 4, 5 and 7 need the Elite awake and
running its RF stack. The remote is now rooted, so its stock app is gone and it
generates no traffic; these wait for our own remote firmware.

## 8a. Results from the device, 2026-11-17 **[MEASURED]**

Stock stack quiesced, `rf_probe` run over SSH, hub restored afterwards
(129 s outage). This is the first time any of our code has driven the radio.

**`write()` returns 0 for a 7-byte command, not the frame length.** Every
command — init, both version queries, all 19 pipe queries — returned exactly
`0`. The chip clocks back `rx0 = 0` during the transaction (no frame), and the
reply arrives later through the IRQ/`read()` path. So `0` is the **normal,
successful** return for a short command, not a failure, and the section-4 table
describes what *can* come back, not what usually does. A driver must therefore
treat only a negative return as an error, and must not wait for a non-zero
`write()` before reading.

**The reply does arrive on `read()`, at the table's length.** Every successful
reply was 7 bytes for a `0x10` command and 20 bytes for a pipe query, matching
the table exactly.

| query | reply |
|---|---|
| init `10 FF 80 00 00 01 00` | `10 FF 80 00 00 00 00` |
| version major/minor | `10 FF 81 F1 01 27 00` → `0x27` = 39, minor 0 |
| version build | `10 FF 81 F1 02 00 10` → build `0x0010` |

Chip firmware is **39.00.0010**, matching `/lib/firmware/cc2544.version`
exactly — the build field is hex, as stock's `"%d.%d.%x"` parse implies. (Our
first run printed it in decimal as `39.00.0016`, which read as a mismatch; since
`hal` reflashes on a version mismatch, that formatting is now pinned by a test.)

**Pairing table — only slot 1 is populated**, confirming one paired remote:

| pipe | reply payload | reading |
|---|---|---|
| `0x03` | `A1 B7 95 EE 17 06 08` | hub RF address `A1B795EE` |
| `0x21` | `08 28 40 21 04 02 07 04` | EquadID **`4021`**, the Juniper family ✓ |
| `0x31` | `75 BB 13 86 0A 00 00 00 0F` | remote RF address `75BB1386` |
| `0x41` | `03 31 31 31` | remote name, length 3: **"111"** |

Pipes `0x20`, `0x22-0x25`, `0x30`, `0x32-0x35`, `0x40`, `0x42-0x45` all timed
out — empty slots answer nothing rather than returning zeros, so a timeout is
how an unused slot presents. The section-7 layout is confirmed, as is the Elite
being slot 1 with EquadID `0x4021`.

**No `read()` overrun observed** (probe 6): across every reply, no byte beyond
the returned length had been written. Not proof of a bounds check — there is
none in the driver — but no misbehaviour in practice.

**`--listen` for 20 s saw 0 frames**, as expected: the Elite is rooted, its
stock RF app is gone, and nothing on the link transmits. Probes 2, 4, 5 and 7
remain blocked on Stage D.

**Still UNKNOWN:** whether `write()` ever returns non-zero in practice (nothing
provoked it); the meaning of the trailing bytes in the `0x03`, `0x21` and `0x31`
replies; and whether the `0x21` layout is `08 28` + EquadID or something else
framing it.

## 9. What this means for our code

`hub/rf/framing.h` defines a 4-byte header `type, seq, (idx<<4|total), len`
inside frames of `HP_RF_FRAME_MAX` = 32, and its comment repeats the wrong
directional 30/32 split.

- The real application budget is the **28-byte inner block on both sides**, so
  our header must fit *inside* it, leaving **24 bytes of payload** — not the 28
  the current code assumes. `frame_cap` must stop meaning "bytes on
  `/dev/rfspi`".
- Byte 0 is not ours to choose: `0x12` on the hub, `0x20` on the remote, with
  slot/pipe following.
- **Decision (2026-11-17): adopt stock's inner layout** —
  `hdr(kind|flags|seq), type, flags, payload[25]` — rather than our invented
  header, so a stock remote interoperates and captured stock frames can
  validate us.
- Still sound in the current code: one datagram per frame, never truncating,
  and reassembly keyed by `(type, seq)`.

A `hub/io/rf_spi.c` must fill the `hp_rf_transport` vtable in `hub/io/sinks.h`
(`send`, `recv`, `claim`, `attached`), matching `rf_pair.c`'s contract: `claim`
returns 0 once then `-HP_E_BUSY`; `send`/`recv` give `-HP_E_INVAL` before a
claim; `recv` returns a length, 0 on timeout, `-HP_E_NOENT` once when the peer
is gone; frames over 32 bytes are `-HP_E_TOOBIG`. `hw_sinks.c` currently stubs
`rf_send`/`rf_recv` with `-HP_E_NOSYS` (lines 160-161) and leaves `attached`
NULL; the vtable is assembled at line 178, which is where the real driver slots
in. Suggested entry points, mirroring `rf_pair.h`: `hp_rf_spi_open(const char
*dev, hp_rf_transport **out)` (mapping EBUSY/EACCES to `-HP_E_HALBUSY` the way
`hp_hw_sinks_open` does, starting the reader thread, sending the init command),
`hp_rf_spi_destroy` (which must signal the blocked reader, not just close),
plus `hp_rf_spi_set_pairing` and `hp_rf_spi_unpair`.

`recv` needs a dedicated reader thread doing `read(fd, tmp[64], 64)`, filtering
reports: pass `0x12` data frames up with the 2-byte header stripped, treat
`0x10`/`0x11` as control, and feed `attached()` from
`10 <slot> 41 04 <status>` (`status & 0x40` = lost).
