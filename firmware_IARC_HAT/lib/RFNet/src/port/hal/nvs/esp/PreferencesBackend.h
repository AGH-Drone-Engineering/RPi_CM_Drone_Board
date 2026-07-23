#pragma once
// ESP32 NV backend on Arduino's Preferences library (esp-idf NVS flash).
// Namespace RF_NVS_NAMESPACE, keys "k<id>"; survives deep sleep and reboots.
// Override RF_NVS_NAMESPACE if it collides with another Preferences user.
#include "../INVBackend.h"
#include "../../../../core/RFConfig.h"
#include "../../../Logger.h"

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>
#include <stdio.h>

// ESP-IDF NVS namespace names are limited to 15 characters.
static_assert(sizeof(RF_NVS_NAMESPACE) - 1 >= 1
              && sizeof(RF_NVS_NAMESPACE) - 1 <= 15,
              "RF_NVS_NAMESPACE must be 1..15 characters (ESP-IDF NVS limit)");

class PreferencesBackend : public INVBackend {
public:
    PreferencesBackend() = default;

    void begin() override {
        _ready = _prefs.begin(RF_NVS_NAMESPACE, /*readOnly=*/false);
        if (!_ready) {
            LOG_E("Prefs", "begin: NVS namespace '" RF_NVS_NAMESPACE "' open failed"
                  " — seq/duty will NOT persist");
        }
    }

    bool isPersistent() const override { return _ready; }

    bool read(uint16_t id, void* out, size_t len) override {
        if (!_ready) return false;
        char key[8]; _key(id, key);
        // Avoids a noisy getBytes() error log for a missing key.
        if (!_prefs.isKey(key)) return false;
        // Avoids a noisy getBytes() error log for a length mismatch.
        if (_prefs.getBytesLength(key) != len) return false;
        return _prefs.getBytes(key, out, len) == len;
    }

    bool write(uint16_t id, const void* in, size_t len) override {
        char key[8]; _key(id, key);
        return _prefs.putBytes(key, in, len) == len;
    }

private:
    static void _key(uint16_t id, char out[8]) { snprintf(out, 8, "k%u", id); }
    Preferences _prefs;
    bool        _ready = false;
};
#endif
