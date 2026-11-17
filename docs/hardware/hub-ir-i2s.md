# Hub IR output via `/dev/i2s`

Our own description of how the stock Harmony Hub emits IR, derived from static
analysis of the stock `ath_i2s.ko` and `/usr/bin/hal`. **Nothing here has been
shown to make light come out of the hub from our own code**: the one apparent
on-device success (section 9) was retracted, and ~10 later emissions reached no
device. Static pass 2 (this revision) found that our `START` argument model was
wrong where it mattered (section 3, section 12).

Tags: **[CODE]** = read directly from disassembly (offset given);
**[INFERRED]** = conclusion not visible in code; **UNKNOWN** = gap.

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
| `0x80044e23` | `I2S_MODE` | int | arg==2: clear bit 14 (`0x4000`) of `0xb80b0000`; any other: set it. [CODE] Bit meaning [INFERRED]: stereo/mono select (Atheros SDK `STEREO_CONFIG` layout, recalled not verified). `hal` passes **2** = bit clear = stereo. It does not touch GPIO or any clock mux. | ioctl @0x105c-0x1090; hal @0x4072dc |
| `0x80044e24` | (unused) | - | [CODE] no-op, returns 0, touches nothing (`move v0,zero` @0xeb8). Not issued by `hal` (its I2S wrappers use only `0x4e20..23,30,31`). | @0xeb8 |
| `0xc0044e25` | (unused) | - | [CODE] no-op, returns 0 (`beq`->@0xeb8 @0xe84). Not referenced by `hal`. | @0xe84 |
| `0xc0044e26` | `I2S_PAUSE` | 0 = TX, else RX | `ath_i2s_dma_pause`: writes 1 to TX ctrl `0xb80a001c` (RX: `0xb80a0024`), sets state 2 | ioctl @0xea8; `dma_pause` @0xdc4. Not used by `hal`. |
| `0xc0044e27` | `I2S_RESUME` | 0 = TX, else RX | `ath_i2s_dma_resume`: writes 4 to ctrl reg, sets state 1 | ioctl @0xec0; @0x70c. Not used by `hal`. |
| `0x80044e30` | `I2S_START` | `(ports<<16) \| (n & 0xffff)` | see below | ioctl @0x1094-0x1158; hal write-start @0x4079dc (`sll a2,a2,16; or`) |
| `0x80044e31` | `I2S_DRAIN` | ignored | `4124 = 4116` (mark all queued), resume TX DMA, then **sleep interruptibly until the consumer index catches the producer index** (`-ERESTARTSYS`/-512 on signal). i.e. "wait until everything queued has been played". | ioctl @0x1160-0x1228; hal write-complete @0x40797c |

Anything else returns `-524` (-ENOTSUPP-ish).

### `I2S_START` (0x80044e30) detail

Decoded from `ath_i2s_ioctl` @0x1094-0x1158 [CODE]. Arg (`a3`) layout:

| arg bits | consumed as |
|----------|-------------|
| 0..15 (`n`) | only tested for `== 0` (@0x1100 `andi s2,s2,0xffff; bnez`). If 0: `sc_buf_var[4124] := sc_buf_var[4116]` (queue start index := producer). Any other value just skips that store. **Not a sample count or buffer index.** |
| 16 | `GPIO16` level in `GPIO_OUT` |
| 17 | `GPIO28` level in `GPIO_OUT` |
| 18 | `GPIO13` level in `GPIO_OUT` |
| 19..31 | ignored |

GPIO write: `a2 = *(0xb8040008)` (AR933x `GPIO_OUT`; `GPIO_OE`=+0, `IN`=+4, `OUT`=+8,
`FUNC`=+0x28; register map from memory of the ath79 headers, **[INFERRED]** but
corroborated by `init_module` below), then one read-modify-write of three bits and
`*(0xb8040008) = a1` + readback (@0x1094-0x1100):

- bit16 `0x10000`: arg bit **set** -> `and ~0x10000` (GPIO16 **low**); clear -> `or` (GPIO16 **high**) (@0x10a0-0x10bc)
- bit17 `0x10000000`: set -> `and 0xefffffff` (GPIO28 **low**); clear -> `or` (**high**) (@0x10c0-0x10dc)
- bit18 `0x2000`: set -> `and ~0x2000` (GPIO13 **low**); clear -> `or` (**high**) (@0x10e0-0x10f4)

