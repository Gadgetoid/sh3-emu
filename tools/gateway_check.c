#include <stdio.h>
#include <string.h>

#include "net/ppp_gateway.h"

static int expect_greeting(ppp_gateway_t *gateway, const char *name, const char *prefix, size_t prefix_length) {
    ppp_gateway_reset(gateway);
    ppp_gateway_from_guest(gateway, (const uint8_t *)prefix, prefix_length);
    ppp_gateway_from_guest(gateway, (const uint8_t *)"CLIENT", 6);
    uint8_t reply[64];
    size_t length = ppp_gateway_to_guest(gateway, reply, sizeof reply);
    bool ok = length == 12 && !memcmp(reply, "CLIENTSERVER", 12);
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    return ok ? 0 : 1;
}

int main(void) {
    if (!net_gateway_available()) {
        printf("skip gateway: built without libslirp\n");
        return 0;
    }
    ppp_gateway_t *gateway = ppp_gateway_create(NULL, NULL);
    if (!gateway) return 1;
    char noise[4096];
    for (size_t i = 0; i < sizeof noise; i++) noise[i] = "Serial debug text, CLIEN\r\n"[i % 26];
    int failures = expect_greeting(gateway, "gateway_greeting", "", 0);
    failures += expect_greeting(gateway, "gateway_greeting_after_text", noise, sizeof noise);
    failures += expect_greeting(gateway, "gateway_greeting_split", "CLI", 3);
    ppp_gateway_destroy(gateway);
    return failures ? 1 : 0;
}
