/* Remote firmware entry point (DM365). Stub: links the shared RF core so the
 * ARM cross build produces a real static binary; UI/RF loop lands later. */
#include "hub/rf/framing.h"

int main(void) {
    hp_rf_asm_t a;
    hp_rf_asm_init(&a);
    return 0;
}
