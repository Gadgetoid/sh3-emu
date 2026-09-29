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

static bool fetch(webproxy_t *proxy, const char *method, const char *url, bool print) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", webproxy_socket_path(proxy));
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(client, (struct sockaddr *)&address, sizeof address) != 0) {
        perror("proxycheck: connect");
        return false;
    }
    char request[4096];
    int length = snprintf(request, sizeof request, "%s %s HTTP/1.0\r\nUser-Agent: proxycheck\r\n\r\n", method, url);
    if (write(client, request, (size_t)length) != length) return false;
    for (;;) {
        webproxy_poll(proxy);
        struct pollfd entry = { client, POLLIN, 0 };
        if (poll(&entry, 1, 10) <= 0) continue;
        char buffer[4096];
        ssize_t got = read(client, buffer, sizeof buffer);
        if (got <= 0) break;
        if (print) fwrite(buffer, 1, (size_t)got, stdout);
    }
    close(client);
    return true;
}

int main(int argc, char **argv) {
    const char *method = "GET";
    int first = 1;
    if (argc > 1 && !strncmp(argv[1], "--method=", 9)) {
        method = argv[1] + 9;
        first = 2;
    }
    if (argc <= first) {
        fprintf(stderr, "usage: proxycheck [--method=METHOD] URL... (prints the last response)\n");
        return 2;
    }
    webproxy_t *proxy = webproxy_start(log_stderr, NETGW_DEFAULT_USER_AGENT);
    if (!proxy) {
        fprintf(stderr, "proxycheck: built without the web proxy\n");
        return 1;
    }
    bool success = true;
    for (int i = first; i < argc && success; i++) success = fetch(proxy, method, argv[i], i == argc - 1);
    webproxy_stop(proxy);
    return success ? 0 : 1;
}
