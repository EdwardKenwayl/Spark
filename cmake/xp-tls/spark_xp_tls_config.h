#pragma once

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

// Use Mbed TLS's address parser instead of Vista's inet_pton.
#define MBEDTLS_TEST_SW_INET_PTON
// curl owns the sockets; Mbed TLS only supplies cryptography and TLS.
#undef MBEDTLS_NET_C
