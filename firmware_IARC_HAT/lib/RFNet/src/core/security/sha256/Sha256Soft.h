#pragma once
#include <stdint.h>
#include <stddef.h>

// SHA-256 (FIPS 180-4), pure software, no external dependencies.
// Streaming API lets HMAC/PBKDF2 hash multi-part messages (ipad||msg,
// opad||inner) without concatenating into one buffer — avoids heap
// allocation on AVR/Arduino.
// Software-only by design (no HW-accelerated backend like DefaultCipher):
// SHA runs only once, in PBKDF2 at FromPassword() key derivation — not the
// per-packet path — so hardware acceleration buys no throughput.

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t totalLen;     // total message bytes fed
    uint32_t bufLen;       // bytes currently buffered in `buf`
    uint8_t  buf[64];
};

void sha256_init  (Sha256Ctx& ctx);
void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len);
void sha256_final (Sha256Ctx& ctx, uint8_t out[32]);

// One-shot convenience wrapper — equivalent to init/update/final.
void sha256_compute(const uint8_t* data, size_t len, uint8_t out[32]);
