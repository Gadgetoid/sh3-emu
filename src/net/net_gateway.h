#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct net_gateway net_gateway_t;

typedef void (*net_gateway_log_fn)(const char *message);

bool           net_gateway_available(void);
net_gateway_t *net_gateway_create(net_gateway_log_fn log);
void           net_gateway_destroy(net_gateway_t *gateway);
void           net_gateway_reset(net_gateway_t *gateway);
void           net_gateway_from_guest(net_gateway_t *gateway, const uint8_t *data, size_t length);
size_t         net_gateway_to_guest(net_gateway_t *gateway, uint8_t *out, size_t max);
void           net_gateway_poll(net_gateway_t *gateway);
bool           net_gateway_online(const net_gateway_t *gateway);
