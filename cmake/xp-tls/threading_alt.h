#pragma once

#include <windows.h>

typedef struct mbedtls_threading_mutex_t {
    CRITICAL_SECTION section;
    int valid;
} mbedtls_threading_mutex_t;