So **a set bit drives that GPIO low; `ports = 0` drives all three high**. No other
register is written by START's GPIO part. Because the write is a whole-word RMW of
`GPIO_OUT`, **START always rewrites all three lines**, and the pins keep the last value
until the next START (nothing restores them on `DRAIN` or `close`; [CODE] `ath_i2s_close`
@0x758-0x8f0 and the DMA helpers never touch `0xb804xxxx`).

Direction/mux are configured **once at module load**, not in START or open
(`init_module` @0x12a8) [CODE]:

- `*(0xb8040028) |= 0x24000000` (@0x1374-0x1388): `GPIO_FUNC` bits 26 and 29 [INFERRED: I2S pin-mux enables; not verified].
- `*(0xb8040000) |= 0x10012000` (@0x1390-0x13a0): `GPIO_OE` bits **28, 16, 13** set (output), exactly the three
  START-controlled lines. This is what makes the mapping bit16->GPIO16, bit17->GPIO28, bit18->GPIO13 solid.
- `ar7240_reset(1)`, 1 ms delay, default divider `0xb80b001c = 0x00215999`, `STEREO_CONFIG = 0x00a21302`, `i2s_reset`.

**GPIO16 gates the built-in emitter** [HARDWARE, 2026-11-17, section 9b]: `ports = 1`
(arg `0x10000`, GPIO16 low, GPIO28/GPIO13 high) emits IR that a phone camera sees and a
Samsung TV obeys. This is consistent with the static decode: stock's `ports = 7` drives all
three low, so `ports = 1` is a **subset** of stock, and `ports = 2` (GPIO28 low, **GPIO16
high**) is the one case that gates the built-in emitter *off* — which is exactly why the
retracted bit-17 run emitted nothing. What GPIO28 and GPIO13 gate is still unknown; the
three-line width fits "one gate per emitter group" (built-in plus two mini-blaster jacks),
and this hub has no blaster cables attached, so driving them low costs nothing and stock
does it unconditionally.

**What stock sends** [CODE]: `hal`'s `ir_send` RPC handler (@0x424430, json format
`{enable:b, keyLatency?:i, ports?:i}`, strings @0x44a140/0x44a150/0x44a15c) initialises
`ports = 7` (`li t3,7` @0x42445c, stored @0x424480), overwrites it only if the key is
present, then passes `ports & 7` (@0x424504). `ir_loopback_test` (@0x423e78) and the
`/ir/ir_test` RPC (@0x424c40, default 7, `& 7` @0x424c8c) also use 7. The value is stored at
request+4 (`ir_send` @0x423d08), read back in `ir_send_thread` (`lw a2,4(s3)` @0x42298c) and
passed with `n=0` to ops+0x30 = `hornet_i2s_write_start` (@0x4079dc: `a2<<16 | (a1&0xffff)`).
Every device in our hub backup has `controlPort: null` (`backups/phase1/hub/pre-root-config/devices.json`),
so stock sends with the default **ports = 7, START arg `0x70000`: GPIO16, GPIO28 and GPIO13 all low**
[INFERRED that the Lua layer omits `ports` for these devices; the `null` is the only evidence].
7 has never been tried and does not need to be: `ports = 1` is proven sufficient (section 9b),
and the daemon default is now `0x01` (section 10).

- If the DMA is not yet running (one-shot flag `sc_buf_var+8292`, set by speaker `open`):
  program TX descriptor base `0xb80a0018` = ring phys address (`ath_i2s_dma_desc` @0x0) and start
  (`0xb80a001c = 2`), clear the flag; otherwise just resume (`0xb80a001c = 4`) (@0x1120-0x1158).
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

`hal`'s write wrapper (ops +0x2c @0x407728) [CODE]: rounds length **up to even**, then
swaps every adjacent byte pair in place (`lbu 0/1 ... sb 1/0` @0x407774-0x407798) before calling `write()`
in a short-write retry loop. So the bytes handed to `write()` are `B1 B0 B3 B2...`
where `B0 B1 B2 B3...` is the order the bits were packed (section 6). Taken with
the fact that `hal` is correct on real hardware, the I2S/DMA shifts out the
**original packed order, MSB of `B0` first** [INFERRED; unverified physically].
Note the boot-time `STEREO_CONFIG` default `0x00a21302` has bit 17 set while every
`DSIZE` value (`0x00a01302`...) has it clear; `hal` always issues DSIZE, so the
post-open state is identical for stock and our tool as long as we also issue DSIZE (we do).

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

