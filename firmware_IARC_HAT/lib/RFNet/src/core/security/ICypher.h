#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr uint8_t CRYPTO_KEY_SIZE   = 16;
constexpr uint8_t CRYPTO_NONCE_SIZE = 12;
constexpr uint8_t CRYPTO_TAG_SIZE   = 8;

// Contract:
// - Single-threaded per instance — caller must serialize calls.
// - encrypt()/decrypt() work in-place (data is both in and out).
// - decrypt() MUST verify the tag in constant time before returning success.
class ICypher {
public:
    virtual ~ICypher() = default;

    virtual bool setKey(const uint8_t key[CRYPTO_KEY_SIZE]) = 0;

    virtual bool encrypt(
        const uint8_t* aad,  size_t aadLen,
        uint8_t*       data, size_t dataLen,
        const uint8_t  nonce[CRYPTO_NONCE_SIZE],
        uint8_t        tag[CRYPTO_TAG_SIZE]) = 0;

    virtual bool decrypt(
        const uint8_t* aad,  size_t aadLen,
        uint8_t*       data, size_t dataLen,
        const uint8_t  nonce[CRYPTO_NONCE_SIZE],
        const uint8_t  tag[CRYPTO_TAG_SIZE]) = 0;
};
