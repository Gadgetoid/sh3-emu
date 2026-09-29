#include "rapi.h"
#include "rapisync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REMOTE_HOME "\\My Documents"

static const char *usage =
    "usage: velo-rapi [--socket=PATH] COMMAND\n"
    "  info                     OS version and storage\n"
    "  ls [PATH]                list a folder (default \\My Documents)\n"
    "  get PATH [LOCAL]         copy a file from the Velo\n"
    "  put LOCAL [PATH]         copy a file to the Velo\n"
    "  rm PATH                  delete a file\n"
    "  mkdir PATH | rmdir PATH  create or remove a folder\n"
    "  mv FROM TO               move or rename\n"
    "  run PROGRAM [ARGUMENTS]  start a program\n"
    "  sync FOLDER              sync a Mac folder with \\My Documents\n"
    "Velo paths are relative to \\My Documents unless they start with / or \\; / and \\ both separate folders.\n";

static void velo_path(const char *path, char *out, size_t size) {
    if (path[0] == '/' || path[0] == '\\') snprintf(out, size, "%s", path);
    else if (*path) snprintf(out, size, "%s\\%s", REMOTE_HOME, path);
    else snprintf(out, size, "%s", REMOTE_HOME);
    for (char *p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    size_t length = strlen(out);
    while (length > 1 && out[length - 1] == '\\') out[--length] = 0;
}

static const char *leaf_of(const char *path) {
    const char *leaf = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') leaf = p + 1;
    }
    return leaf;
}

static void show_progress(void *context, uint64_t done, uint64_t total) {
    if (!isatty(STDERR_FILENO)) return;
    fprintf(stderr, "\r%s %llu%%", (const char *)context, total ? (unsigned long long)(done * 100 / total) : 100ull);
    if (done >= total) fputc('\n', stderr);
}

static void log_line(void *context, const char *message) {
    (void)context;
    printf("%s\n", message);
}

static int fail(rapi_t *rapi) {
    fprintf(stderr, "velo-rapi: %s\n", rapi_error(rapi));
    return 1;
}

static int list(rapi_t *rapi, const char *path) {
    char pattern[1100];
    velo_path(path, pattern, sizeof pattern - 2);
    rapi_file_t info;
    if (strpbrk(pattern, "*?") == NULL && (!rapi_stat(rapi, pattern, &info) || (info.attributes & RAPI_ATTRIBUTE_DIRECTORY))) {
        strcat(pattern, "\\*");
    }
    rapi_file_t *files;
    size_t count;
    if (!rapi_list(rapi, pattern, &files, &count)) return fail(rapi);
    for (size_t i = 0; i < count; i++) {
        if (files[i].attributes & RAPI_ATTRIBUTE_DIRECTORY) printf("%10s  %s\\\n", "", files[i].name);
        else printf("%10u  %s\n", files[i].size, files[i].name);
    }
    free(files);
    return 0;
}

static int put(rapi_t *rapi, const char *local, const char *path) {
    char remote[1100];
    velo_path(path ? path : "", remote, sizeof remote);
    rapi_file_t info;
    if (!path || (rapi_stat(rapi, remote, &info) && (info.attributes & RAPI_ATTRIBUTE_DIRECTORY))) {
        size_t length = strlen(remote);
        snprintf(remote + length, sizeof remote - length, "\\%s", leaf_of(local));
    }
    return rapi_upload(rapi, local, remote, show_progress, (void *)leaf_of(local)) ? 0 : fail(rapi);
}

static int get(rapi_t *rapi, const char *path, const char *local) {
    char remote[1100];
    velo_path(path, remote, sizeof remote);
    const char *target = local ? local : leaf_of(remote);
    return rapi_download(rapi, remote, target, show_progress, (void *)leaf_of(remote)) ? 0 : fail(rapi);
}