Independent corroboration of `D = b/40` [CODE]: the driver's own boot default divider is
`0x00215999` (`init_module` @0x13cc-0x13d8) = 33.35 -> `b = 1334 ns`, and `hal`'s ops-table
header word at +0x08 is `0x536 = 1334` (the capture bit time, section 11). Three
unrelated places agree on the units. Also `DRAIN` blocking for the predicted
`bits * b` (section 9) shows 16-bit DSIZE serialises one bit per bit-clock with no slot padding
(the 32-bit `I2S_WORD_SIZE` bit 11 is clear in the 16-bit config value).

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

Verification of the model against `hal` (pass 2) [CODE unless noted]:

| claim | verdict | evidence |
|-------|---------|----------|
| 1 bit per bit-clock, carrier baked in | confirmed | emitter @0x422104-0x422258: per carrier cycle `bitbuf_add(all-ones,pulse_bits)` then `add(0,space_bits)` |
| mark = 1-bits, space = 0-bits, no inversion in `hal` | confirmed | mark: `s1=s0=-1` (@0x422184-0x422188); space: 0; no `nor` on the write path (only the read wrapper @0x407938 inverts) |
| MSB-first | confirmed | `bitbuf_add` mask starts `0x80` and shifts right (@0x440630-0x44068c) |
| byte-pair swap, even length | confirmed (code); physical direction unverified | @0x407728-0x407798 |
| `N` table | confirmed | @0x407230-0x407270 (`<2251:2, <2501:4, <3000:8, <12000:10, else 12`) |
| `b = ceil(X/N)` | confirmed | `(X-1+N)/N` @0x4225f8-0x422604 |
| duty = `min(duty,50)`, `pulse = (duty*N+99)/100`, `space = N-pulse` | confirmed | `sltiu 51 / movn 50` @0x422460-0x42246c; @0x422638-0x422660 |
| `ir_bench --duty` is a no-op | **refuted for the current source**: `--dry-run --out` at duty 25/33/50 gives 3/9, 4/8, 6/6 pulse/space bits and three different md5s | `tools/ir_bench.c` L95-98, run on host; the plan's "no-op" claim does not reproduce (it may have described an older on-device binary) |
| tail | 32 zero bits then flush | `li s8,32` @0x422948 |
| stereo / channel interleave matters? | no [INFERRED] | 16-bit slots, no padding, SD is continuous across L/R; DRAIN time matches `bits*b`. No code selects a channel for IR. |

Our `ir_bench` stream construction matches `hal`'s except: it aligns each repeat to 16 bits
(`hal` flushes to a byte boundary and the wrapper rounds each chunk to even), and `hal` writes in
chunks as its 8192-byte buffer fills while we issue one `write()`. Neither changes the bit sequence.
**No difference in bit content, order, polarity, carrier or call sequence between `ir_bench` and `hal`
was found; the only difference found is the START `ports` value (section 3).**

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

`hal` ops table [CODE] (data @0x4525d0, file offset 0x525d0; `ir_init` @0x424244 stores its address):
`+0x00=0xc0 +0x04=1500 +0x08=1334 +0x0c=768 +0x10=192`, `+0x14`=@0x407230 (period->N),
`+0x18`=@0x407274 open, `+0x1c`=@0x40742c close, `+0x20`=`+0x28`=@0x407440 clock, `+0x24`=NULL,
`+0x2c`=@0x407728 write, `+0x30`=@0x4079dc start, `+0x34`=@0x40797c complete (DRAIN),
`+0x38`=@0x407858 read, `+0x3c`=NULL (no pre-send hook). `ir_send_thread` opens **once**
(ops+0x18 @0x4222c4, fd kept at `100(sp)`), and the per-request loop (back-edge @0x422764 ->
@0x4223b8) never closes it; ops+0x1c is only reached on error (@0x4227b8). So stock does
DSIZE/MODE/VOLUME once per process life and `FREQ` + `write` + `START` + `DRAIN` per send. Stock log strings name the
mic-side read path (`ir_read`, learning) which uses the byte-inverting read
wrapper @0x407858 (`nor`), i.e. received bits are active-low.

## 7. Confidence

