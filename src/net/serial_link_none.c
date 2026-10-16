/* Bridge guest serial traffic to the network only, where host serial devices and sockets are not supported. */
#include "net/serial_link.h"

#include <string.h>

void serial_link_init(serial_link_t *link, net_gateway_log_fn log) {
    memset(link, 0, sizeof *link);
    link->log = log;
    link->options = (net_gateway_options_t){ NET_GATEWAY_DEFAULT_USER_AGENT, NULL, 0 };
    link->fd = -1;
    link->pty_slave = -1;
    link->listener = -1;
}

void serial_link_close(serial_link_t *link) {
    if (link->gateway) ppp_gateway_destroy(link->gateway);
    link->gateway = NULL;
    link->queued = 0;
    link->baud = 0;
    link->dtr = false;
    link->mode = SERIAL_OFF;
}

const char *serial_link_open(serial_link_t *link, serial_mode_t mode, const char *device) {
    (void)device;
    serial_link_close(link);
    if (mode == SERIAL_OFF) return NULL;
    if (mode != SERIAL_NETWORK) return "this build has no host serial ports or sockets";
    link->gateway = ppp_gateway_create(link->log, &link->options);
    if (!link->gateway) return net_gateway_available() ? "cannot start the network" : "this build has no network (libslirp)";
    link->mode = mode;
    return NULL;
}

void serial_link_pump(serial_link_t *link, machine_t *machine) {
    if (link->mode != SERIAL_NETWORK || !link->gateway) return;
    uint8_t buffer[4096];
    size_t count;
    bool dtr = machine_serial_dtr(machine);
    if (dtr && !link->dtr) ppp_gateway_reset(link->gateway);
    link->dtr = dtr;
    while ((count = machine_serial_take(machine, buffer, sizeof buffer)) > 0) ppp_gateway_from_guest(link->gateway, buffer, count);
    ppp_gateway_poll(link->gateway, machine_cycles(machine) / (MACHINE_CLOCK_HZ / 1000));
    size_t space = machine_serial_space(machine);
    count = ppp_gateway_to_guest(link->gateway, buffer, space < sizeof buffer ? space : sizeof buffer);
    machine_serial_send(machine, buffer, count);
}

bool serial_link_attached(const serial_link_t *link) {
    return link->mode != SERIAL_OFF;
}

int serial_link_ports(char ports[][SERIAL_LINK_PORT_NAME], int max) {
    (void)ports; (void)max;
    return 0;
}
