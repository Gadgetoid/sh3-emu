#include <windows.h>

#define VDISK_PHYSICAL    0x10800000u
#define VDISK_WINDOW      0x8000u
#define VDISK_BUFFER      0x1000u
#define VDISK_SECTOR      512u
#define VDISK_MAX_SECTORS 32u
#define VDISK_MAGIC       0x4B534456u

enum {
    REG_MAGIC = 0x00 / 4,
    REG_SECTORS = 0x08 / 4,
    REG_FLAGS = 0x0C / 4,
    REG_LBA = 0x10 / 4,
    REG_COUNT = 0x14 / 4,
    REG_COMMAND = 0x18 / 4,
    REG_STATUS = 0x1C / 4,
};

enum { COMMAND_READ = 1, COMMAND_WRITE = 2 };
enum { STATUS_OK, STATUS_RANGE, STATUS_IO, STATUS_NO_MEDIA, STATUS_READ_ONLY };

#define MEM_RELEASE   0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_NOCACHE  0x200
#define PAGE_PHYSICAL 0x400

#define DISK_IOCTL_GETINFO      1
#define DISK_IOCTL_READ         2
#define DISK_IOCTL_WRITE        3
#define DISK_IOCTL_INITIALIZED  4
#define DISK_IOCTL_SETINFO      5
#define DISK_IOCTL_FORMAT_MEDIA 6

#define DISK_INFO_FLAG_MBR           1
#define DISK_INFO_FLAG_CHS_UNCERTAIN 2

#define SH_WMGR 17

#define ERROR_INVALID_PARAMETER 87
#define ERROR_WRITE_PROTECT     19
#define ERROR_SECTOR_NOT_FOUND  27
#define ERROR_NOT_READY         21
#define ERROR_GEN_FAILURE       31

typedef struct {
    DWORD di_total_sectors;
    DWORD di_bytes_per_sect;
    DWORD di_cylinders;
    DWORD di_heads;
    DWORD di_sectors;
    DWORD di_flags;
} DISK_INFO;

typedef struct {
    BYTE *sb_buf;
    DWORD sb_len;
} SG_BUF;

typedef struct {
    DWORD  sr_start;
    DWORD  sr_num_sec;
    DWORD  sr_num_sg;
    DWORD  sr_status;
    void  *sr_callback;
    SG_BUF sr_sglist[1];
} SG_REQ;

typedef struct {
    HANDLE p_hDevice;
    HKEY   p_hKey;
} POST_INIT_BUF;

BOOL WINAPI VirtualCopy(void *destination, void *source, DWORD size, DWORD protection);
BOOL WINAPI VirtualFree(void *address, DWORD size, DWORD type);
BOOL WINAPI LoadFSD(HANDLE device, LPCWSTR fsd);
void *WINAPI MapPtrToProcess(void *pointer, HANDLE process);
HANDLE WINAPI GetCallerProcess(void);
void WINAPI SetLastError(DWORD error);
BOOL WINAPI IsAPIReady(DWORD api);

typedef struct {
    SG_REQ *request;
    DWORD   index, offset;
} cursor_t;

static volatile DWORD *registers;
static volatile BYTE *buffer;
static CRITICAL_SECTION lock;
static DISK_INFO info;
static HANDLE device;
static WCHAR file_system[32];

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved) {
    return TRUE;
}

static void describe(void) {
    DWORD sectors = registers[REG_SECTORS];
    info.di_total_sectors = sectors;
    info.di_bytes_per_sect = VDISK_SECTOR;
    info.di_heads = 16;
    info.di_sectors = 63;
    info.di_cylinders = sectors / (16 * 63);
    info.di_flags = DISK_INFO_FLAG_MBR | DISK_INFO_FLAG_CHS_UNCERTAIN;
}

DWORD DSK_Init(DWORD context) {
    if (!registers) {
        void *window = VirtualAlloc(0, VDISK_WINDOW, MEM_RESERVE, PAGE_NOACCESS);
        if (!window) return 0;
        if (!VirtualCopy(window, (void *)(VDISK_PHYSICAL >> 8), VDISK_WINDOW, PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL)) {
            VirtualFree(window, 0, MEM_RELEASE);
            return 0;
        }
        registers = window;
        buffer = (volatile BYTE *)window + VDISK_BUFFER;
        InitializeCriticalSection(&lock);
    }
    if (registers[REG_MAGIC] != VDISK_MAGIC) return 0;
    describe();
    return 1;
}

BOOL DSK_Deinit(DWORD handle) { return TRUE; }
DWORD DSK_Open(DWORD handle, DWORD access, DWORD share) { return handle; }
BOOL DSK_Close(DWORD handle) { return TRUE; }
DWORD DSK_Read(DWORD handle, void *data, DWORD size) { return 0; }
DWORD DSK_Write(DWORD handle, const void *data, DWORD size) { return 0; }
DWORD DSK_Seek(DWORD handle, long distance, DWORD method) { return 0; }
void DSK_PowerUp(DWORD handle) {}
void DSK_PowerDown(DWORD handle) {}

