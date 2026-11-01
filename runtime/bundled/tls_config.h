/* TF-PSA-Crypto user configuration for the Mbed TLS copy bundled into the Flywheel runtime.
 * Flywheel programs use TLS from several threads (HTTP clients, server workers), so the
 * crypto core needs its threading layer. */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
/* Implemented in runtime/tls.inc with SRW locks and condition variables. */
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif
