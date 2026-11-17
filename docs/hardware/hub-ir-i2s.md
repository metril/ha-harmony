# Hub IR output via `/dev/i2s`

Our own description of how the stock Harmony Hub emits IR, derived from static
analysis of the stock `ath_i2s.ko` and `/usr/bin/hal`, then **exercised on the
device** (section 9: our own `tools/ir_bench.c` turned the Samsung TV on). Every claim carries evidence; **[INFERRED]** marks
conclusions not directly visible in code, **UNKNOWN** marks gaps.

Binaries analysed (read-only copies, md5 verified against the device, not
committed):

| file | bytes | md5 |
|------|-------|-----|
| `/lib/modules/2.6.31-g89d565c/ath_i2s.ko` | 14356 on host (device reports 16,544 as module size in memory) | `328dc78a0f0510ba6154fce9629c21a5` |
| `/lib/modules/2.6.31-g89d565c/cc2544.ko` | 19828 | `45201b7f2662cb9d9a4347cb810fadbf` |
| `/usr/bin/hal` | 363640 | `a3fdd823440c43f36e235fe51c45f008` |

All three are ELF32 MIPS big-endian (`readelf -h`). `ath_i2s.ko` is not
stripped (83 symbols, function names intact). `hal` is stripped of static
symbols but keeps `.dynsym` (486 entries, incl. `ir_send`, `ir_init`,
`bitbuf_*`) and many debug strings (`src/libhal/ir/libir.c`). No kernel config
or System.map exists on the device (`/proc/config.gz` absent, `/proc/kallsyms`
empty).

## 1. Summary

IR is **not** pulse-timing by ioctl. The userspace daemon synthesises the whole
waveform as a **1-bit-per-I2S-bit-clock bitstream** (carrier cycles included),
`write()`s it into a DMA ring, then triggers the DMA with an ioctl. The I2S
serial-data line carries that bitstream; a mark is a run of `1` bits chopped
into carrier cycles, a space is `0` bits. The carrier frequency is therefore
set by (a) the I2S bit-clock divider and (b) how many bits form one carrier
cycle.

## 2. Device node and open semantics

- `/dev/i2s`, char major 253 (dynamic; module params `ath_i2s_major`,
  `ath_i2s_minor`; `__param_ath_i2s_major/minor`).
- `file_operations` (`ath_i2s_fops`, `.data` + reloc table): `owner`, `read`
  (`ath_i2s_read` @0xacc), `write` (`ath_i2s_write` @0x434), **`ioctl`**
  (`ath_i2s_ioctl` @0xe10, the legacy `(inode,file,cmd,arg)` slot, not
  `unlocked_ioctl`), `open` (@0x90), `release` (@0x758). No `mmap`, no `poll`,
  no `llseek`, no `flush`.
- **All ioctl args are plain integers passed by value** (the handler uses the
  raw `arg` register, never `copy_from_user`), despite the `_IOW` encoding.
- `open` decides direction from `file->f_mode` (`f_mode & 1` = read = "mic",
  `f_mode & 2` = write = "speaker"). The two directions are independent slots
  (`sc_buf_var[0]` mic, `sc_buf_var[1]` speaker).
  - If that slot is already open: `printk("%s, %d I2S speaker busy")` /
    `"... I2S mic busy"` and return **-EBUSY (-16)**. (strings `.rodata.str1.4`;
    `li v0,-16` @0x130.)
  - Stock `hal` opens with flags `1` (**O_WRONLY**) for sending (`hal` ops table
    entry +0x18 @0x407274 takes the open flags; IR send path calls it with 1,
    @0x4222c4). It holds this for its lifetime, so **any write-open by us gets
    EBUSY while `hal` runs**. A read-only open (mic slot) is independent.
- Speaker open allocates a ring of **512 DMA descriptors of 12 bytes**
  (`dma_alloc_coherent(512*12)`) plus **512 buffers of 768 bytes**
  (`kmalloc(512*768)`, GFP_DMA 0xd0), mapped per-buffer. Mic path uses 128
  descriptors. Open fails with -ENOMEM and `"i2s buffer malloc failed"` if
  either allocation fails (@0x3d0). Ring capacity for output: **393,216 bytes
  = 3,145,728 bits**.
