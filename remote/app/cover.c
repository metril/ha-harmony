#include "remote/app/cover.h"

#include <string.h>

typedef struct {
  uint32_t cid; /* 0 = free */
  uint32_t used; /* LRU stamp */
  unsigned got;  /* distinct bytes held */
  uint8_t seen[(RA_COVER_BYTES + 7) / 8];
  uint8_t data[RA_COVER_BYTES];
} slot_t;

static slot_t slots[RA_COVER_SLOTS];
static uint32_t stamp;
static uint16_t pix[RA_COVER_W * RA_COVER_H]; /* the expansion on show */
static uint32_t pix_cid;

void ra_cover_reset(void) {
  memset(slots, 0, sizeof slots);
  stamp = 0;
  pix_cid = 0;
}

static slot_t *find(uint32_t cid) {
  for (unsigned i = 0; i < RA_COVER_SLOTS; i++)
    if (slots[i].cid == cid) return &slots[i];
  return NULL;
}

int ra_cover_chunk(uint32_t cid, unsigned off, const uint8_t *d, size_t n) {
  if (!cid) return 0;
  if (off > RA_COVER_BYTES || n > RA_COVER_BYTES - off) return -1;
  slot_t *s = find(cid);
  if (!s) {
    s = &slots[0];
    for (unsigned i = 0; i < RA_COVER_SLOTS; i++) {
      if (!slots[i].cid) { s = &slots[i]; break; }
      if (slots[i].used < s->used) s = &slots[i];
    }
    if (s->cid == pix_cid) pix_cid = 0; /* the evicted cover's expansion is stale */
    memset(s, 0, sizeof *s);
    s->cid = cid;
  }
  s->used = ++stamp;
  bool was = s->got == RA_COVER_BYTES;
  for (size_t i = 0; i < n; i++) {
    unsigned b = off + (unsigned)i;
    if (!(s->seen[b >> 3] & (1u << (b & 7)))) {
      s->seen[b >> 3] |= (uint8_t)(1u << (b & 7));
      s->got++;
    }
    s->data[b] = d[i];
  }
  if (pix_cid == cid) pix_cid = 0; /* re-sent: expand again */
  return !was && s->got == RA_COVER_BYTES;
}

bool ra_cover_have(uint32_t cid) {
  slot_t *s = cid ? find(cid) : NULL;
  return s && s->got == RA_COVER_BYTES;
}

const uint16_t *ra_cover_expand(uint32_t cid) {
  slot_t *s = cid ? find(cid) : NULL;
  if (!s || s->got != RA_COVER_BYTES) return NULL;
  s->used = ++stamp;
  if (pix_cid == cid) return pix;
  uint16_t pal[16];
  for (unsigned i = 0; i < 16; i++) pal[i] = (uint16_t)(s->data[1728 + 2 * i] | (s->data[1729 + 2 * i] << 8));
  for (unsigned i = 0; i < RA_COVER_W * RA_COVER_H; i++) {
    uint8_t b = s->data[i >> 1];
    pix[i] = pal[(i & 1) ? (b & 15) : (b >> 4)];
  }
  pix_cid = cid;
  return pix;
}
