/* Insert and eject the emulated Ethernet card for the app, and keep its host network open while it's in. */
#include "app/ethernet_service.h"

#include "net/net_gateway.h"

void ethernet_service_init(ethernet_service_t *service, const settings_t *settings) {
    ethernet_link_init(&service->link, NULL, settings->user_agent);
}

bool ethernet_service_available(machine_t *machine) {
    return net_gateway_available() && machine_has_ethernet_slot(machine);
}

const char *ethernet_service_toggle(ethernet_service_t *service, machine_t *machine) {
    if (machine_ethernet_inserted(machine)) {
        machine_eject_ethernet(machine);
        ethernet_link_close(&service->link);
        return "Ethernet card ejected";
    }
    if (ethernet_link_insert(&service->link, machine)) return "Ethernet card inserted; CE uses it after a soft reset once its driver is installed";
    return "could not insert the Ethernet card";
}

void ethernet_service_close(ethernet_service_t *service) {
    ethernet_link_close(&service->link);
}
