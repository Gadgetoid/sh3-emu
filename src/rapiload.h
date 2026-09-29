#pragma once
#include "rapi.h"

#define RAPILOAD_DEFAULT_DEST "\\Program Files\\Accessories"

typedef void (*rapiload_log_fn)(void *context, const char *message);

bool rapiload_run(rapi_t *rapi, const char *script, const char *dest, rapiload_log_fn log, void *context);