| item | confidence | basis |
|------|-----------|-------|
| node `/dev/i2s`, major 253, write-only open, EBUSY when held | high | module strings + code |
| ioctl numbers `0x80044e20..23,30,31`, by-value args; `0x4e24`, `0xc0044e25` are no-ops | high | `ath_i2s_ioctl` compares, same constants in `hal` |
| sample size 16-bit, MODE 2, VOLUME 15 as used by `hal` | high | `hal` @0x4072c0-0x4072fc |
| ring geometry (512 x 768 B), write blocks when full, DMA not started by write | high | `ath_i2s_open`/`write` disassembly |
| bitstream structure: 1 bit/bclk, carrier baked in, MSB-first, mark=1, N/duty/b formulas | high | section 6 table |
| `START` arg: bits 16/17/18 -> GPIO16/GPIO28/GPIO13 `OUT` level, set = low; bits 0-15 only `n==0` test | high (decode); high that these GPIOs are outputs | @0x1094-0x1100; `init_module` OE `0x10012000` |
| stock default `ports = 7` (`START` arg `0x70000`) | high for the code default; medium that stock actually sends 7 for our devices | @0x42445c/0x424504; `controlPort: null` in backup |
| GPIO16 (START bit 16) gates the built-in emitter | **high** | hardware: camera flicker + TV reacted, section 9b |
| what GPIO28 and GPIO13 gate | **unknown** | no binary evidence; not needed (`ports = 1` suffices) |
| 16.16 divider `D = b/40` | high | boot default 0x215999 = 1334 ns = hal +0x08; DRAIN duration matched |
| byte-pair swap direction (physical), mark polarity, N=12 at 37.9 kHz | high | hardware: the TV decoded a real Samsung frame, section 9b |
| one 108 ms frame is enough for a Samsung TV (no `min_repeats`) | high | hardware: verified at 25, 3 and 1 repeats, section 9b |
| `I2S_MODE` bit meaning, line idle level, underrun behaviour | unknown | |

## 8. Unknowns and cheapest experiments

Superseded by sections 9b (what the hardware settled) and 12 (what is left). Remaining unknowns:
what GPIO28 and GPIO13 gate; idle level; underrun behaviour; carriers that need N != 12 (only
37900 Hz has been driven); the IR **capture** path (section 11, never run).

## 9. Bench result (2026-11-16, live hub) - SUCCESS CLAIM RETRACTED

