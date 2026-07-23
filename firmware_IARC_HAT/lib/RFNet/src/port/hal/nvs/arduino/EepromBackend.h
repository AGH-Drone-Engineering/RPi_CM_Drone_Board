#pragma once
#include "../INVBackend.h"
#include "../../../../core/RFConfig.h"
#include "../../../Logger.h"

#if defined(ARDUINO) && !defined(ARDUINO_ARCH_ESP32)
#include <EEPROM.h>
#include <string.h>

// A 0xA5 marker byte flags a written slot; an erased (0xFF) cell reads as absent.
class EepromBackend : public INVBackend {
public:
    explicit EepromBackend(int baseAddr = 0) : _base(baseAddr) {}

    void begin() override {
#if defined(ARDUINO_ARCH_AVR)
        _ready = true;   // AVR EEPROM is directly addressable — no init to fail
#else
        _ready = EEPROM.begin(RF_EEPROM_SIZE_BYTES);
        if (!_ready) LOG_E("Eeprom", "begin(%u) failed — seq/duty will NOT persist",
                           (unsigned)RF_EEPROM_SIZE_BYTES);
#endif
    }

    bool isPersistent() const override { return _ready; }

    bool read(uint16_t id, void* out, size_t len) override {
        if (!_ready || len + 1 > RF_NV_SLOT_BYTES) return false;
        int off = _base + (int)id * RF_NV_SLOT_BYTES;
        if (EEPROM.read(off) != MARKER) return false;
        uint8_t* dst = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < len; ++i) dst[i] = EEPROM.read(off + 1 + i);
        return true;
    }

    bool write(uint16_t id, const void* in, size_t len) override {
        if (!_ready || len + 1 > RF_NV_SLOT_BYTES) return false;
        int off = _base + (int)id * RF_NV_SLOT_BYTES;
        const uint8_t* src = static_cast<const uint8_t*>(in);
        EEPROM.update(off, MARKER);
        for (size_t i = 0; i < len; ++i) EEPROM.update(off + 1 + i, src[i]);
#if !defined(ARDUINO_ARCH_AVR)
        return EEPROM.commit();
#else
        return true;
#endif
    }

private:
    static constexpr uint8_t MARKER = 0xA5;
    int  _base;
    bool _ready = false;
};
#endif
