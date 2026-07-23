#include "AesGcmStm32Hal.h"
#if !defined(MBEDTLS_GCM_C) && defined(HAL_CRYP_MODULE_ENABLED) && \
    (defined(STM32F4xx) || defined(STM32F7xx) || defined(STM32H7xx) || \
     defined(STM32L4xx) || defined(STM32WBxx))

#include <string.h>
#include "../../../core/RFConfig.h"
#include "../../Logger.h"

AesGcmStm32Hal::AesGcmStm32Hal() {
    _hcryp.Instance = CRYP;
}

AesGcmStm32Hal::~AesGcmStm32Hal() {
    HAL_CRYP_DeInit(&_hcryp);
    memset(_keyWords, 0, sizeof(_keyWords));
}

bool AesGcmStm32Hal::setKey(const uint8_t key[CRYPTO_KEY_SIZE]) {
    for (size_t i = 0; i < 4; i++) {
        _keyWords[i] = ((uint32_t)key[i*4+0] << 24)
                     | ((uint32_t)key[i*4+1] << 16)
                     | ((uint32_t)key[i*4+2] <<  8)
                     |  (uint32_t)key[i*4+3];
    }

    _hcryp.Init = {
        .Algorithm=CRYP_AES_GCM, .KeySize=CRYP_KEYSIZE_128B, .pKey=_keyWords,
        .pInitVect=nullptr, .Header=nullptr, .HeaderSize=0,
        .DataWidthUnit=CRYP_DATAWIDTHUNIT_BYTE, .DataType=CRYP_DATATYPE_8B,
        .KeyIVConfigSkip=CRYP_KEYIVCONFIG_ALWAYS,
    };
    _ready = (HAL_CRYP_Init(&_hcryp) == HAL_OK);
    return _ready;
}

static void buildHalIV(uint32_t iv[4], const uint8_t nonce[CRYPTO_NONCE_SIZE]) {
    iv[0] = ((uint32_t)nonce[0]<<24)|((uint32_t)nonce[1]<<16)|((uint32_t)nonce[2]<<8)|(uint32_t)nonce[3];
    iv[1] = ((uint32_t)nonce[4]<<24)|((uint32_t)nonce[5]<<16)|((uint32_t)nonce[6]<<8)|(uint32_t)nonce[7];
    iv[2] = ((uint32_t)nonce[8]<<24)|((uint32_t)nonce[9]<<16)|((uint32_t)nonce[10]<<8)|(uint32_t)nonce[11];
    iv[3] = 0x00000002;
}

static void extractTag(uint8_t tag[CRYPTO_TAG_SIZE], const uint32_t tagWords[4]) {
    for (size_t i = 0; i < CRYPTO_TAG_SIZE; i++)
        tag[i] = (tagWords[i/4] >> (24 - (i%4)*8)) & 0xFF;
}

static uint32_t* alignAad(uint32_t buf[2], const uint8_t* aad, size_t aadLen) {
    if (aadLen > 8) return nullptr;
    buf[0] = buf[1] = 0;
    memcpy(buf, aad, aadLen);
    return buf;
}

