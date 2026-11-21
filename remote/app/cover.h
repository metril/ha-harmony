#ifndef HP_REMOTE_COVER_H
#define HP_REMOTE_COVER_H

/* Now Playing cover art cache (cover_chunk, RF code 42). A cover is 1760 bytes: 48x72 at 4 bpp (1728, high nibble first)
 * then 16 RGB565 little-endian palette entries (32). Up to RA_COVER_SLOTS covers are cached by cid in static memory
 * (chunks may arrive in any order, repeated or partly overlapping); one static 48x72 RGB565 buffer holds the expansion of
 * the cover on show. An incomplete cid is a miss. No allocation, no LVGL. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RA_COVER_W 48
#define RA_COVER_H 72
#define RA_COVER_BYTES 1760
#define RA_COVER_SLOTS 4

void ra_cover_reset(void);
/* Stores bytes [off, off+n) of cover `cid` (0 is no cover: ignored). 1 = this chunk completed the cover; 0 = stored (or
 * already held); -1 = out of range. A new cid takes a free slot, else the least recently used. */
int ra_cover_chunk(uint32_t cid, unsigned off, const uint8_t *d, size_t n);
bool ra_cover_have(uint32_t cid);
/* The 48x72 RGB565 expansion of `cid` (static, row-major, stride 96 B), or NULL while incomplete / unknown. */
const uint16_t *ra_cover_expand(uint32_t cid);

#endif
