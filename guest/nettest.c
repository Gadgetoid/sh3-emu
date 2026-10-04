#include <windows.h>
#include <winsock.h>

#define HOST_NAME "host"
#define RESOLVE_TRIES 60
#define RESPONSE_MAX 2048
#define TOKEN_MAX 64

typedef int (PASCAL *startup_fn)(WORD, LPWSADATA);

static void report(const char *text) {
    WCHAR line[256];
    int length = 0;
    while (text[length] && length < 255) {
        line[length] = (WCHAR)(unsigned char)text[length];
        length++;
    }
    line[length] = 0;
    OutputDebugStringW(line);
}

static int append(char *buffer, int length, const char *text) {
    while (*text) buffer[length++] = *text++;
    buffer[length] = 0;
    return length;
}

static int parse_port(const WCHAR *text) {
    int port = 0;
    while (*text == ' ') text++;
    while (*text >= '0' && *text <= '9') port = port * 10 + (*text++ - '0');
    return port;
}

static int fetch(unsigned long address, int port, const char *path, char *response, int size) {
    SOCKET connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection == INVALID_SOCKET) return -1;
    struct sockaddr_in target;
    memset(&target, 0, sizeof target);
    target.sin_family = AF_INET;
    target.sin_port = htons((unsigned short)port);
    target.sin_addr.s_addr = address;
    if (connect(connection, (struct sockaddr *)&target, sizeof target)) {
        closesocket(connection);
        return -1;
    }
    char request[256];
    int length = append(request, 0, "GET ");
    length = append(request, length, path);
    length = append(request, length, " HTTP/1.0\r\nHost: " HOST_NAME "\r\n\r\n");
    send(connection, request, length, 0);
    int received = 0, got;
    while (received < size - 1 && (got = recv(connection, response + received, size - 1 - received, 0)) > 0) received += got;
    response[received] = 0;
    closesocket(connection);
    return received;
}

static const char *body_of(const char *response) {
    for (const char *p = response; *p; p++)
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') return p + 4;
    return NULL;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    int port = parse_port(command_line);
    startup_fn startup = (startup_fn)GetProcAddressW(LoadLibraryW(L"winsock.dll"), L"WSAStartup");
    WSADATA data;
    if (!port) {
        report("nettest: usage nettest PORT\r\n");
        return 1;
    }
    if (!startup || startup(MAKEWORD(1, 1), &data)) {
        report("nettest: cannot start winsock\r\n");
        return 1;
    }
    struct hostent *host = NULL;
    for (int attempt = 0; attempt < RESOLVE_TRIES && !host; attempt++) {
        host = gethostbyname(HOST_NAME);
        if (!host) Sleep(1000);
    }
    if (!host) {
        report("nettest: cannot resolve " HOST_NAME "\r\n");
        return 1;
    }
    unsigned long address;
    memcpy(&address, host->h_addr_list[0], sizeof address);
    char path[128], response[RESPONSE_MAX];
    int length = append(path, 0, "/resolved/");
    append(path, length, inet_ntoa(*(struct in_addr *)&address));
    report("nettest: ");
    report(path);
    report("\r\n");
    if (fetch(address, port, path, response, sizeof response) <= 0) {
        report("nettest: no response\r\n");
        return 1;
    }
    const char *body = body_of(response);
    char token[TOKEN_MAX];
    int token_length = 0;
    while (body && body[token_length] > ' ' && token_length < TOKEN_MAX - 1) {
        token[token_length] = body[token_length];
        token_length++;
    }
    token[token_length] = 0;
    length = append(path, 0, "/received/");
    append(path, length, token);
    report("nettest: ");
    report(path);
    report("\r\n");
    fetch(address, port, path, response, sizeof response);
    return 0;
}
