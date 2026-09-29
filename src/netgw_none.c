#include "netgw.h"

#include <stddef.h>

netgw_t *netgw_create(netgw_log_fn log, const netgw_options_t *options) { (void)log; (void)options; return NULL; }
void     netgw_destroy(netgw_t *gateway) { (void)gateway; }
void     netgw_reset(netgw_t *gateway) { (void)gateway; }
void     netgw_from_guest(netgw_t *gateway, const uint8_t *data, size_t length) { (void)gateway; (void)data; (void)length; }
size_t   netgw_to_guest(netgw_t *gateway, uint8_t *out, size_t max) { (void)gateway; (void)out; (void)max; return 0; }
void     netgw_poll(netgw_t *gateway) { (void)gateway; }
bool     netgw_online(const netgw_t *gateway) { (void)gateway; return false; }
bool     netgw_take_desktop_connected(netgw_t *gateway) { (void)gateway; return false; }