bool AesGcmStm32Hal::encrypt(const uint8_t* aad, size_t aadLen,
                               uint8_t* data, size_t dataLen,
                               const uint8_t nonce[CRYPTO_NONCE_SIZE],
                               uint8_t tag[CRYPTO_TAG_SIZE]) {
    if (!_ready) return false;
    if (dataLen > RF_FRAME_BUF_SIZE) {
        LOG_E("AesGcmHal", "encrypt: dataLen too large (%u > %u)",
              (unsigned)dataLen, (unsigned)RF_FRAME_BUF_SIZE);
        return false;
    }

    uint32_t iv[4]; buildHalIV(iv, nonce);
    uint32_t aadBuf[2];
    uint32_t* aadAligned = alignAad(aadBuf, aad, aadLen);
    if (!aadAligned) { LOG_E("AesGcmHal", "encrypt: AAD too large (%u > 8)", (unsigned)aadLen); return false; }

    // DMA requires 4-byte alignment.
    alignas(uint32_t) uint8_t aligned[RF_FRAME_BUF_SIZE];
    memcpy(aligned, data, dataLen);

    _hcryp.Init.pInitVect = iv;
    _hcryp.Init.Header    = aadAligned;
    _hcryp.Init.HeaderSize = (uint32_t)aadLen;
    if (HAL_CRYP_Init(&_hcryp) != HAL_OK) { LOG_E("AesGcmHal", "encrypt: HAL_CRYP_Init failed"); return false; }
    if (HAL_CRYP_Encrypt(&_hcryp, reinterpret_cast<uint32_t*>(aligned), dataLen,
                         reinterpret_cast<uint32_t*>(aligned), 1000) != HAL_OK) {
        LOG_E("AesGcmHal", "encrypt: HAL_CRYP_Encrypt failed len=%u", (unsigned)dataLen);
        return false;
    }
    uint32_t tagWords[4]{};
    if (HAL_CRYPEx_AESGCM_GenerateAuthTAG(&_hcryp, tagWords, 1000) != HAL_OK) {
        LOG_E("AesGcmHal", "encrypt: tag generation failed");
        return false;
    }
    extractTag(tag, tagWords);
    memcpy(data, aligned, dataLen);
    return true;
}

bool AesGcmStm32Hal::decrypt(const uint8_t* aad, size_t aadLen,
                               uint8_t* data, size_t dataLen,
                               const uint8_t nonce[CRYPTO_NONCE_SIZE],
                               const uint8_t tag[CRYPTO_TAG_SIZE]) {
    if (!_ready) return false;
    if (dataLen > RF_FRAME_BUF_SIZE) {
        LOG_E("AesGcmHal", "decrypt: dataLen too large (%u > %u)",
              (unsigned)dataLen, (unsigned)RF_FRAME_BUF_SIZE);
        return false;
    }

    uint32_t iv[4]; buildHalIV(iv, nonce);
    uint32_t aadBuf[2];
    uint32_t* aadAligned = alignAad(aadBuf, aad, aadLen);
    if (!aadAligned) { LOG_E("AesGcmHal", "decrypt: AAD too large (%u > 8)", (unsigned)aadLen); return false; }

    alignas(uint32_t) uint8_t aligned[RF_FRAME_BUF_SIZE];
    memcpy(aligned, data, dataLen);

    _hcryp.Init.pInitVect = iv;
    _hcryp.Init.Header    = aadAligned;
    _hcryp.Init.HeaderSize = (uint32_t)aadLen;
    if (HAL_CRYP_Init(&_hcryp) != HAL_OK) { LOG_E("AesGcmHal", "decrypt: HAL_CRYP_Init failed"); return false; }
    if (HAL_CRYP_Decrypt(&_hcryp, reinterpret_cast<uint32_t*>(aligned), dataLen,
                         reinterpret_cast<uint32_t*>(aligned), 1000) != HAL_OK) {
        LOG_E("AesGcmHal", "decrypt: HAL_CRYP_Decrypt failed len=%u", (unsigned)dataLen);
        return false;
    }
    uint32_t computedWords[4]{};
    if (HAL_CRYPEx_AESGCM_GenerateAuthTAG(&_hcryp, computedWords, 1000) != HAL_OK) {
        LOG_E("AesGcmHal", "decrypt: tag generation failed");
        return false;
    }

    uint8_t diff = 0;
    for (size_t i = 0; i < CRYPTO_TAG_SIZE; i++) {
        uint8_t computed = (computedWords[i/4] >> (24 - (i%4)*8)) & 0xFF;
        diff |= computed ^ tag[i];
    }
    if (diff != 0) { memset(data, 0, dataLen); return false; }
    memcpy(data, aligned, dataLen);
    return true;
}

#endif
