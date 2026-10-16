/* Carry RAPI messages over Unix and TCP stream sockets. */
#include "rapi/rapi_socket.h"

#include <netdb.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static int prepare(int fd) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    return fd;
}

int rapi_socket_unix(const char *path) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return prepare(fd);
}

int rapi_socket_tcp(const char *host, const char *port, bool *found) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *addresses = NULL;
    *found = getaddrinfo(host, port, &hints, &addresses) == 0 && addresses;
    if (!*found) return -1;
    int fd = -1;
    for (struct addrinfo *at = addresses; at && fd < 0; at = at->ai_next) {
        fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (fd >= 0 && connect(fd, at->ai_addr, at->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(addresses);
    return fd < 0 ? -1 : prepare(fd);
}

void rapi_socket_timeout(int socket, int seconds) {
    struct timeval timeout = { seconds, 0 };
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
}

bool rapi_socket_read(int socket, void *data, size_t length) {
    uint8_t *p = data;
    while (length) {
        ssize_t got = recv(socket, p, length, 0);
        if (got <= 0) return false;
        p += got;
        length -= (size_t)got;
    }
    return true;
}

bool rapi_socket_write(int socket, const void *data, size_t length) {
    const uint8_t *p = data;
    while (length) {
        ssize_t sent = send(socket, p, length, SEND_FLAGS);
        if (sent <= 0) return false;
        p += sent;
        length -= (size_t)sent;
    }
    return true;
}

void rapi_socket_close(int socket) {
    close(socket);
}
