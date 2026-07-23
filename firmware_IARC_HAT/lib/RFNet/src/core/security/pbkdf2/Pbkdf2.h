#pragma once
#include <stdint.h>
#include <stddef.h>

// PBKDF2-HMAC-SHA256 (RFC 2898).
void pbkdf2_sha256(const char*    password, size_t passLen,
                   const uint8_t* salt,     size_t saltLen,
                   uint32_t       iter,
                   uint8_t*       out,      size_t outLen);
