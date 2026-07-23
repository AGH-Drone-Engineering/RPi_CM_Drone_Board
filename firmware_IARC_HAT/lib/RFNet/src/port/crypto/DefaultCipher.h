#pragma once

// Selects the ICypher backend: mbedTLS GCM > STM32 CRYP HAL > software fallback.
// STM32 CRYP HAL only compiles in when MBEDTLS_GCM_C is undefined (see
// AesGcmStm32Hal.h's own guard) — the check below mirrors that guard exactly
// so the two files can't disagree about which backend actually exists.
// Define RF_CIPHER_OVERRIDE beforehand to supply your own DefaultCipher/RF_CIPHER_BACKEND.
#ifndef RF_CIPHER_OVERRIDE

#if __has_include("mbedtls/build_info.h")
#  include "mbedtls/build_info.h"
#elif __has_include("mbedtls/config.h")
#  include "mbedtls/config.h"
#endif

#if defined(MBEDTLS_GCM_C)
#  include "mbedtls/AesGcmMbedTls.h"
   using DefaultCipher = AesGcmMbedTls;
#  define RF_CIPHER_BACKEND "AES-128-GCM (mbedTLS)"
#elif defined(HAL_CRYP_MODULE_ENABLED) && \
    (defined(STM32F4xx) || defined(STM32F7xx) || defined(STM32H7xx) || \
     defined(STM32L4xx) || defined(STM32WBxx))
#  include "stm32hal/AesGcmStm32Hal.h"
   using DefaultCipher = AesGcmStm32Hal;
#  define RF_CIPHER_BACKEND "AES-128-GCM (STM32 CRYP)"
#else
#  include "soft/AesGcmSoft.h"
   using DefaultCipher = AesGcmSoft;
#  define RF_CIPHER_BACKEND "AES-128-GCM (software)"
#endif

#endif // RF_CIPHER_OVERRIDE