- First open (neither slot in use) calls `i2s_reset` (@0x28: writes `0xff` to
  MBOX reg `0xb80a0058`, `udelay(500)`, sets bit 19 of STEREO config
  `0xb80b0000`, `udelay(100)`) and finally writes `0x62` to `0xb80a0010`.
- `release` clears all descriptors' owner bits, pauses DMA, unmaps/frees the
  ring and clears the slot flag (@0x758-0x8cc). `hal` never closes in normal
  operation.

## 3. ioctl commands

Numbers are `cmd` values compared in `ath_i2s_ioctl`: the high half is
`0x8004`/`0xc004`, low half `0x4e..`. Type byte `'N'` (0x4e). Names below are
**ours**, chosen from behaviour. `hal` error strings confirm these four:
`I2S_DSIZE`, `I2S_MODE`, `I2S_VOLUME`, `I2S_FREQ`.

| cmd | our name | arg (by value) | effect | evidence |
|-----|----------|----------------|--------|----------|
| `0x80044e20` | `I2S_VOLUME` | int | maps arg to 0..31 style gain code and writes `code \| code<<8` to `0xb80b0004`. arg `<0`->15; `0..14` -> `(~arg & 0xf) \| 0x10`; `15` -> 0; `16..22` -> arg-15; `>=23` -> 7. `hal` passes **15** (-> 0). | ioctl @0xec8-0xf10; hal @0x4072f4 (`li a2,15`) |
| `0x80044e21` | `I2S_FREQ` | u32 divider (see section 5) | if different from current `0xb80b001c`: pause active TX/RX DMA, write arg to `0xb80b001c`, `i2s_reset`, resume. This is the **bit-clock divider**. | ioctl @0xf1c-0xfc4; hal @0x4076a4 |
| `0x80044e22` | `I2S_DSIZE` | 8, 16, 24 or 32 | writes STEREO config `0xb80b0000` = `0x00a00302` (8), `0x00a01302` (16), `0x00a22b02` (24), `0x00a23b02` (32), then `i2s_reset`. Other values: `printk("Data size %d not supported")`, return `-524`. `hal` passes **16**. | ioctl @0xfc8-0x1058; hal @0x4072c0 (`li a2,16`) |
| `0x80044e23` | `I2S_MODE` | int | arg==2: clear bit 14 (`0x4000`) of `0xb80b0000`; any other: set it. Meaning of the bit **UNKNOWN** [INFERRED mono/stereo or MCLK source]. `hal` passes **2**. | ioctl @0x105c-0x1090; hal @0x4072dc |
| `0x80044e24` | (unused) | - | no-op, returns 0 | @0xeb8 |
| `0xc0044e25` | (unused) | - | no-op, returns 0 | @0xe84 |
| `0xc0044e26` | `I2S_PAUSE` | 0 = TX, else RX | `ath_i2s_dma_pause`: writes 1 to TX ctrl `0xb80a001c` (RX: `0xb80a0024`), sets state 2 | ioctl @0xea8; `dma_pause` @0xdc4. Not used by `hal`. |
| `0xc0044e27` | `I2S_RESUME` | 0 = TX, else RX | `ath_i2s_dma_resume`: writes 4 to ctrl reg, sets state 1 | ioctl @0xec0; @0x70c. Not used by `hal`. |
| `0x80044e30` | `I2S_START` | `(ports<<16) \| (n & 0xffff)` | see below | ioctl @0x1094-0x1158; hal write-start @0x4079dc (`sll a2,a2,16; or`) |
| `0x80044e31` | `I2S_DRAIN` | ignored | `4124 = 4116` (mark all queued), resume TX DMA, then **sleep interruptibly until the consumer index catches the producer index** (`-ERESTARTSYS`/-512 on signal). i.e. "wait until everything queued has been played". | ioctl @0x1160-0x1228; hal write-complete @0x40797c |

