#include "rapi/rapi_sync.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_SIZE       1024
#define FREE_MARGIN     (64 * 1024)
#define MANIFEST_HEADER "velo-sync 1"

typedef struct {
    char     path[PATH_SIZE];
    bool     on_mac, on_velo, in_manifest, failed;
    int64_t  mac_time;
    uint64_t mac_size;
    uint64_t velo_time;
    uint32_t velo_size;
    int64_t  known_mac_time;
    uint64_t known_mac_size;
    uint64_t known_velo_time;
    uint32_t known_velo_size;
} entry_t;

typedef struct {
    entry_t *items;
    size_t   count, capacity;
} entries_t;

typedef struct {
    rapi_t            *rapi;
    const char        *folder;
    const char        *remote_root;
    rapi_sync_log_fn    log;
    void              *context;
    rapi_sync_result_t *result;
    uint64_t           free_space;
    bool               disconnected;
} sync_t;

static void sync_log(sync_t *sync, const char *format, ...) {
    if (!sync->log) return;
    char message[PATH_SIZE * 2];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    sync->log(sync->context, message);
}

static entry_t *find_or_add(entries_t *entries, const char *path) {
    for (size_t i = 0; i < entries->count; i++) {
        if (!strcasecmp(entries->items[i].path, path)) return &entries->items[i];
    }
    if (entries->count == entries->capacity) {
        size_t capacity = entries->capacity ? entries->capacity * 2 : 64;
        entry_t *grown = realloc(entries->items, capacity * sizeof *grown);
        if (!grown) return NULL;
        entries->items = grown;
        entries->capacity = capacity;
    }
    entry_t *entry = &entries->items[entries->count++];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->path, sizeof entry->path, "%s", path);
    return entry;
}

static void join_relative(char *out, size_t size, const char *relative, const char *name) {
    if (*relative) snprintf(out, size, "%s/%s", relative, name);
    else snprintf(out, size, "%s", name);
}

static void local_path(const sync_t *sync, const char *relative, char *out, size_t size) {
    snprintf(out, size, "%s/%s", sync->folder, relative);
}

