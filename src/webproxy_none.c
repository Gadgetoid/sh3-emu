#include "webproxy.h"

#include <stddef.h>

webproxy_t *webproxy_start(netgw_log_fn log, const char *user_agent) { (void)log; (void)user_agent; return NULL; }
void        webproxy_stop(webproxy_t *proxy) { (void)proxy; }
void        webproxy_poll(webproxy_t *proxy) { (void)proxy; }
const char *webproxy_socket_path(const webproxy_t *proxy) { (void)proxy; return NULL; }