Anything else returns `-524` (-ENOTSUPP-ish).

### `I2S_START` (0x80044e30) detail

- Bits 16..18 of the arg are an **output-port/GPIO mask**; the code does
  read-modify-write on `0xb8040008` (AR7240-family GPIO output register
  [INFERRED from the SoC memory map; not confirmed on this device]):
  bit16 -> GPIO16, bit17 -> GPIO28, bit18 -> GPIO13. **Bit set = that GPIO is
  driven low; bit clear = driven high** (@0x1098-0x10f8). `hal` forwards a
  per-request `ports` value here (see section 6; the Lua layer has a `ports`
  field). The mapping of GPIOs to the physical emitters (internal/mini-blaster
  jacks) is **UNKNOWN**.
- If the low 16 bits (`n`) are 0: queue start index `4124 := producer index`
  (@0x1104-0x1118).
- Then starts the DMA: if the one-shot flag `sc_buf_var+8292` (set in
  speaker `open`) is set, program TX descriptor base `0xb80a0018` = ring phys
  address and start DMA (`0xb80a001c = 2`), clear the flag; otherwise just
  resume (`0xb80a001c = 4`) (@0x1120-0x1158).
- `write()` alone never starts the DMA (it only fills descriptors and enables
  the TX-complete IRQ, `0xb80a004c |= 0x400`); **`I2S_START` must be issued
  after the data is queued.** Evidence: no DMA start in `ath_i2s_write`
  (0x434-0x70c).

## 4. write() and the DMA ring

`ath_i2s_write(file, buf, count, ppos)` (@0x434):

- Copies user data into 768-byte buffers, one descriptor each; descriptor word0
  gets the byte count (<=768, 12-bit field), owner bit 31 and bit 30 set once
  filled. Partial last buffer: descriptor length = remaining bytes, no padding
  (@0x580-0x65c).
- If the next descriptor is still DMA-owned (ring full) it sleeps
  interruptibly (`prepare_to_wait`/`schedule`) until the IRQ frees one; returns
  -ERESTARTSYS on signal. Returns bytes written. Blocking only; no O_NONBLOCK
  handling seen. Fault in copy -> -EFAULT.
- Descriptor layout (12 bytes, from `ath_i2s_open` loop @0x1f0-0x31c):
  word0 = `size[11:0] | length[23:12] | ... | owner[31]`; word1 = buffer physical
  address (low 28 bits); word2 = next-descriptor physical address (low 28 bits).
  Last descriptor links back to the first (ring). Software producer index is
  `sc_buf_var+4116`, consumer index `+4120`.
- IRQ (`ath_i2s_intr` @0x940, MBOX status `0xb80a0044`, bit 0x400 = TX descriptor
  complete): advances the consumer index, wakes sleepers; if it catches the
  producer it re-arms any newly queued descriptors and restarts DMA.
  **UNKNOWN**: exact underrun behaviour and the line idle level after the last
  descriptor.
- Data size per transfer is set by `I2S_DSIZE`; `hal` uses **16-bit**. With
  `I2S_MODE`=2 and 16-bit DSIZE `hal` treats the data as a pure bit-serial
  stream, so sample/word framing is irrelevant to IR as long as `hal`'s byte
  order is replicated (below).

### Byte order on the wire [INFERRED]

`hal`'s write wrapper (ops +0x2c @0x407728): rounds length **up to even**, then
swaps every adjacent byte pair (`lbu 0/1 ... sb 1/0`) before calling `write()`
in a short-write retry loop. So the bytes handed to `write()` are `B1 B0 B3 B2...`
where `B0 B1 B2 B3...` is the order the bits were packed (section 6). Taken with
the fact that `hal` is correct on real hardware, the I2S/DMA shifts out the
**original packed order, MSB of `B0` first**. Verify on device (section 8).

## 5. Clock / carrier programming

There is no "carrier" register. The carrier is synthesised in the bitstream; the
I2S **bit clock** is programmed so that `N` bits span one carrier period.