static void remote_path(const sync_t *sync, const char *relative, char *out, size_t size) {
    if (*relative) snprintf(out, size, "%s\\%s", sync->remote_root, relative);
    else snprintf(out, size, "%s", sync->remote_root);
    for (char *p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
}

static bool valid_on_velo(const char *relative) {
    return strpbrk(relative, "\\:*?\"<>|") == NULL;
}

static void scan_mac(sync_t *sync, entries_t *entries, const char *relative) {
    char directory[PATH_SIZE * 2];
    local_path(sync, relative, directory, sizeof directory);
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *item;
    while ((item = readdir(dir))) {
        const char *name = item->d_name;
        size_t length = strlen(name);
        if (name[0] == '.' || (length > 5 && !strcmp(name + length - 5, ".part"))) continue;
        char child[PATH_SIZE], full[PATH_SIZE * 2];
        join_relative(child, sizeof child, relative, name);
        local_path(sync, child, full, sizeof full);
        struct stat info;
        if (stat(full, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            scan_mac(sync, entries, child);
        } else if (S_ISREG(info.st_mode)) {
            entry_t *entry = find_or_add(entries, child);
            if (!entry) continue;
            entry->on_mac = true;
            entry->mac_time = (int64_t)info.st_mtime;
            entry->mac_size = (uint64_t)info.st_size;
        }
    }
    closedir(dir);
}

static bool scan_velo(sync_t *sync, entries_t *entries, const char *relative, int depth) {
    if (depth > RAPI_FOLDER_DEPTH_MAX) {
        sync_log(sync, "can't list %s: folders nested too deeply", relative);
        return false;
    }
    char pattern[PATH_SIZE * 2];
    remote_path(sync, relative, pattern, sizeof pattern - 2);
    strcat(pattern, "\\*");
    rapi_file_t *files;
    size_t count;
    if (!rapi_list(sync->rapi, pattern, &files, &count)) {
        sync_log(sync, "can't list %s: %s", pattern, rapi_error(sync->rapi));
        return false;
    }
    bool success = true;
    for (size_t i = 0; i < count && success; i++) {
        char child[PATH_SIZE];
        join_relative(child, sizeof child, relative, files[i].name);
        if (files[i].attributes & RAPI_ATTRIBUTE_DIRECTORY) {
            success = scan_velo(sync, entries, child, depth + 1);
            continue;
        }
        entry_t *entry = find_or_add(entries, child);
        if (!entry) continue;
        entry->on_velo = true;
        entry->velo_time = files[i].write_time;
        entry->velo_size = files[i].size;
    }
    free(files);
    return success;
}

static void load_manifest(sync_t *sync, entries_t *entries, const char *manifest_path) {
    FILE *file = fopen(manifest_path, "r");
    if (!file) return;
    char line[PATH_SIZE * 2], header[PATH_SIZE * 2];
    snprintf(header, sizeof header, "%s\t%s\t%s\n", MANIFEST_HEADER, sync->folder, sync->remote_root);
    if (!fgets(line, sizeof line, file) || strcmp(line, header)) {
        fclose(file);
        return;
    }
    while (fgets(line, sizeof line, file)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        long long mac_time;
        unsigned long long mac_size, velo_time;
        unsigned velo_size;
        if (sscanf(tab + 1, "%lld\t%llu\t%llu\t%u", &mac_time, &mac_size, &velo_time, &velo_size) != 4) continue;
        entry_t *entry = find_or_add(entries, line);
        if (!entry) continue;
        entry->in_manifest = true;
        entry->known_mac_time = mac_time;
        entry->known_mac_size = mac_size;
        entry->known_velo_time = velo_time;
        entry->known_velo_size = velo_size;
    }
    fclose(file);
}

static void save_manifest(sync_t *sync, const entries_t *entries, const char *manifest_path) {
    char partial[PATH_SIZE * 2];
    snprintf(partial, sizeof partial, "%s.new", manifest_path);
    FILE *file = fopen(partial, "w");
    if (!file) return;
    fprintf(file, "%s\t%s\t%s\n", MANIFEST_HEADER, sync->folder, sync->remote_root);
    for (size_t i = 0; i < entries->count; i++) {
        const entry_t *entry = &entries->items[i];
        if (entry->failed && entry->in_manifest) {
            fprintf(file, "%s\t%lld\t%llu\t%llu\t%u\n", entry->path, (long long)entry->known_mac_time,
                    (unsigned long long)entry->known_mac_size, (unsigned long long)entry->known_velo_time, entry->known_velo_size);
        } else if (!entry->failed && entry->on_mac && entry->on_velo) {
            fprintf(file, "%s\t%lld\t%llu\t%llu\t%u\n", entry->path, (long long)entry->mac_time,
                    (unsigned long long)entry->mac_size, (unsigned long long)entry->velo_time, entry->velo_size);
        }
    }
    if (fclose(file) == 0) rename(partial, manifest_path);
}

static void make_local_directories(const char *path) {
    char directory[PATH_SIZE * 2];
    snprintf(directory, sizeof directory, "%s", path);
    for (char *p = directory + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(directory, 0755);
        *p = '/';
    }
}

static void make_remote_directories(sync_t *sync, const char *relative) {
    char partial[PATH_SIZE];
    snprintf(partial, sizeof partial, "%s", relative);
    for (char *p = partial; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        char remote[PATH_SIZE * 2];
        rapi_file_t info;
        remote_path(sync, partial, remote, sizeof remote);
        if (!rapi_stat(sync->rapi, remote, &info)) rapi_make_directory(sync->rapi, remote);
        *p = '/';
    }
}

static bool move_to_trash(const char *path) {
    const char *home = getenv("HOME");
    const char *leaf = strrchr(path, '/');
    if (!home || !leaf) return false;
    char trash[PATH_SIZE * 2];
    snprintf(trash, sizeof trash, "%s/.Trash", home);
    struct stat info;
    if (stat(trash, &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    for (int copy = 0; copy < 100; copy++) {
        char target[PATH_SIZE * 3];
        if (copy) snprintf(target, sizeof target, "%s/%s %d", trash, leaf + 1, copy);
        else snprintf(target, sizeof target, "%s/%s", trash, leaf + 1);
        if (access(target, F_OK) != 0) return rename(path, target) == 0;
    }
    return false;
}

static bool check_connection(sync_t *sync) {
    rapi_version_t version;
    if (!rapi_version(sync->rapi, &version)) sync->disconnected = true;
    return !sync->disconnected;
}

static bool upload(sync_t *sync, entry_t *entry) {
    char local[PATH_SIZE * 2], remote[PATH_SIZE * 2];
    local_path(sync, entry->path, local, sizeof local);
    remote_path(sync, entry->path, remote, sizeof remote);
    if (!valid_on_velo(entry->path)) {
        sync_log(sync, "skipped %s: name not allowed on the Velo", entry->path);
        sync->result->skipped++;
        return false;
    }
    if (entry->mac_size + FREE_MARGIN > sync->free_space) {
        sync_log(sync, "skipped %s: not enough storage on the Velo", entry->path);
        sync->result->skipped++;
        return false;
    }
    make_remote_directories(sync, entry->path);
    rapi_file_t info;
    if (!rapi_upload(sync->rapi, local, remote, NULL, NULL) || !rapi_stat(sync->rapi, remote, &info)) {
        sync_log(sync, "can't copy %s to the Velo: %s", entry->path, rapi_error(sync->rapi));
        check_connection(sync);
        return false;
    }
    entry->on_velo = true;
    entry->velo_time = info.write_time;
    entry->velo_size = info.size;
    sync->free_space -= entry->mac_size < sync->free_space ? entry->mac_size : sync->free_space;
    sync->result->uploaded++;
    sync_log(sync, "copied %s to the Velo", entry->path);
    return true;
}

static bool download_as(sync_t *sync, entry_t *entry, const char *relative) {
    char local[PATH_SIZE * 2], remote[PATH_SIZE * 2];
    local_path(sync, relative, local, sizeof local);
    remote_path(sync, entry->path, remote, sizeof remote);
    make_local_directories(local);
    if (!rapi_download(sync->rapi, remote, local, NULL, NULL)) {
        sync_log(sync, "can't copy %s from the Velo: %s", entry->path, rapi_error(sync->rapi));
        check_connection(sync);
        return false;
    }
    sync->result->downloaded++;
    sync_log(sync, "copied %s from the Velo", relative);
    return true;
}

static bool download(sync_t *sync, entry_t *entry) {
    if (!download_as(sync, entry, entry->path)) return false;
    char local[PATH_SIZE * 2];
    struct stat info;
    local_path(sync, entry->path, local, sizeof local);
    if (stat(local, &info) != 0) return false;
    entry->on_mac = true;
    entry->mac_time = (int64_t)info.st_mtime;
    entry->mac_size = (uint64_t)info.st_size;
    return true;
}

static bool resolve_conflict(sync_t *sync, entries_t *entries, size_t index) {
    char copy[PATH_SIZE];
    const char *path = entries->items[index].path;
    const char *leaf = strrchr(path, '/');
    const char *dot = strrchr(leaf ? leaf : path, '.');
    if (dot && dot != (leaf ? leaf + 1 : path)) snprintf(copy, sizeof copy, "%.*s (Velo)%s", (int)(dot - path), path, dot);
    else snprintf(copy, sizeof copy, "%s (Velo)", path);
    if (!download_as(sync, &entries->items[index], copy)) return false;
    sync->result->conflicts++;
    sync_log(sync, "%s changed on both: kept the Velo's copy as %s", path, copy);
    char local[PATH_SIZE * 2];
    struct stat info;
    local_path(sync, copy, local, sizeof local);
    entry_t *added = find_or_add(entries, copy);
    if (added && stat(local, &info) == 0) {
        added->on_mac = true;
        added->mac_time = (int64_t)info.st_mtime;
        added->mac_size = (uint64_t)info.st_size;
    }
    return upload(sync, &entries->items[index]);
}

static bool apply(sync_t *sync, entries_t *entries, size_t index) {
    entry_t *entry = &entries->items[index];
    bool mac_changed = entry->on_mac && (!entry->in_manifest || entry->mac_time != entry->known_mac_time || entry->mac_size != entry->known_mac_size);
    bool velo_changed = entry->on_velo && (!entry->in_manifest || entry->velo_time != entry->known_velo_time || entry->velo_size != entry->known_velo_size);
    if (entry->on_mac && entry->on_velo) {
        if (!entry->in_manifest && entry->mac_size == entry->velo_size) return true;
        if (!entry->in_manifest || (mac_changed && velo_changed)) return resolve_conflict(sync, entries, index);
        if (mac_changed) return upload(sync, entry);
        if (velo_changed) return download(sync, entry);
        return true;
    }
    if (entry->on_mac) {
        if (!entry->in_manifest || mac_changed) return upload(sync, entry);
        char local[PATH_SIZE * 2];
        local_path(sync, entry->path, local, sizeof local);
        if (!move_to_trash(local)) {
            sync_log(sync, "%s was deleted on the Velo; couldn't move the Mac copy to the Trash", entry->path);
            return false;
        }
        entry->on_mac = false;
        sync->result->deleted_on_mac++;
        sync_log(sync, "%s was deleted on the Velo: moved the Mac copy to the Trash", entry->path);
        return true;
    }
    if (entry->on_velo) {
        if (!entry->in_manifest || velo_changed) return download(sync, entry);
        char remote[PATH_SIZE * 2];
        remote_path(sync, entry->path, remote, sizeof remote);
        if (!rapi_delete(sync->rapi, remote)) {
            sync_log(sync, "can't delete %s on the Velo: %s", entry->path, rapi_error(sync->rapi));
            check_connection(sync);
            return false;
        }
        entry->on_velo = false;
        sync->result->deleted_on_velo++;
        sync_log(sync, "%s was deleted on the Mac: deleted it on the Velo", entry->path);
        return true;
    }
    return true;
}

bool rapi_sync_run(rapi_t *rapi, const char *folder, const char *remote_root, const char *manifest_path,
                  rapi_sync_log_fn log, void *context, rapi_sync_result_t *result) {
    rapi_sync_result_t counts = { 0 };
    sync_t sync = { rapi, folder, remote_root, log, context, &counts, 0, false };
    entries_t entries = { 0 };
    rapi_store_t store;
    sync.free_space = rapi_store(rapi, &store) ? store.free_size : 0;
    load_manifest(&sync, &entries, manifest_path);
    scan_mac(&sync, &entries, "");
    if (!scan_velo(&sync, &entries, "", 0)) {
        free(entries.items);
        return false;
    }
    size_t applied = 0;
    for (; applied < entries.count && !sync.disconnected; applied++) {
        if (!apply(&sync, &entries, applied)) entries.items[applied].failed = true;
    }
    for (size_t i = applied; i < entries.count; i++) entries.items[i].failed = true;
    save_manifest(&sync, &entries, manifest_path);
    free(entries.items);
    if (result) *result = counts;
    return !sync.disconnected;
}
