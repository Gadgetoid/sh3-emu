#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PPFS_FILES        16
#define PPFS_PATH_MAX     1024
#define PPFS_MESSAGE_MAX  (64 * 1024 + 64)

typedef void (*ppfs_log_fn)(void *context, const char *message);

typedef struct {
    char     root[PPFS_PATH_MAX];
    FILE    *files[PPFS_FILES];
    char     find_pattern[256];
    int      find_index;
    uint32_t find_handle;
    uint8_t  input[PPFS_MESSAGE_MAX];
    size_t   input_length;
    uint8_t  output[PPFS_MESSAGE_MAX];
    size_t   output_length;
    size_t   output_position;
    int      send_phase;
    uint8_t  latched;
    ppfs_log_fn log;
    void    *log_context;
} ppfs_t;

void     ppfs_init(ppfs_t *ppfs);
void     ppfs_close_all(ppfs_t *ppfs);
void     ppfs_set_root(ppfs_t *ppfs, const char *root);
uint32_t ppfs_read_register(ppfs_t *ppfs);
void     ppfs_write_register(ppfs_t *ppfs, uint32_t value);
