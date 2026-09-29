#pragma once
#include "rapi.h"

typedef void (*rapisync_log_fn)(void *context, const char *message);

typedef struct {
    unsigned uploaded, downloaded, deleted_on_mac, deleted_on_velo, conflicts, skipped;
} rapisync_result_t;

bool rapisync_run(rapi_t *rapi, const char *folder, const char *remote_root, const char *manifest_path,
                  rapisync_log_fn log, void *context, rapisync_result_t *result);
