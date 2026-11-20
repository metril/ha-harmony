#ifndef HH_BT_MAIN_H
#define HH_BT_MAIN_H

#include <stdint.h>

/* `hub bt-helper ...`: argv without the program name and the subcommand. */
int bt_helper_main(int argc, char **argv);

/* LE: queues the Service Changed indication for this connection (bt_sc.h decides whether one is due). */
void bt_sc_request(uint16_t con_handle);

#endif