> **Retracted 2026-11-17** (see `docs/plans/2026-11-17-phase3-hardware-slice.md`). The row-5 "off -> on"
> was almost certainly a coincidence (the TV wakes unprompted; later ~10 emissions, including one right
> after a stock send, reached no device). The table below is kept as a **log of what was run**, not
> as evidence. The only facts it supports: every ioctl returned 0, `write` returned in full, and `DRAIN`
> blocked for the predicted duration. Nothing in it shows IR leaving the hub.
>
> **Superseded by section 9b**, which confirmed IR on hardware with `ports = 1`. Note that row 4
> here *did* use `ports = 1` and saw no reaction - on an oracle (HA's `media_player` state) too
> slow and too noisy to show it. The working mask was tried in this very run and missed.

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

Established by this run (and nothing else):
- Call sequence `open(O_WRONLY)`, DSIZE 16, MODE 2, VOLUME 15, FREQ, write, START, DRAIN, close
  completes with rc 0 everywhere; the single 18,424-byte write returned in full (0.2 ms; DMA not started until START).
- `DRAIN` blocked 323.5 ms against a predicted 324.1 ms: the divider formula gives the intended bit time
  to ~0.2 % **as seen by the DMA clock**. (Also independently corroborated by the boot default divider, section 5.)

**Retracted / not established:** "ports = 2 / bit 17 drives the built-in emitter", "byte order works",
"mark polarity works", "N=12 is sufficient for a Samsung TV". None of these was observed. Static analysis
(section 3) shows ports 0, 1 and 2 only ever drive subsets of the three select GPIOs that stock sets
together (default 7).

Hazard found: a **read-only open of `/dev/i2s` (mic slot) followed by `close()`
hung in the kernel** (`ath_i2s_close`, process spinning in state R, SIGKILL had no
effect) while `hal` held the speaker slot. Afterwards a restarted `hal` died
immediately (zombie `[hal]` children) and only a `reboot` restored the stock
stack. **Root cause found statically [CODE]** (`ath_i2s_close` @0x758-0x860): for the mic slot,
close tests `sc_buf_var[8268] == 1` (RX DMA running). `ath_i2s_open` sets that to 0 and only the first
`read()` starts RX DMA (`ath_i2s_read` @0xd20-0xd5c) and sets it to 1. If it is not 1, close skips
the "clear all owner bits + resume" branch (@0x7d4-0x800) and goes straight to the wait loop
@0x818-0x830, which sleeps `schedule_timeout_interruptible(250)` per descriptor until the DMA clears
each descriptor's owner bit (set at open for RX, @0x2e0-0x2f4). With no DMA running that never
happens, and since the loop ignores signals SIGKILL cannot stop it. **Always `read()` at least once
before closing a mic fd** (section 11). `ir_bench --probe` used to do open+close with no read and so
hung by construction; it now write-opens the **speaker** slot instead (the same safe availability test
the daemon's startup probe uses), and carries this hazard in a comment so it is not reintroduced.

Still unknown: what GPIO28 and GPIO13 gate (section 3); the I2S_MODE bit; idle level and
underrun behaviour.

## 9b. Bench result (2026-11-17, live hub) - IR CONFIRMED, `ports = 1`

Same tool and same Samsung stream as section 9. **Oracle: a phone camera aimed at the
emitter window** (940 nm reads as a violet flicker), with the TV's on-screen volume bar as a
second, independent witness. The HA `media_player` state was *not* used: it lags by minutes
and the TV wakes unprompted, which is what produced the retracted section 9 result.

| ports | arg | GPIO16/28/13 | camera | TV |
|-------|-----|--------------|--------|-----|
| 1 | `0x10000` | low/high/high | **flicker** | **volume bar moved** |
| 2 | `0x20000` | high/low/high | nothing | nothing |

Run one variant at a time with a human confirming each; a batched sweep cannot tell you which
variant did it, and that is how the first wrong conclusion was reached.

**Established:**
- **`ports = 1` (START bit 16) drives the built-in emitter.** Byte order (adjacent-pair swap),
  mark polarity (mark = 1-bits, no inversion), N=12 at 37900 Hz and the divider formula are all
  therefore correct as implemented - the TV decoded a real Samsung frame.
- **One 108 ms frame is enough.** Verified at 25, 3 and 1 repeats; the single frame worked.
  So the `raw: {hz, us}` config form needs no `min_repeats` field, and repeat fidelity is a
  refinement, not a blocker.
- Full path end to end: HA's own `HarmonyHubClient` -> WS `ir_send` -> our MIPS daemon's
  non-sim path -> `/dev/i2s` -> TV obeyed.

The only error in the pre-2026-11-17 model was the port mask. Everything upstream of START was
already right, which is why every ioctl returned 0 and `DRAIN` blocked for the full predicted
duration: the daemon was emitting a correct waveform into a gated-off output.

## 10. Daemon backend (`hub/io/hw_sinks.c`)

> **Status: IR path verified on real hardware (section 9b).** `--ir-ports` defaults to `0x01`
> (START bit 16, the built-in emitter).

Implements the section 9 sequence behind `hp_ir_sink`; host-tested with injected syscalls and
diffed byte-for-byte against `tools/ir_bench.c --out`. Engine `port_mask` bit n maps to START bit
16+n (`--ir-ports`, default `0x01` = bit 16, the built-in emitter; mask 0 = all configured ports;
an empty result is `-HP_E_INVAL`). `send` ends after START and leaves DRAIN+close to
`hp_hw_service()` (or the next send), so the daemon loop never blocks. Not yet verified on the
device: the deferred-DRAIN flow, startup write-open/close probe, hold-open-after-START, any
carrier other than 37900 Hz.

## 11. Capture (receive) path

Static analysis only; **never run**. All [CODE] unless tagged.

**What it is.** The hub's IR *receiver* is read through the same device: `hal`'s learning thread
(`ir_read_thread`, @0x423428), `ir_cap` and `ir_loopback_test`/`ir_loopback_thread` (@0x423dd8/@0x4239a0)
open `/dev/i2s` with flags 0 (ops+0x18 with `a0=0`, @0x4234b8 and @0x423a00), i.e. the **mic slot**, and
`read()` it through ops+0x38 (@0x407858). The data are raw one-bit-per-bit-clock samples of the receiver
line (the learner measures carrier frequency, strings `loopback frequency %d`), so a process can capture
ambient IR or the hub's own emission if the receiver sees it.

**Driver side** (`ath_i2s_open` @0x90, `ath_i2s_read` @0xacc):
- Mic slot = 128 descriptors x 768 B (`li s2,128` @0x178) = 98,304 B = 786,432 bits. That is 1.05 s of
  history at 1334 ns/bit, 1.73 s at 2199 ns/bit. EBUSY (`"I2S mic busy"`) only if the mic slot is
  already open; **independent of the speaker slot**, so it can be opened while `hal` holds the speaker.
- No ioctl is needed to start capture. The first `read()` returns **0** and starts the RX DMA
  (`dma_desc` + `dma_start(1)` @0xd20-0xd5c; `0xb80a0020`/`0xb80a0024 = 2`); later `read()`s block
  (interruptibly) until a descriptor completes and return **768 B per completed descriptor**.
- **Always pass a buffer >= 768 B and a count that is a multiple of 768.** The copy length is hard-coded
  768 (`li a2,768` @0xcb8) while the descriptor length is re-armed to `min(count,768)` (@0xc34-0xc3c);
  `hal` passes 192 into a 776-byte stack buffer (ops+0x10, buffer size `(192*4+14)&~7`) and relies on that.
- **Never `close()` a mic fd that has not been `read()` at least once** (section 9 hazard): close then spins
  forever, ignoring signals, and only a reboot recovers. After at least one read RX state is 1 and close
  clears the owner bits and returns. Any capture tool must therefore `read()` before any error path that
  closes - including the failure paths, which is the easy one to get wrong.
- Bit clock and `STEREO_CONFIG` are **shared with TX** (`0xb80b001c`, `0xb80b0000`). `I2S_FREQ` with a value
  different from the register pauses *both* DMAs, runs `i2s_reset` (MBOX reset `0xb80a0058 = 0xff`) and
  resumes them (@0xf1c-0xfc4), and the resume of RX (`0xb80a0024 = 4`) does **not** reprogram the RX
  descriptor base [INFERRED risk: a capture can die when a FREQ change lands mid-capture]. `FREQ` with the
  value already in the register returns immediately (`beq v1,a3` @0xf24), so **set the capturer's divider to
  exactly the divider the emitter will use and the emitter's own FREQ becomes a no-op.**

**Setup `hal` itself uses.** Learning (`ir_read_thread`): no clock ioctl (ops+0x24 is NULL, @0x4234d4
skips), so samples at whatever divider is current; the boot default is `0x00215999` = **1334 ns/bit**
(749.6 kHz, ~20 samples per 38 kHz cycle), and hal's ops+0x08 is the same 1334. Loopback
(`ir_loopback_thread`): ops+0x20 = clock with ops+0x04 = **1500 ns** first (@0x423a94, divider 37.5 =
`0x00258000`), then reads. `ir_loopback_test` then emits a canned 22-byte signal with `ports = 7`
(@0x423e78-0x423e90): hal's own self-test is "emit on all ports while the receiver listens", which suggests
the receiver does see the emitter [INFERRED].

**Data format.** `hal`'s read wrapper (@0x407858; loop @0x407924-0x407950) reads until its count is met,
then for every byte pair does `nor` (bitwise invert) **and** swaps the two bytes: so the raw bytes from
`read()` are the *active-low, pair-swapped* mirror of the TX format. After undoing both, bits are MSB-first,
one per bit-clock; carrier-present reads as 1 after inversion [INFERRED from the inversion + learner].
`hal`'s run-length decoder (@0x423250, caps at 1022 transitions) is the reference.

**Proposed capture experiment (describe only).** A tiny `ir_capture` tool:
`open(O_RDONLY)`; `ioctl(FREQ, D)` with the divider that will be used (stock Samsung 37.9 kHz -> X=26385 ns,
N=12, b=2199 ns, `D=0x0036f999`; compute per carrier); `read(768)` once (returns 0); loop `read(768)` into a file,
logging a monotonic timestamp per chunk, for ~3 s; **drain and close cleanly**. Trigger in order:
1. **Calibration**: press the TV's own remote at the hub. This proves the receiver works and shows a known
   frame's polarity, bit order and carrier in our decoder before trusting anything else.
2. Stock emission via HA `remote.send_command` (no quiesce needed; mic slot is independent).
3. Our emission with `hal` quiesced, capturer started first, same divider.
Diff in the run-length domain (mark/space durations, carrier period, repeat structure), not raw bytes.
If the receiver sees stock but not ours, the problem is upstream of the bit stream (section 12).

## 12. Why our emission was inert: RESOLVED (hypothesis 1)

> **Resolved 2026-11-17 by section 9b.** The cause was hypothesis 1 - the wrong select line -
> though not in the form predicted: the fix was not stock's `ports = 7` but `ports = 1`
> (GPIO16 alone). `ports = 2` was the single worst choice available, because it is the one
> value that drives GPIO16 *high* and so gates the built-in emitter off. Hypotheses 2-5 are
> ruled out as explanations of the inert emission: with only the port mask changed, the same
> bitstream, clock and call sequence drove a real TV. They are kept below because the
> register-snapshot and capture experiments remain useful for the unexplored parts
> (what GPIO28/GPIO13 gate, carriers needing N != 12, IR learning).
>
> The process lesson, recorded because it cost the most: a single unreplicated observation on a
> self-triggering oracle is not evidence. The TV's own wake behaviour manufactured a false
> positive that then shipped as a code default.

1. **We never drove the select lines stock drives (`ports` != 7). Likelihood: high.** [CONFIRMED as
   the cause, with the corrected mask `ports = 1`; see the banner above.] START rewrites
   GPIO16/GPIO28/GPIO13 together, active-low. Stock sends `ports=7` (all three low) by default; we tried
   0, 1, 2 only (bench default 0, daemon default 2). If the emitter path needs GPIO16 and/or GPIO13 low in
   addition to or instead of GPIO28, ports=2 emits nothing. The earlier "prime with a stock send, then
   quiesce and run ir_bench" test was also **invalid**: our START with ports=2 overwrote GPIO16/GPIO13
   back to high, undoing the primed state.
   *Experiment:* with `hal` quiesced and a phone camera on the emitter (940 nm shows as a violet flicker),
   run the same 324 ms stream with `--ports 7`, then 0 through 6. First run 7. ~0.35 s each. Any flicker
   identifies the mask; then repeat with the TV.
   *Even cheaper, no emission by us:* read `GPIO_OUT` (`0x18040008`) and `GPIO_OE` (`0x18040000`)
   (e.g. a read-only `/dev/mem` mmap, if present) before and after one stock `remote.send_command`.
   Bits 16/28/13 of OUT after the send show stock's ports directly (expected all 0 for 7), and OE should
   read bits 28/16/13 = 1.
2. **Something outside this driver also gates the emitter. Likelihood: medium.** The kernel exposes
   `AR7240_GPIOC` (major 77) and `hal` holds nothing, so a GPIO-driven enable (IR power, mux, or one of
   these three pins shared with an LED) could be set by another process (e.g. the Lua layer, `input0`
   driver sysfs) and left set; our quiesce may have changed it, or it may be reset by whatever restarts.
   *Experiment:* snapshot OE/OUT/FUNC (`0x18040000/08/28`) idle (stock running), after a stock send, after
   quiesce, and after our emission, and diff; also `grep -l AR7240_GPIOC` over the device's binaries and scripts.
   (The squashfs root in the local backup does not extract with `unsquashfs` here, lzma error 9, so
   the root filesystem was not searched.)
3. **Bit content or clock differs in some way static analysis cannot see. Likelihood: low-medium.** No
   difference was found between `ir_bench` and `hal` (section 6), `DRAIN` timing matches, but physical bit
   order/polarity/carrier are still unverified.
   *Experiment:* the capture of section 11 (calibrate with the real remote, then compare stock vs ours).
4. **I2S SD is not muxed to the emitter. Likelihood: low.** `GPIO_FUNC |= 0x24000000` is set once at module
   load [CODE]; stock and ours share it. *Experiment:* register snapshot as in item 2 (expect bits 26 and 29 set).
5. **Carrier or duty marginal for the receiver. Likelihood: low.** Cannot explain "nothing visible on the
   camera"; only relevant once something flickers.

**What was wrong in the old model:** (a) `START` bit 17 as an "emitter select/enable" was never evidenced;
the code only shows three independent, active-low GPIO levels and stock sets all three by default;
(b) the "stock priming" test could not work; (c) the claim that `ir_bench --duty` is a no-op does not hold for the
current source; (d) the `--probe` hang is a driver quirk with a known cause, not an unexplained fault.
**What survives:** the ioctl set, the 512x768 ring, the divider formula, and the whole bitstream recipe
(section 6 table).
