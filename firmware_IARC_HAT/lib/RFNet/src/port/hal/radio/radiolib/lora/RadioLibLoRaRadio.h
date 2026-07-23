#pragma once
#ifndef NATIVE_BUILD
#include <RadioLib.h>
#include "../RadioLibRFRadio.h"
#include "port/Logger.h"

// LoRa layer on top of RadioLibRFRadio.
// - Adds RfProfile (SF/BW/CR).
// - Adds CAD-augmented cca(): also catches weak preambles below the RSSI floor.
// - Works with any RadioLib LoRa chip exposing setSpreadingFactor/setBandwidth/
//   setCodingRate/scanChannel.
// - Per-chip subclasses only need pin assignments, channel/freq tables, and DIO IRQ wiring.
template <typename TRadio>
class RadioLibLoRaRadio : public RadioLibRFRadio<TRadio>
{
public:
    enum class RfProfile
    {
        HIGH_SPEED,
        NORMAL,
        LONG_RANGE
    };

    explicit RadioLibLoRaRadio(::Module *mod, RfProfile profile = RfProfile::NORMAL)
        : RadioLibRFRadio<TRadio>(mod), _profile(profile) {}

    bool init() override
    {
        if (!RadioLibRFRadio<TRadio>::init())
            return false; // base logged the error code
        int16_t err = applyProfile(_profile);
        if (err != RADIOLIB_ERR_NONE)
        {
            LOG_E("Radio", "init: applyProfile failed err=%d", (int)err);
            return false;
        }
        return true;
    }

    // CAD first (cheap, chip-internal); if clear, fall back to the base
    // RSSI sweep to catch non-LoRa interferers. BUSY on either signal.
    CcaResult cca(uint32_t timeoutMs, int8_t rssiThresholdDbm) override
    {
        // A reception mid-flight or an unread frame would be aborted by
        // scanChannel(); report BUSY without touching the receiver.
        if (this->_rxInProgress())
            return CcaResult::BUSY;
        this->_ccaInFlight = true;
        int16_t cad = this->radio.scanChannel();
        this->_ccaInFlight = false;
        if (cad == RADIOLIB_LORA_DETECTED)
        {
            LOG_D("Radio", "CCA: BUSY (CAD detected preamble)");
            // scanChannel() leaves the chip in standby; re-arm since the
            // caller won't send() on BUSY.
            this->radio.startReceive();
            return CcaResult::BUSY;
        }
        return this->_rssiSweep(timeoutMs, (float)rssiThresholdDbm);
    }

    // Modulation parameters (SF/BW/CR), not just TX-side: every node on the same link must match profile.
    int16_t setTransmitProfile(RfProfile profile)
    {
        _profile = profile;
        return applyProfile(profile);
    }

protected:
    // SF/BW/CR setters are uniform across RadioLib's LoRa chips; override
    // only if a chip names them differently.
    virtual int16_t applyProfile(RfProfile p)
    {
        int16_t err;
        switch (p)
        {
        case RfProfile::HIGH_SPEED:
            if ((err = this->radio.setSpreadingFactor(7)) != RADIOLIB_ERR_NONE)
                return err;
            if ((err = this->radio.setBandwidth(500.0f)) != RADIOLIB_ERR_NONE)
                return err;
            if ((err = this->radio.setCodingRate(5)) != RADIOLIB_ERR_NONE)
                return err;
            LOG_I("Radio", "profile HIGH_SPEED: SF7 BW500 CR5");
            return RADIOLIB_ERR_NONE;
        case RfProfile::LONG_RANGE:
            if ((err = this->radio.setSpreadingFactor(12)) != RADIOLIB_ERR_NONE)
                return err;
            if ((err = this->radio.setBandwidth(125.0f)) != RADIOLIB_ERR_NONE)
                return err;
            if ((err = this->radio.setCodingRate(8)) != RADIOLIB_ERR_NONE)
                return err;
            LOG_I("Radio", "profile LONG_RANGE: SF12 BW125 CR8");
            return RADIOLIB_ERR_NONE;
        default:
            if ((err = this->radio.setSpreadingFactor(9)) != RADIOLIB_ERR_NONE)
                return err;
            if ((err = this->radio.setBandwidth(125.0f)) != RADIOLIB_ERR_NONE)
                return err;
            if ((err = this->radio.setCodingRate(7)) != RADIOLIB_ERR_NONE)
                return err;
            LOG_I("Radio", "profile NORMAL: SF9 BW125 CR7");
            return RADIOLIB_ERR_NONE;
        }
    }

private:
    RfProfile _profile;
};

#endif // NATIVE_BUILD
