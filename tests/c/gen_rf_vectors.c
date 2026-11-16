/* Writes golden RF framing vectors (from the C implementation) as JSON. */
#include <stdio.h>
#include "hub/rf/framing.h"

static void hex(FILE *o, const uint8_t *p, size_t n)
{
    fputc('"', o);
    for (size_t i = 0; i < n; i++) fprintf(o, "%02x", p[i]);
    fputc('"', o);
}

int main(int argc, char **argv)
{
    static const struct { size_t cap, len; uint8_t type, seq; } cases[] = {
        {30, 0, HP_MSG_BATTERY, 1},   {30, 1, HP_MSG_BATTERY, 2},   {30, 26, HP_MSG_KEY_EVENT, 3},
        {30, 27, HP_MSG_KEY_EVENT, 4}, {30, 80, HP_MSG_PAGE_PUSH, 5}, {30, 390, HP_MSG_PAGE_PUSH, 250},
        {32, 0, HP_MSG_BATTERY, 6},   {32, 28, HP_MSG_KEY_EVENT, 7}, {32, 29, HP_MSG_KEY_EVENT, 8},
        {32, 100, HP_MSG_STATE_UPDATE, 9}, {32, 420, HP_MSG_PAGE_PUSH, 255},
    };
    FILE *o = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!o) return 1;
    fprintf(o, "[\n");
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        uint8_t p[HP_RF_MSG_MAX];
        hp_rf_frame_t fr[16];
        size_t n;
        for (size_t i = 0; i < cases[c].len; i++) p[i] = (uint8_t)(i * 7 + 3);
        if (hp_rf_split((hp_msg_type_t)cases[c].type, cases[c].seq, p, cases[c].len,
                        cases[c].cap, fr, 16, &n) != 0)
            return 1;
        fprintf(o, "  {\"type\": %u, \"seq\": %u, \"frame_cap\": %zu, \"payload\": ",
                cases[c].type, cases[c].seq, cases[c].cap);
        hex(o, p, cases[c].len);
        fprintf(o, ", \"frames\": [");
        for (size_t i = 0; i < n; i++) {
            if (i) fprintf(o, ", ");
            hex(o, fr[i].data, fr[i].len);
        }
        fprintf(o, "]}%s\n", c + 1 < sizeof cases / sizeof *cases ? "," : "");
    }
    fprintf(o, "]\n");
    if (argc > 1) fclose(o);
    return 0;
}
