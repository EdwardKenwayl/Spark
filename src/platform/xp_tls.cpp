#if defined(SPARK_WINDOWS_XP)
#include <windows.h>
#include <wincrypt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/threading.h>

#include <algorithm>
#include <limits>

extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t length, size_t* written) {
    *written = 0;
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    while (*written < length) {
        const DWORD count = static_cast<DWORD>(std::min<size_t>(
            length - *written, std::numeric_limits<DWORD>::max()));
        if (!CryptGenRandom(provider, count, output + *written)) {
            CryptReleaseContext(provider, 0);
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        *written += count;
    }
    CryptReleaseContext(provider, 0);
    return 0;
}

namespace spark {
namespace {
void Initialize(mbedtls_threading_mutex_t* mutex) {
    mutex->valid = InitializeCriticalSectionAndSpinCount(&mutex->section, 0) ? 1 : 0;
}
void Free(mbedtls_threading_mutex_t* mutex) {
    if (mutex->valid) DeleteCriticalSection(&mutex->section);
    mutex->valid = 0;
}
int Lock(mbedtls_threading_mutex_t* mutex) {
    if (!mutex->valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    EnterCriticalSection(&mutex->section);
    return 0;
}
int Unlock(mbedtls_threading_mutex_t* mutex) {
    if (!mutex->valid) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    LeaveCriticalSection(&mutex->section);
    return 0;
}
}  // namespace
void InitializeXpTls() { mbedtls_threading_set_alt(Initialize, Free, Lock, Unlock); }
}  // namespace spark
#endif
