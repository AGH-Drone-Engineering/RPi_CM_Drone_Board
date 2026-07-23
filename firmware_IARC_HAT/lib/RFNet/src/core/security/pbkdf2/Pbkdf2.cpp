#include "Pbkdf2.h"
#include "../sha256/Sha256Soft.h"
#include <string.h>

static void hmac_sha256(const uint8_t* key, size_t keyLen,
                         const uint8_t* msg, size_t msgLen,
                         uint8_t out[32]) {
    uint8_t k[64] = {};
    if (keyLen <= 64) {
        memcpy(k, key, keyLen);
    } else {
        sha256_compute(key, keyLen, k);  // remaining 32 bytes stay zero-padded
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    uint8_t inner[32];
    {
        Sha256Ctx c;
        sha256_init(c);
        sha256_update(c, ipad, 64);
        sha256_update(c, msg,  msgLen);
        sha256_final (c, inner);
    }
    {
        Sha256Ctx c;
        sha256_init(c);
        sha256_update(c, opad,  64);
        sha256_update(c, inner, 32);
        sha256_final (c, out);
    }
}

void pbkdf2_sha256(const char*    password, size_t passLen,
                   const uint8_t* salt,     size_t saltLen,
                   uint32_t       iter,
                   uint8_t*       out,      size_t outLen) {
    const uint8_t* pkey = reinterpret_cast<const uint8_t*>(password);
    // INT(i) per RFC 2898 must be uint32_t: AVR's 16-bit size_t makes >>24 UB.
    uint32_t block = 0;

    while (outLen > 0) {
        block++;
        // U_1 = HMAC(password, salt || INT(block))
        uint8_t counterBe[4] = {
            (uint8_t)(block >> 24),
            (uint8_t)(block >> 16),
            (uint8_t)(block >>  8),
            (uint8_t)(block      ),
        };

        uint8_t k[64] = {};
        if (passLen <= 64) memcpy(k, pkey, passLen);
        else               sha256_compute(pkey, passLen, k);

        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

        uint8_t U[32], T[32];
        {
            Sha256Ctx c;
            sha256_init(c);
            sha256_update(c, ipad,      64);
            sha256_update(c, salt,      saltLen);
            sha256_update(c, counterBe, 4);
            sha256_final (c, U);
        }
        {
            Sha256Ctx c;
            sha256_init(c);
            sha256_update(c, opad, 64);
            sha256_update(c, U,    32);
            sha256_final (c, U);
        }
        memcpy(T, U, 32);

        for (uint32_t i = 1; i < iter; i++) {
            hmac_sha256(pkey, passLen, U, 32, U);
            for (int j = 0; j < 32; j++) T[j] ^= U[j];
        }

        size_t copy = (outLen < 32) ? outLen : 32;
        memcpy(out, T, copy);
        out    += copy;
        outLen -= copy;
    }
}