static int sync_folder(rapi_t *rapi, const char *folder) {
    char manifest[1100], absolute[1024];
    if (!realpath(folder, absolute)) {
        fprintf(stderr, "velo-rapi: no folder %s\n", folder);
        return 1;
    }
    rapi_data_path("sync-manifest.txt", manifest, sizeof manifest);
    rapisync_result_t result;
    if (!rapisync_run(rapi, absolute, REMOTE_HOME, manifest, log_line, NULL, &result)) return fail(rapi);
    printf("%u to the Velo, %u from the Velo, %u deleted on the Mac, %u deleted on the Velo, %u conflicts, %u skipped\n",
           result.uploaded, result.downloaded, result.deleted_on_mac, result.deleted_on_velo, result.conflicts, result.skipped);
    return 0;
}

int main(int argc, char **argv) {
    char socket_path[1024];
    int first = 1;
    if (argc > 1 && !strncmp(argv[1], "--socket=", 9)) {
        snprintf(socket_path, sizeof socket_path, "%s", argv[1] + 9);
        first = 2;
    } else {
        rapi_data_path("rapi.sock", socket_path, sizeof socket_path);
    }
    if (argc <= first) {
        fputs(usage, stderr);
        return 2;
    }
    const char *command = argv[first];
    int count = argc - first - 1;
    char **args = argv + first + 1;
    char error[1200];
    rapi_t *rapi = rapi_connect(socket_path, error, sizeof error);
    if (!rapi) {
        fprintf(stderr, "velo-rapi: %s\n", error);
        return 1;
    }
    char path[1100], second[1100];
    int status = 2;
    if (!strcmp(command, "info") && count == 0) {
        rapi_version_t version;
        rapi_store_t store;
        if (!rapi_version(rapi, &version) || !rapi_store(rapi, &store)) status = fail(rapi);
        else {
            printf("Windows CE %u.%02u build %u\nstorage %u KB, %u KB free\n", version.major, version.minor, version.build,
                   store.store_size / 1024, store.free_size / 1024);
            status = 0;
        }
    }
    else if (!strcmp(command, "ls") && count <= 1) status = list(rapi, count ? args[0] : "");
    else if (!strcmp(command, "get") && (count == 1 || count == 2)) status = get(rapi, args[0], count == 2 ? args[1] : NULL);
    else if (!strcmp(command, "put") && (count == 1 || count == 2)) status = put(rapi, args[0], count == 2 ? args[1] : NULL);
    else if (!strcmp(command, "rm") && count == 1) { velo_path(args[0], path, sizeof path); status = rapi_delete(rapi, path) ? 0 : fail(rapi); }
    else if (!strcmp(command, "mkdir") && count == 1) { velo_path(args[0], path, sizeof path); status = rapi_make_directory(rapi, path) ? 0 : fail(rapi); }
    else if (!strcmp(command, "rmdir") && count == 1) { velo_path(args[0], path, sizeof path); status = rapi_remove_directory(rapi, path) ? 0 : fail(rapi); }
    else if (!strcmp(command, "mv") && count == 2) {
        velo_path(args[0], path, sizeof path);
        velo_path(args[1], second, sizeof second);
        status = rapi_move(rapi, path, second) ? 0 : fail(rapi);
    }
    else if (!strcmp(command, "run") && count >= 1) {
        char arguments[1100] = "";
        for (int i = 1; i < count; i++) {
            size_t length = strlen(arguments);
            snprintf(arguments + length, sizeof arguments - length, "%s%s", i > 1 ? " " : "", args[i]);
        }
        const char *program = args[0];
        if (program[0] == '/') { velo_path(program, path, sizeof path); program = path; }
        status = rapi_run(rapi, program, arguments) ? 0 : fail(rapi);
    }
    else if (!strcmp(command, "sync") && count == 1) status = sync_folder(rapi, args[0]);
    if (status == 2) fputs(usage, stderr);
    rapi_disconnect(rapi);
    return status;
}
