#pragma once
#include <string.h>
#include "../../../core/security/ICypher.h"

// AES-128-GCM, pure software, no external deps. Tag truncated to CRYPTO_TAG_SIZE (8) bytes.

class AesGcmSoft final : public ICypher {
    uint8_t _rk[176] = {};  // expanded AES-128 key schedule: 11 round keys x 16 bytes
    bool    _ready   = false;

    void _aesBlock(const uint8_t in[16], uint8_t out[16]) const;              // AES-128 single block (FIPS-197)
    void _ctrCrypt(const uint8_t J0[16], uint8_t* data, size_t len) const;    // keystream from inc32(J0); J0 itself masks the tag
    void _computeTag(const uint8_t H[16], const uint8_t EJ0[16],
                     const uint8_t* aad, size_t aadLen,
                     const uint8_t* C,   size_t cLen,
                     uint8_t tag[CRYPTO_TAG_SIZE]) const;

public:
    ~AesGcmSoft() override { memset(_rk, 0, sizeof(_rk)); }
    bool setKey(const uint8_t key[CRYPTO_KEY_SIZE]) override;  // AES-128 key expansion (FIPS-197)

    bool encrypt(const uint8_t* aad, size_t aadLen,
                 uint8_t* data, size_t dataLen,
                 const uint8_t nonce[CRYPTO_NONCE_SIZE],
                 uint8_t tag[CRYPTO_TAG_SIZE]) override;

    bool decrypt(const uint8_t* aad, size_t aadLen,
                 uint8_t* data, size_t dataLen,
                 const uint8_t nonce[CRYPTO_NONCE_SIZE],
                 const uint8_t tag[CRYPTO_TAG_SIZE]) override;
};