`hal` `hornet_i2s_clock` (@0x407440; debug strings `pulse_bclk_ns=%d`,
`i2s_bclk=%f`, `i2s_mclk=%f`, `div=%f`, `ath_stereo_clk=%08x`). With
`b = pulse_bclk_ns` (the intended time of one bit, ns):

```
bclk_Hz = 1.0 / (b / 1e9)
mclk_Hz = bclk_Hz * 4.0
D       = (2.0e8 / mclk_Hz) * 0.5        # = b / 40
arg     = (uint32)D << 16 | ((uint32)((D - floor(D)) * 65536.0) & 0xffff)
ioctl(fd, 0x80044e21, arg)               # I2S_FREQ
```

Constants read from `.rodata` @0x443ca0..0x443cc8 (1e9, 1.0, 4.0, 2e8, 0.5,
65536.0). The divider is a **16.16 fixed-point** value (integer part in the high
half, fraction in the low half), written verbatim to `0xb80b001c`. The 200 MHz
reference and the 4x MCLK:BCLK ratio come from the constants; the register
semantics are [INFERRED] from the arithmetic. Not yet confirmed: that the
hardware really yields a bit time of `b` ns for this `D` (cross-check with a
scope or the I2S read-back loopback).

How `hal` picks `b` for a carrier of period `X` ns (`ir_send_thread`, the large
unnamed function @0x42226c-0x422b04, labelled `MD5Update` by the disassembler):

1. `N = f(X)` = number of bit-clocks per carrier cycle, via helper @0x407230:
   `X<2251 -> 2`, `<2501 -> 4`, `<3000 -> 8`, `<12000 -> 10`, else `12`.
2. `b = ceil(X / N)` ns (`(X-1+N)/N`, @0x4225f8).
3. `pulse_bits = (duty% * N + 99) / 100`, `space_bits = N - pulse_bits`
   (@0x422638-0x422660); error `pulse bits is too large` if either > 64.
4. `hornet_i2s_clock(fd, b)` -> section above.

Carrier period `X` is accepted only in **[2000, 111111] ns** (~9 kHz..500 kHz;
@0x42243c-0x422448, error `IR frequency out of range`). Duty is clamped to
**<= 50 %** (@0x422460-0x42246c). Note the integer `N` set is tiny, so at 38 kHz
(X ~ 26316 ns) `N = 12`, `b = 2193 ns`, and each carrier cycle is 12 bits, e.g.
4 ones + 8 zeros at 33 % duty. Because `b` is derived by division and rounded
up, the realised carrier is `12*b`, slightly longer than `X`.

## 6. Bitstream construction (what we must replicate)

`hal` bit packer `bitbuf_*` (dynsym; `bitbuf_add` @0x4405bc, `_init` @0x440798,
`_flush` @0x4404f8): `bitbuf_add(bb, uint64 value, nbits)` appends the low
`nbits` bits of `value`, **MSB-first**, into a byte buffer (bit 7 of byte 0 is
the first bit). Buffer starts 8192 bytes, grows by 8192; when a byte-filled
chunk is complete and a flush callback is set it calls it (the callback is the
byte-swapping `write()` wrapper). `bitbuf_flush` pads with zero bits to a byte
boundary, writes out, and zeroes the buffer.

Section emitter (@0x422104). A section is a big-endian `u16 count` followed by
`count` big-endian `u16` entries; entry bit 15 = **mark (1)** / **space (0)**,
bits 14..0 = duration in **microseconds** (`1000*d` ns; `sum` helper @0x4220b0
masks `0x7fff`). For each entry, repeat carrier cycles until the accumulated
time (in units of the carrier period ns, stored in the packer context +0x1c)
reaches `1000*d - period/2` (rounded to nearest cycle):

- mark: `add(all-ones, pulse_bits)` then `add(0, space_bits)`
- space: `add(0, pulse_bits)` then `add(0, space_bits)` (i.e. N zero bits)

