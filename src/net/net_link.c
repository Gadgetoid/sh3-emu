#include "net/net_link.h"

void net_link_pump(net_link_t *link, machine_t *machine) {
    if (!link->gateway) return;
    uint8_t buffer[4096];
    size_t count;
    bool dtr = machine_serial_dtr(machine);
    if (dtr && !link->dtr) net_gateway_reset(link->gateway);
    link->dtr = dtr;
    while ((count = machine_serial_take(machine, buffer, sizeof buffer)) > 0) net_gateway_from_guest(link->gateway, buffer, count);
    net_gateway_poll(link->gateway);
    size_t space = machine_serial_space(machine);
    count = net_gateway_to_guest(link->gateway, buffer, space < sizeof buffer ? space : sizeof buffer);
    machine_serial_send(machine, buffer, count);
}
