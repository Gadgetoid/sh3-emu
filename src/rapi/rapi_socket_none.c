/* Provide unavailable-operation stubs when RAPI sockets are not supported. */
#include "rapi/rapi_socket.h"

int  rapi_socket_unix(const char *path) {
    (void)path; return -1;
}
int  rapi_socket_tcp(const char *host, const char *port, bool *found) {
    (void)host; (void)port; *found = false; return -1;
}
void rapi_socket_timeout(int socket, int seconds) {
    (void)socket; (void)seconds;
}
bool rapi_socket_read(int socket, void *data, size_t length) {
    (void)socket; (void)data; (void)length; return false;
}
bool rapi_socket_write(int socket, const void *data, size_t length) {
    (void)socket; (void)data; (void)length; return false;
}
void rapi_socket_close(int socket) {
    (void)socket;
}
