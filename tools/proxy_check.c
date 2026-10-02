#include "net/web_proxy.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void log_stderr(const char *message) {
    fputs(message, stderr);
}

static bool fetch(web_proxy_t *proxy, const char *method, const char *header, const char *url, bool print) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", web_proxy_socket_path(proxy));
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(client, (struct sockaddr *)&address, sizeof address) != 0) {
        perror("proxycheck: connect");
        return false;
    }
    char request[4096];
    int length = snprintf(request, sizeof request, "%s %s HTTP/1.0\r\nUser-Agent: proxycheck\r\n%s%s\r\n", method, url, header,
                          *header ? "\r\n" : "");
    if (write(client, request, (size_t)length) != length) return false;
    for (;;) {
        web_proxy_poll(proxy);
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
    const char *header = "";
    int first = 1;
    for (; first < argc && !strncmp(argv[first], "--", 2); first++) {
        if (!strncmp(argv[first], "--method=", 9)) method = argv[first] + 9;
        else if (!strncmp(argv[first], "--header=", 9)) header = argv[first] + 9;
        else break;
    }
    if (argc <= first) {
        fprintf(stderr, "usage: proxycheck [--method=METHOD] [--header=HEADER] URL... (prints the last response)\n");
        return 2;
    }
    web_proxy_t *proxy = web_proxy_start(log_stderr, NET_GATEWAY_DEFAULT_USER_AGENT);
    if (!proxy) {
        fprintf(stderr, "proxycheck: the web proxy did not start (or was built without libcurl)\n");
        return 1;
    }
    bool success = true;
    for (int i = first; i < argc && success; i++) success = fetch(proxy, method, header, argv[i], i == argc - 1);
    web_proxy_stop(proxy);
    return success ? 0 : 1;
}
