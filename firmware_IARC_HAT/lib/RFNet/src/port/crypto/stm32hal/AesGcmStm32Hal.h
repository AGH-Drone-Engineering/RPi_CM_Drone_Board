#pragma once
#if __has_include("mbedtls/build_info.h")
    #include "mbedtls/build_info.h"
#elif __has_include("mbedtls/config.h")
    #include "mbedtls/config.h"
#endif

// AES-128-GCM via STM32 CRYP peripheral (HAL); used when mbedTLS GCM isn't available.
// setKey() sets the key once; encrypt()/decrypt() then vary nonce and AAD per message.
// - HAL rewrites the key register on every call regardless (KeyIVConfigSkip=ALWAYS, see .cpp) — internal detail, harmless, invisible from this API.
#if !defined(MBEDTLS_GCM_C) && defined(HAL_CRYP_MODULE_ENABLED) && \
    (defined(STM32F4xx) || defined(STM32F7xx) || defined(STM32H7xx) || \
     defined(STM32L4xx) || defined(STM32WBxx))

#include "../../../core/security/ICypher.h"
#if   __has_include("stm32f4xx_hal.h")
#  include "stm32f4xx_hal.h"
#elif __has_include("stm32f7xx_hal.h")
#  include "stm32f7xx_hal.h"
#elif __has_include("stm32h7xx_hal.h")
#  include "stm32h7xx_hal.h"
#elif __has_include("stm32l4xx_hal.h")
#  include "stm32l4xx_hal.h"
#elif __has_include("stm32wbxx_hal.h")
#  include "stm32wbxx_hal.h"
#elif __has_include("main.h")
#  include "main.h"
#endif

class AesGcmStm32Hal final : public ICypher {
    CRYP_HandleTypeDef _hcryp{};
    uint32_t _keyWords[4]{};
    bool _ready = false;

public:
    AesGcmStm32Hal();
    ~AesGcmStm32Hal() override;
    bool setKey(const uint8_t key[CRYPTO_KEY_SIZE]) override;

    // encrypt()/decrypt(): AAD limited to 8 bytes; dataLen > RF_FRAME_BUF_SIZE is rejected.
    bool encrypt(const uint8_t* aad, size_t aadLen,
                 uint8_t* data, size_t dataLen,
                 const uint8_t nonce[CRYPTO_NONCE_SIZE],
                 uint8_t tag[CRYPTO_TAG_SIZE]) override;
    bool decrypt(const uint8_t* aad, size_t aadLen,
                 uint8_t* data, size_t dataLen,
                 const uint8_t nonce[CRYPTO_NONCE_SIZE],
                 const uint8_t tag[CRYPTO_TAG_SIZE]) override;
};

#endif
