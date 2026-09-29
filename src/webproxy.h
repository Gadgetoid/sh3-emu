#pragma once
#include "netgw.h"

typedef struct webproxy webproxy_t;

webproxy_t *webproxy_start(netgw_log_fn log, const char *user_agent);
void        webproxy_stop(webproxy_t *proxy);
void        webproxy_poll(webproxy_t *proxy);
const char *webproxy_socket_path(const webproxy_t *proxy);
