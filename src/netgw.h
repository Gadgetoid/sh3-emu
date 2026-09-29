#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETGW_DEFAULT_USER_AGENT "Lynx/2.9.2 libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/3.0.13"

typedef struct netgw netgw_t;

typedef void (*netgw_log_fn)(const char *message);

netgw_t *netgw_create(netgw_log_fn log, const char *user_agent);
void     netgw_destroy(netgw_t *gateway);
void     netgw_reset(netgw_t *gateway);
void     netgw_from_guest(netgw_t *gateway, const uint8_t *data, size_t length);
size_t   netgw_to_guest(netgw_t *gateway, uint8_t *out, size_t max);
void     netgw_poll(netgw_t *gateway);
bool     netgw_online(const netgw_t *gateway);
