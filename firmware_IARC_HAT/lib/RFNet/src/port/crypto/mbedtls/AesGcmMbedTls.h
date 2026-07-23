#pragma once

// AES-128-GCM via mbedTLS (constant-time tag compare handled internally).
#if __has_include("mbedtls/build_info.h")
    #include "mbedtls/build_info.h"
#elif __has_include("mbedtls/config.h")
    #include "mbedtls/config.h"
#endif

#if defined(MBEDTLS_GCM_C)

#include "../../../core/security/ICypher.h"
#include "mbedtls/gcm.h"

class AesGcmMbedTls final : public ICypher {
    mbedtls_gcm_context _ctx;
    bool _ready = false;

public:
    AesGcmMbedTls()  { mbedtls_gcm_init(&_ctx); }
    ~AesGcmMbedTls() { mbedtls_gcm_free(&_ctx); }

    bool setKey(const uint8_t key[CRYPTO_KEY_SIZE]) override {
        _ready = false;
        _ready = (mbedtls_gcm_setkey(&_ctx, MBEDTLS_CIPHER_ID_AES,
                                     key, CRYPTO_KEY_SIZE * 8) == 0);
        return _ready;
    }

    bool encrypt(const uint8_t* aad, size_t aadLen,
                 uint8_t* data, size_t dataLen,
                 const uint8_t nonce[CRYPTO_NONCE_SIZE],
                 uint8_t tag[CRYPTO_TAG_SIZE]) override {
        if (!_ready) return false;
        return mbedtls_gcm_crypt_and_tag(
            &_ctx, MBEDTLS_GCM_ENCRYPT, dataLen,
            nonce, CRYPTO_NONCE_SIZE,
            aad, aadLen,
            data, data,
            CRYPTO_TAG_SIZE, tag) == 0;
    }

    bool decrypt(const uint8_t* aad, size_t aadLen,
                 uint8_t* data, size_t dataLen,
                 const uint8_t nonce[CRYPTO_NONCE_SIZE],
                 const uint8_t tag[CRYPTO_TAG_SIZE]) override {
        if (!_ready) return false;
        return mbedtls_gcm_auth_decrypt(
            &_ctx, dataLen,
            nonce, CRYPTO_NONCE_SIZE,
            aad, aadLen,
            tag, CRYPTO_TAG_SIZE,
            data, data) == 0;
    }
};

#endif // MBEDTLS_GCM_C
