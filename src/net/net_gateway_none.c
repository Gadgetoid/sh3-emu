#include "net/net_gateway.h"

bool           net_gateway_available(void) { return false; }
net_gateway_t *net_gateway_create(net_gateway_log_fn log) { (void)log; return NULL; }
void           net_gateway_destroy(net_gateway_t *gateway) { (void)gateway; }
void           net_gateway_reset(net_gateway_t *gateway) { (void)gateway; }
void           net_gateway_from_guest(net_gateway_t *gateway, const uint8_t *data, size_t length) { (void)gateway; (void)data; (void)length; }
size_t         net_gateway_to_guest(net_gateway_t *gateway, uint8_t *out, size_t max) { (void)gateway; (void)out; (void)max; return 0; }
void           net_gateway_poll(net_gateway_t *gateway) { (void)gateway; }
bool           net_gateway_online(const net_gateway_t *gateway) { (void)gateway; return false; }
