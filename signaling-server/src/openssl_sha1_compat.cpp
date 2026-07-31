#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <cstddef>
#include <cstdint>

extern "C" unsigned char* SHA1(const unsigned char* data,
                               std::size_t length,
                               unsigned char* digest) {
    static unsigned char fallback_digest[20] = {};
    if (digest == nullptr) {
        digest = fallback_digest;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    DWORD digest_length = 20;

    if (!CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT) &&
        !CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        ZeroMemory(digest, digest_length);
        return digest;
    }

    if (!CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        ZeroMemory(digest, digest_length);
        return digest;
    }

    if (data != nullptr && length > 0) {
        const DWORD chunk = static_cast<DWORD>(
            length > 0xFFFFFFFFu ? 0xFFFFFFFFu : length);
        if (!CryptHashData(hash, data, chunk, 0)) {
            CryptDestroyHash(hash);
            CryptReleaseContext(provider, 0);
            ZeroMemory(digest, digest_length);
            return digest;
        }
    }

    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_length, 0)) {
        ZeroMemory(digest, digest_length);
    }

    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return digest;
}
#endif