static BYTE *cursor_span(cursor_t *cursor, DWORD *available) {
    while (cursor->index < cursor->request->sr_num_sg) {
        SG_BUF *piece = &cursor->request->sr_sglist[cursor->index];
        if (cursor->offset < piece->sb_len) {
            *available = piece->sb_len - cursor->offset;
            return (BYTE *)MapPtrToProcess(piece->sb_buf, GetCallerProcess()) + cursor->offset;
        }
        cursor->index++;
        cursor->offset = 0;
    }
    return NULL;
}

static BOOL move_bytes(cursor_t *cursor, DWORD bytes, BOOL to_device) {
    DWORD done = 0;
    while (done < bytes) {
        DWORD available;
        BYTE *span = cursor_span(cursor, &available);
        if (!span) return FALSE;
        if (available > bytes - done) available = bytes - done;
        for (DWORD i = 0; i < available; i++) {
            if (to_device) buffer[done + i] = span[i];
            else span[i] = buffer[done + i];
        }
        cursor->offset += available;
        done += available;
    }
    return TRUE;
}

static DWORD device_error(DWORD status) {
    switch (status) {
        case STATUS_RANGE: return ERROR_SECTOR_NOT_FOUND;
        case STATUS_NO_MEDIA: return ERROR_NOT_READY;
        case STATUS_READ_ONLY: return ERROR_WRITE_PROTECT;
    }
    return ERROR_GEN_FAILURE;
}

static DWORD transfer(SG_REQ *request, BOOL write) {
    cursor_t cursor = { request, 0, 0 };
    DWORD lba = request->sr_start, remaining = request->sr_num_sec;
    if (remaining > registers[REG_SECTORS] || lba > registers[REG_SECTORS] - remaining) return ERROR_SECTOR_NOT_FOUND;
    while (remaining) {
        DWORD count = remaining < VDISK_MAX_SECTORS ? remaining : VDISK_MAX_SECTORS;
        if (write && !move_bytes(&cursor, count * VDISK_SECTOR, TRUE)) return ERROR_INVALID_PARAMETER;
        registers[REG_LBA] = lba;
        registers[REG_COUNT] = count;
        registers[REG_COMMAND] = write ? COMMAND_WRITE : COMMAND_READ;
        DWORD status = registers[REG_STATUS];
        if (status != STATUS_OK) return device_error(status);
        if (!write && !move_bytes(&cursor, count * VDISK_SECTOR, FALSE)) return ERROR_INVALID_PARAMETER;
        lba += count;
        remaining -= count;
    }
    return ERROR_SUCCESS;
}

static DWORD WINAPI mount_when_ready(void *parameter) {
    while (!IsAPIReady(SH_WMGR)) Sleep(250);
    if (registers[REG_SECTORS]) LoadFSD(device, file_system);
    return 0;
}

static BOOL load_file_system(POST_INIT_BUF *post) {
    DWORD type, size = sizeof file_system;
    if (RegQueryValueExW(post->p_hKey, L"FSD", NULL, &type, (BYTE *)file_system, &size) != ERROR_SUCCESS || type != REG_SZ) return TRUE;
    device = post->p_hDevice;
    HANDLE thread = CreateThread(NULL, 0, mount_when_ready, NULL, 0, NULL);
    if (!thread) return FALSE;
    CloseHandle(thread);
    return TRUE;
}

BOOL DSK_IOControl(DWORD handle, DWORD code, BYTE *in, DWORD in_size, BYTE *out, DWORD out_size, DWORD *returned) {
    if (!in && code != DISK_IOCTL_FORMAT_MEDIA) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    switch (code) {
        case DISK_IOCTL_GETINFO:
            describe();
            *(DISK_INFO *)in = info;
            return TRUE;
        case DISK_IOCTL_SETINFO:
            return TRUE;
        case DISK_IOCTL_READ:
        case DISK_IOCTL_WRITE: {
            SG_REQ *request = (SG_REQ *)in;
            EnterCriticalSection(&lock);
            DWORD error = transfer(request, code == DISK_IOCTL_WRITE);
            LeaveCriticalSection(&lock);
            request->sr_status = error;
            if (error != ERROR_SUCCESS) {
                SetLastError(error);
                return FALSE;
            }
            if (returned) *returned = request->sr_num_sec * VDISK_SECTOR;
            return TRUE;
        }
        case DISK_IOCTL_INITIALIZED:
            return load_file_system((POST_INIT_BUF *)in);
        case DISK_IOCTL_FORMAT_MEDIA:
            return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}
