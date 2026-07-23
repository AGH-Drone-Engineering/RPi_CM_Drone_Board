#pragma once
#include "../INVBackend.h"
#include "../../../../core/RFConfig.h"
#include <string.h>

// Volatile-RAM fallback when no NVM backend is detected. Nothing survives a
// reboot; see DefaultNVBackend.h.
#ifndef RF_NULL_NVS_SLOTS
#  define RF_NULL_NVS_SLOTS 4
#endif
// Matches RF_NV_SLOT_BYTES so records sized for the EEPROM backend fit here too.
#ifndef RF_NULL_NVS_SLOT_BYTES
#  define RF_NULL_NVS_SLOT_BYTES RF_NV_SLOT_BYTES
#endif

class NullNVBackend : public INVBackend {
public:
    void begin() override {}

    bool isPersistent() const override { return false; }

    bool read(uint16_t id, void* out, size_t len) override {
        if (len > RF_NULL_NVS_SLOT_BYTES) return false;
        for (auto& s : _slots) {
            if (s.used && s.id == id) {
                memcpy(out, s.data, len);
                return true;
            }
        }
        return false;
    }

    bool write(uint16_t id, const void* in, size_t len) override {
        if (len > RF_NULL_NVS_SLOT_BYTES) return false;
        for (auto& s : _slots) {
            if (s.used && s.id == id) { memcpy(s.data, in, len); return true; }
        }
        for (auto& s : _slots) {
            if (!s.used) {
                s.used = true; s.id = id;
                memcpy(s.data, in, len);
                return true;
            }
        }
        return false;  // all slots taken
    }

private:
    struct Slot {
        bool     used = false;
        uint16_t id   = 0;
        uint8_t  data[RF_NULL_NVS_SLOT_BYTES] = {};
    };
    Slot _slots[RF_NULL_NVS_SLOTS];
};
