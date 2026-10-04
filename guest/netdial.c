#include <windows.h>
#include <ras.h>

#define ENTRY_NAME L"Odo Network"
#define DEVICE_TYPE L"direct"
#define DEVICE_NAME L"Serial Cable on COM1:"
#define STATUS_POLL_MS 2000
#define REDIAL_MS 5000

static void report(const WCHAR *format, DWORD value) {
    WCHAR line[128];
    wsprintfW(line, format, value);
    OutputDebugStringW(line);
}

static void make_entry(void) {
    RASENTRYW entry;
    memset(&entry, 0, sizeof entry);
    entry.dwSize = sizeof entry;
    entry.dwfOptions = RASEO_RemoteDefaultGateway;
    entry.dwfNetProtocols = RASNP_Ip;
    entry.dwFramingProtocol = RASFP_Ppp;
    lstrcpyW(entry.szDeviceType, DEVICE_TYPE);
    lstrcpyW(entry.szDeviceName, DEVICE_NAME);
    DWORD result = RasSetEntryProperties(NULL, ENTRY_NAME, &entry, sizeof entry, NULL, 0);
    if (result) report(L"netdial: cannot make the entry (%u)\r\n", result);
}

static void stay_connected(HRASCONN connection) {
    for (;;) {
        RASCONNSTATUSW status;
        memset(&status, 0, sizeof status);
        status.dwSize = sizeof status;
        if (RasGetConnectStatus(connection, &status) || status.rasconnstate != RASCS_Connected || status.dwError) return;
        Sleep(STATUS_POLL_MS);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    make_entry();
    for (;;) {
        RASDIALPARAMSW parameters;
        memset(&parameters, 0, sizeof parameters);
        parameters.dwSize = sizeof parameters;
        lstrcpyW(parameters.szEntryName, ENTRY_NAME);
        HRASCONN connection = NULL;
        DWORD result = RasDial(NULL, NULL, &parameters, 0, NULL, &connection);
        if (!result) {
            OutputDebugStringW(L"netdial: connected\r\n");
            stay_connected(connection);
            OutputDebugStringW(L"netdial: disconnected\r\n");
        }
        if (connection) RasHangUp(connection);
        Sleep(REDIAL_MS);
    }
}
