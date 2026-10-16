#pragma once
#include <stdbool.h>
#include <stddef.h>

int  rapi_socket_unix(const char *path);
int  rapi_socket_tcp(const char *host, const char *port, bool *found);
void rapi_socket_timeout(int socket, int seconds);
bool rapi_socket_read(int socket, void *data, size_t length);
bool rapi_socket_write(int socket, const void *data, size_t length);
void rapi_socket_close(int socket);
