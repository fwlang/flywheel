/* Windows threading primitives for the bundled Mbed TLS (MBEDTLS_THREADING_ALT).
 * SRWLOCK and CONDITION_VARIABLE are each one pointer and need no destruction; they are
 * declared opaquely here so this header does not pull <windows.h> into the runtime. */
#ifndef FLYWHEEL_THREADING_ALT_H
#define FLYWHEEL_THREADING_ALT_H
typedef struct {
    void *lock;
} mbedtls_platform_mutex_t;
typedef struct {
    void *condition;
} mbedtls_platform_condition_variable_t;
#endif
