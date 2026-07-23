#pragma once

// Non-volatile storage backend for duty-cycle state and replay-protection counters.
// Auto-selects by platform; inject a custom implementation via cfg.nv.backend.
// - Without persistence: duty-cycle deadline and nonce counter reset every reboot.
// - Reset nonce counter reuses GCM (key, nonce) pairs -> breaks confidentiality/integrity.
// - Engine::begin() refuses an encrypted start on a non-persistent backend unless RF_ALLOW_VOLATILE_NV is defined.

#if defined(__ZEPHYR__)
#  include "zephyr/ZephyrNvsBackend.h"
   using DefaultNVBackend = ZephyrNvsBackend;

#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#  include "esp/PreferencesBackend.h"
   using DefaultNVBackend = PreferencesBackend;

#elif defined(ARDUINO)
#  include "arduino/EepromBackend.h"
   using DefaultNVBackend = EepromBackend;

#else
#  warning "DefaultNVBackend: no NVM backend detected — using volatile RAM (no persistence)"
#  include "null/NullNVBackend.h"
   using DefaultNVBackend = NullNVBackend;
#endif
