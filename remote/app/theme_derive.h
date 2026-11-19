#ifndef RA_THEME_DERIVE_H
#define RA_THEME_DERIVE_H

/* Integer-only twin of harmony_core/theme_math.py (pinned by tests/vectors/themes.json).
 *   luminance  Y = 2126*L[r] + 7152*L[g] + 722*L[b]  (ra_lin: 12-bit sRGB->linear, full scale 40950000)
 *   contrast   (Yhi + K) / (Ylo + K), K = 2047500; compared by cross-multiplication in 64 bits
 *   snap565    R,B: min(31,(v+4)>>3)<<3 ; G: min(63,(v+2)>>2)<<2
 *   ink        tx or bg, whichever contrasts more with the fill (tie: tx)
 *   pressed    per channel (3*fill + ink + 2)>>2, then snap565 */
#include "remote/app/theme_presets.h"

#include <stdint.h>

#define RA_WIRE_VERSION 1
#define RA_WIRE_LEN 99 /* version, 13 tokens, 12 x {fill, ink, pressed}; RGB565 big-endian */

uint32_t ra_snap565(uint32_t rgb);
uint32_t ra_contrast_x100(uint32_t a, uint32_t b); /* floored */
int ra_meets(uint32_t a, uint32_t b, uint32_t num, uint32_t den); /* contrast(a,b) >= num/den */
uint32_t ra_ink(uint32_t fill, uint32_t tx, uint32_t bg);
uint32_t ra_pressed(uint32_t fill, uint32_t ink);
int ra_needs_outline(uint32_t fill, uint32_t bg); /* contrast < 1.5:1: draw a 1 px TX3 outline */
uint16_t ra_to565(uint32_t rgb);
uint32_t ra_from565(uint16_t v);

/* Decode a wire theme into *out (id RA_THEME_CUSTOM_ID). 0 ok, -1 bad length, -2 bad version. */
int ra_theme_decode(const uint8_t *b, unsigned len, ra_theme_t *out);

#endif
