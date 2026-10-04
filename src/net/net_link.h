#pragma once
#include <stdbool.h>

#include "core/machine.h"
#include "net/net_gateway.h"

typedef struct {
    net_gateway_t *gateway;
    bool           dtr;
} net_link_t;

void net_link_pump(net_link_t *link, machine_t *machine);
