#include "webproxy.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void log_stderr(const char *message) {
    fputs(message, stderr);
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: proxycheck URL [METHOD]\n");
        return 2;
    }
    const char *method = argc == 3 ? argv[2] : "GET";
    webproxy_t *proxy = webproxy_start(log_stderr, NETGW_DEFAULT_USER_AGENT);
    if (!proxy) {
        fprintf(stderr, "proxycheck: built without the web proxy\n");
        return 1;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", webproxy_socket_path(proxy));
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(client, (struct sockaddr *)&address, sizeof address) != 0) {
        perror("proxycheck: connect");
        return 1;
    }
    char request[4096];
    int length = snprintf(request, sizeof request, "%s %s HTTP/1.0\r\nUser-Agent: proxycheck\r\n\r\n", method, argv[1]);
    if (write(client, request, (size_t)length) != length) return 1;
    for (;;) {
        webproxy_poll(proxy);
        struct pollfd entry = { client, POLLIN, 0 };
        if (poll(&entry, 1, 10) <= 0) continue;
        char buffer[4096];
        ssize_t got = read(client, buffer, sizeof buffer);
        if (got <= 0) break;
        fwrite(buffer, 1, (size_t)got, stdout);
    }
    close(client);
    webproxy_stop(proxy);
    return 0;
}