So **space time is quantised to whole carrier periods**, mark likewise.
Emission order for one send: `presilence` zero-bit cycles (header `u16`
at +14 in units of us, `*1000` ns; extended so the frame meets the requested
minimum duration), then the **"once" section**, then the **"repeat" section
`min_repeat` times** (header byte +16; `bitbuf_flush` between repetitions, so
each repeat is byte-aligned), then 32 zero bits of tail (`li s8,32`
@0x422948), flush, free. `ir_send_repeat`/`ir_send_cancel` re-enter the repeat
loop until cancelled.

Request header (bytes relative to the signal blob, `ir_send_thread` field loads
@0x4223f8-0x422530): `+0` type must be `1` or `0xff` (else `Expected IR
type`); `+1..4` carrier **period ns** (u32 BE); `+5` duty %; `+6..7`
presilence us; `+8` min_repeat; `+10..11` offset of once section; `+12..13`
offset of repeat section; `+14..15` trailing silence us (loaded, usage not fully
traced). (Offsets here are relative to the byte at hal's `+8`.)

Our `hp_ir_signal_t` maps cleanly: `carrier_hz -> X = 1e9/hz`, `us[]` pairs ->
section entries (bit 15 on marks), `once_n`/`rep_n` -> the two sections.

### Per-send call sequence in `hal` (order matters)

```
fd = open("/dev/i2s", O_WRONLY)              # ops+0x18, then:
ioctl(fd, I2S_DSIZE, 16)
ioctl(fd, I2S_MODE,  2)
ioctl(fd, I2S_VOLUME, 15)                    # @0x407274, one-time setup per open
ioctl(fd, I2S_FREQ, div16_16(b))             # per carrier, section 5
bitbuf: write(fd, swapped_chunks...)         # as buffer fills / at flush
ioctl(fd, I2S_START, (ports<<16)|0)          # ops+0x30
ioctl(fd, I2S_DRAIN, 0)                      # ops+0x34, blocks until played
```

Whether `hal` keeps one fd open across sends or reopens per send is
**UNKNOWN**; the ops table suggests it opens on demand (`ir_send_thread`
calls ops+0x18 each time @0x4222c4 and ops+0x1c on error paths) but the task
brief says it holds the fd for its lifetime. Stock log strings name the
mic-side read path (`ir_read`, learning) which uses the byte-inverting read
wrapper @0x407858 (`nor`), i.e. received bits are active-low.

## 7. Confidence

| item | confidence | basis |
|------|-----------|-------|
| node `/dev/i2s`, major 253, write-only open, EBUSY when held | high | module strings + code, task facts |
| ioctl numbers `0x80044e20..24,30,31`, `0xc0044e25..27`, by-value args | high | direct compare in `ath_i2s_ioctl`, same constants in `hal` |
| sample size 16-bit, MODE 2, VOLUME 15 as used by `hal` | high | `hal` @0x4072c0-0x4072fc |
| ring geometry (512 x 768 B), write blocks when full, DMA not started by write | high | `ath_i2s_open`/`write` disassembly |
| bitstream-with-carrier model (1 bit per bclk, MSB-first, marks=1) | high for structure, medium for polarity | `bitbuf_add`, emitter @0x422104 |
| 16.16 divider formula for `I2S_FREQ` | medium-high arithmetic; unverified physically | constants + FP call chain |
| byte-pair swap needed before `write()` | medium | `hal` write wrapper; direction not hardware-verified |
| GPIO meaning of `ports` bits | low | register map is [INFERRED] |
| `I2S_MODE` bit meaning, line idle level, underrun behaviour | unknown | |

## 8. Unknowns and cheapest experiment

Unknowns: physical bit time vs `D`; wire bit order (swap direction); mark
polarity; whether the `ports` mask is mandatory to light the LED; idle level.

Cheapest on-device experiment (requires user sign-off; do **not** run before
that): with the stock stack paused only for the duration of the test, have our
tool `open(O_WRONLY)`, set DSIZE=16/MODE=2/VOLUME=15, program a divider for a
known `b`, queue a short 32-bit-ish pattern (e.g. `0xAAAA` repeated) and
`I2S_START` with ports=0, while observing the emitter with a phone camera or
IR-capable receiver and, better, the hub's own mic-side read loopback
(`ir_loopback_test` path) or a logic probe. That one run fixes bit time, order
and polarity together. Because stopping `hal` is disruptive, prefer a
read-only `open(O_RDONLY)` of `/dev/i2s` first (independent mic slot, does not
touch the speaker slot) only to confirm the node and ioctl plumbing.

## 9. Bench result (2026-11-16, live hub)

Tool: `tools/ir_bench.c` (soft-float MIPS build, all parameters are arguments).
Target: Samsung `PowerToggle` (NEC-like `0xE0E040BF`, 37900 Hz, 4500/4500 header,
560/1690 bits, 46780 us final gap), 3 repeats. Oracle: HA
`media_player.un55js9000` (was `off`). `hal`/`lua` were killed first, so the
speaker slot was free. Computed stream for all attempts: X=26385 ns, N=12,
duty 33 % (4 ones + 8 zeros per cycle), bit time 2199 ns, divider `0x0036f999`
(54.975), 147,392 bits = 18,424 bytes, 324.1 ms.

| # | variable changed | ports | swap | invert | syscalls | TV state before -> after |
|---|------------------|-------|------|--------|----------|--------------------------|
| 1 | baseline | 0 | 1 | 0 | all rc 0, write 18424 | off -> off |
| 2 | swap off | 0 | 0 | 0 | all rc 0 | off -> off |
| 3 | invert (swap 1) | 0 | 1 | 1 | all rc 0 | off -> off |
| 4 | ports = 1 | 1 | 1 | 0 | all rc 0, DRAIN 323.5 ms | off -> off |
| 5 | **ports = 2** | 2 | 1 | 0 | all rc 0, DRAIN 323.5 ms | off -> **on** (volume 0.21) |

Stopped at the first success.

Confirmed:
- **ports = 2 (`START` arg `0x20000`, bit 17, the GPIO28 line) drives the
  built-in emitter.** ports 0 and 1 emit nothing visible; bit 17 is required.
  Only one `ports` value was tried after 1, so bits 16 and 18 (and combinations)
  remain unmapped, and whether bit 17 is "emitter select" or "enable" is unknown.
- **Bit time**: `DRAIN` blocked 323.5 ms for a stream computed at 324.1 ms, so
  divider `D = ceil(X/N)/40` yields the intended 2199 ns/bit to within 0.2 %
  (as far as the DMA clock; the optical carrier was not scoped, but the TV
  decoded a 37.9 kHz NEC frame, so it is right to within receiver tolerance).
- **Byte order**: swapping adjacent byte pairs (`swap=1`, as `hal` does) with
  MSB-first packing works. The un-swapped stream (attempt 2) was only tried with
  ports 0, so it is not disproved independently of the ports result.
- **Polarity**: mark = 1 (not inverted) works. Inverted was tried only with
  ports 0, so not disproved independently either.
- Call sequence `open(O_WRONLY)`, DSIZE 16, MODE 2, VOLUME 15, FREQ, write,
  START, DRAIN, close works as described; every call returned 0 and the single
  18,424-byte write returned in full (0.2 ms, DMA not started until START).
- `N=12` / carrier-period/12 rounding is sufficient for a Samsung TV.

Hazard found: a **read-only open of `/dev/i2s` (mic slot) followed by `close()`
hung in the kernel** (`ath_i2s_close`, process spinning in state R, SIGKILL had no
effect) while `hal` held the speaker slot. Afterwards a restarted `hal` died
immediately (zombie `[hal]` children) and only a `reboot` restored the stock
stack. Do not `--probe` while `hal` runs; a plain reboot cleared it.

Still unknown: meaning of ports bits 16 and 18 and whether bit 17 only selects
the emitter; whether 0/1 would work with other ports values; the I2S_MODE bit;
idle level and underrun behaviour; mic-slot close hang root cause.
