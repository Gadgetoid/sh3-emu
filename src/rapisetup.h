#pragma once
#include "rapi.h"

#define RAPISETUP_PROXY_SERVER "10.0.2.4"
#define RAPISETUP_PROXY_PORT   8080

bool rapisetup_proxy(rapi_t *rapi, bool enable);
bool rapisetup_connection(rapi_t *rapi, uint32_t baud);
