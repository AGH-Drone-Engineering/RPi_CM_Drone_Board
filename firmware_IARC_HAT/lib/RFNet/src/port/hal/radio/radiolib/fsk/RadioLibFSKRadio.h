#pragma once
#ifndef NATIVE_BUILD
#include <RadioLib.h>
#include <string.h>
#include "core/RFConfig.h"
#include "port/hal/radio/IRadio.h"
#include "port/Logger.h"

// Generic scaffolding for RadioLib FSK chips exposing the "packet engine"
// API (HW address filtering + per-packet GDO callback). CC1101 is the
// prototype; same shape covers most TI/Si-derivative FSK frontends.
// - Implements IRadio's L2 framing contract (see IRadio.h): L2 byte
//   prepended/stripped by the chip's HW packet engine (setNodeAddress() +
//   transmit(data, len, addr)), not a manual buffer copy.
// - IRQ events come from a per-packet GDO callback, not flag registers;
//   pollEvent() drains a flag set by the ISR trampoline.
//
// Subclass responsibilities:
//   - Construct ::Module with platform RadioLibHal + pin assignments. For
//     CC1101: IRQ pin is GDO0 (Module's `irq` slot); optional secondary
//     GPIO is GDO2 (Module's `gpio` slot, used by RadioLib's blocking
//     transmit() to detect TX-complete).
//   - Override init() for chip-specific config (channel/frequency, output
//     power) on top of the base RfProfile.
//   - Implement getLiveRssiDbm(): live channel-RSSI read for CCA
//     (chip-specific status register access).
//   - Optionally override isReceivingFrame() so cca() can report BUSY while
//     a frame streams in (e.g. CC1101 PKTSTATUS.SFD).
template <typename TRadio>
class RadioLibFSKRadio : public IRadio {
public:
    // Range vs. time-on-air vs. duty-cycle trade-off; values land on
    // CC1101's quantised setters (datasheet §13).
    //   HIGH_SPEED : 250 kbps, fdev=127 kHz, BW=270 kHz (fast bulk)
    //   NORMAL     : 38.4 kbps, fdev=20 kHz, BW=102 kHz (SmartRF preset)
    //   LONG_RANGE : 1.2 kbps,  fdev=5.2 kHz, BW=58 kHz (sensitivity floor)
    enum class RfProfile { HIGH_SPEED, NORMAL, LONG_RANGE };

    explicit RadioLibFSKRadio(::Module* mod, RfProfile profile = RfProfile::NORMAL)
        : radio(mod), _mod(mod), _profile(profile) {}

    ~RadioLibFSKRadio() override {
        finish();
        delete _mod;
    }

    bool init() override {
        if (_initialized) return true;
        int16_t err;
        if ((err = radio.begin()) != RADIOLIB_ERR_NONE) {
            LOG_E("Radio", "init: begin() failed err=%d", (int)err);
            return false;
        }

        // Variable packet length: frame size travels in wire byte 0.
        if ((err = radio.variablePacketLengthMode(255)) != RADIOLIB_ERR_NONE) {
            LOG_E("Radio", "init: variablePacketLengthMode failed err=%d", (int)err);
            return false;
        }
        if ((err = radio.setCrcFiltering(true)) != RADIOLIB_ERR_NONE) {
            LOG_E("Radio", "init: setCrcFiltering failed err=%d", (int)err);
            return false;
        }
        if ((err = applyProfile(_profile)) != RADIOLIB_ERR_NONE) {
            LOG_E("Radio", "init: applyProfile failed err=%d", (int)err);
            return false;
        }

        // RadioLib only prepends/strips the L2 byte while filtering is
        // enabled — leaving it off shifts frames by one byte against every
        // peer. No SW fallback; failure here is fatal.
        if ((err = radio.setNodeAddress(_localAddr, 2)) != RADIOLIB_ERR_NONE) {
            LOG_E("Radio", "init: setNodeAddress(0x%02X) failed err=%d",
                  (unsigned)_localAddr, (int)err);
            return false;
        }

        _instance = this;
        // GDO0 fires on packet-end during RX (see _txInFlight below for TX).
        radio.setPacketReceivedAction(&_dispatchIrq);

        // begin() leaves the chip in IDLE; re-arm RX here.
        radio.startReceive();
        _initialized = true;
        return true;
    }

    bool finish() override {
        if (!_initialized) return true;
        radio.clearPacketReceivedAction();
        radio.sleep();
        _instance = nullptr;
        _initialized = false;
        return true;
    }

    void setIrqCallback(void (*cb)(void*), void* ctx) override {
        _irqCb = cb;
        _irqCtx = ctx;
    }

    // FSK has no CAD — pure RSSI sweep is standard. Samples getLiveRssiDbm(),
    // NOT radio.getRSSI(): CC1101's getRSSI() returns the last packet's RSSI,
    // frozen ≈-74 dBm before any packet arrives — above the default -85 dBm
    // threshold, which would wedge CCA in BUSY forever.
    CcaResult cca(uint32_t timeoutMs, int8_t rssiThresholdDbm) override {
        if (!_initialized) return CcaResult::CLEAR;
        // An unread or in-flight frame would be destroyed by strobing IDLE
        // here — report BUSY untouched.
        if (_rxFlag || isReceivingFrame())
            return CcaResult::BUSY;
        // Chip is already in RX; every path leaves it armed. Settle delay
        // lets the AGC converge before the first sample.
        _mod->hal->delay(RF_CCA_SETTLE_MS);
        uint32_t remaining = timeoutMs;
        while (remaining > 0) {
            const float rssi = getLiveRssiDbm();
            if (rssi >= (float)rssiThresholdDbm) {
                LOG_D("Radio", "CCA: BUSY (RSSI %.1f dBm >= %d dBm)",
                      rssi, (int)rssiThresholdDbm);
                // Still in RX; leave a possible incoming frame untouched.
                return CcaResult::BUSY;
            }
            uint32_t step = remaining > RF_CCA_SWEEP_STEP_MS
                          ? RF_CCA_SWEEP_STEP_MS : remaining;
            _mod->hal->delay(step);
            remaining -= step;
        }
        LOG_D("Radio", "CCA: CLEAR (< %d dBm)", (int)rssiThresholdDbm);
        // CLEAR: send() will handle the transition next; no standby() needed.
        return CcaResult::CLEAR;
    }

    // Packet engine prepends `addr` as the L2 byte via transmit(); data is
    // the L3 frame per IRadio's contract.
    TxResult send(const uint8_t* data, uint16_t length, uint8_t addr = 0) override {
        if (!_initialized || !data) return TxResult::BAD_FRAME;
        if (length == 0) return TxResult::BAD_FRAME;
        // Variable-length packet engine carries length in one byte; max
        // on-air payload (incl. L2 addr) is 255.
        if ((uint32_t)length + 1u > 255u) return TxResult::BAD_FRAME;

        _txInFlight = true;
        int16_t err = radio.transmit(data, (size_t)length, addr);
        _txInFlight = false;

        TxResult result;
        if (err == RADIOLIB_ERR_NONE) {
            result = TxResult::OK;
        } else {
            LOG_E("Radio", "transmit failed err=%d len=%u addr=0x%02X",
                  (int)err, (unsigned)length, (unsigned)addr);
            result = TxResult::RADIO_ERROR;
        }
        radio.startReceive();
        return result;
    }

    void startReceive() override { if (_initialized) radio.startReceive(); }
    void standby()     override { if (_initialized) radio.standby();    }

    bool readPacket(RadioPacket* outPacket) override {
        if (!_initialized || !outPacket) return false;

        // RadioLib v7.6.0 quirk (issue #1482, RX side unfixed): with HW
        // address filtering on, getPacketLength() over-counts by one byte,
        // corrupting readData()'s CRC check. Workaround: cap at length-1
        // (readData() clamps to min(getPacketLength(), len)). Remove once
        // RadioLib fixes the RX side.
        const size_t fifoLen = radio.getPacketLength(true);
        if (fifoLen < 2u || fifoLen > MAX_PAYLOAD_SIZE + 1u) {
            radio.startReceive();
            return false;
        }
        const size_t l3Len = fifoLen - 1u; // RadioLib quirk workaround, see above

        int16_t err = radio.readData(outPacket->data, l3Len);
        outPacket->length = (uint16_t)l3Len;
        // getRSSI() is valid here (unlike cca()) — readData just captured
        // this packet's RSSI.
        outPacket->rssi   = rssiToInt16(radio.getRSSI());
        radio.startReceive();

        // CRC mismatch: FIFO already cleared by the chip. No distinct
        // CRC_ERROR event: GDO0 pulses identically for pass/fail.
        if (err != RADIOLIB_ERR_NONE) {
            if (err != RADIOLIB_ERR_CRC_MISMATCH) {
                LOG_W("Radio", "readData err=%d", (int)err);
            }
            return false;
        }
        return true;
    }

    RadioEvent pollEvent() override {
        if (!_initialized) return {RadioEvent::NONE};
        if (_rxFlag) {
            _rxFlag = false;
            return {RadioEvent::RX_DONE};
        }
        return {RadioEvent::NONE};
    }

    // RadioLib has no getTimeOnAir() for CC1101; compute from bit rate plus
    // framing overhead (preamble 2B + sync 2B + length 1B + addr 1B + CRC 2B).
    // Round up: 0 ms reads as "unknown" to Engine.
    uint32_t getAirtimeMs(uint16_t frameLen) const override {
        if (_bitRateKbps <= 0.0f) return 0;
        const float bits = (float)((uint32_t)frameLen + 8u) * 8.0f;
        const float msF  = bits / _bitRateKbps;
        uint32_t ms = (uint32_t)msF;
        if ((float)ms < msF) ms++;
        return ms;
    }

    uint16_t getMaxPayloadSize() const override { return 255; }

    // `numBroadcastAddrs=2` makes both 0x00 and 0xFF broadcast — 0xFF is
    // ADDR_BROADCAST; 0x00 is a chip side effect (unicasts to address 0x00
    // pass every node's HW filter, filtered at L3 instead). Prefer non-zero
    // addresses. Returns true: the HW filter makes Engine's SW filter
    // redundant.
    bool setLocalAddress(uint8_t addr) override {
        _localAddr = addr;
        if (addr == 0x00) {
            LOG_W("Radio", "local address 0x00 doubles as a CC1101 HW broadcast — every "
                  "node wakes for unicasts to this node (L3 still filters); prefer non-zero");
        }
        // Before init(): value is recorded only; init() programs it into
        // the chip.
        if (!_initialized) return true;
        int16_t err = radio.setNodeAddress(addr, 2);
        if (err != RADIOLIB_ERR_NONE) {
            // No SW fallback: filtering stays on with the old address —
            // framing stays consistent, but the new address won't receive
            // until this succeeds.
            LOG_E("Radio", "setNodeAddress(0x%02X) failed err=%d — chip keeps previous "
                  "address; retry setLocalAddress()",
                  (unsigned)addr, (int)err);
            return false;
        }
        return true;
    }

    int16_t setTransmitProfile(RfProfile profile) {
        _profile = profile;
        return applyProfile(profile);
    }

protected:
    // Live channel RSSI — see cca() for why radio.getRSSI() won't work.
    // Pure virtual: getting this wrong silently breaks LBT.
    virtual float getLiveRssiDbm() = 0;

    // True while a frame is mid-receive; lets cca() report BUSY without
    // disturbing it. Default false is safe — the RSSI sweep still catches
    // the carrier.
    virtual bool isReceivingFrame() { return false; }

    // Profiles for any CC1101-class chip; override if a chip has a
    // narrower RxBW/fdev range.
    virtual int16_t applyProfile(RfProfile p) {
        int16_t err;
        switch (p) {
            case RfProfile::HIGH_SPEED:
                if ((err = radio.setBitRate(250.0f))            != RADIOLIB_ERR_NONE) return err;
                if ((err = radio.setFrequencyDeviation(127.0f)) != RADIOLIB_ERR_NONE) return err;
                if ((err = radio.setRxBandwidth(270.0f))        != RADIOLIB_ERR_NONE) return err;
                _bitRateKbps = 250.0f;
                LOG_I("Radio", "profile HIGH_SPEED: 250 kbps fdev=127 kHz BW=270 kHz");
                return RADIOLIB_ERR_NONE;
            case RfProfile::LONG_RANGE:
                if ((err = radio.setBitRate(1.2f))              != RADIOLIB_ERR_NONE) return err;
                if ((err = radio.setFrequencyDeviation(5.2f))   != RADIOLIB_ERR_NONE) return err;
                if ((err = radio.setRxBandwidth(58.0f))         != RADIOLIB_ERR_NONE) return err;
                _bitRateKbps = 1.2f;
                LOG_I("Radio", "profile LONG_RANGE: 1.2 kbps fdev=5.2 kHz BW=58 kHz");
                return RADIOLIB_ERR_NONE;
            default:
                if ((err = radio.setBitRate(38.4f))             != RADIOLIB_ERR_NONE) return err;
                if ((err = radio.setFrequencyDeviation(20.0f))  != RADIOLIB_ERR_NONE) return err;
                if ((err = radio.setRxBandwidth(102.0f))        != RADIOLIB_ERR_NONE) return err;
                _bitRateKbps = 38.4f;
                LOG_I("Radio", "profile NORMAL: 38.4 kbps fdev=20 kHz BW=102 kHz");
                return RADIOLIB_ERR_NONE;
        }
    }

    TRadio    radio;
    ::Module* _mod;

    // Set during transmit(); the trampoline drops the chip's
    // sync-word-sent edge on TX.
    volatile bool _txInFlight = false;
    float         _bitRateKbps = 0.0f;

private:
    static void _dispatchIrq() {
        if (!_instance) return;
        if (_instance->_txInFlight) return;
        _instance->_rxFlag = true;
        if (_instance->_irqCb) _instance->_irqCb(_instance->_irqCtx);
    }

    // One ISR slot per template instantiation — two backends of the same
    // TRadio can't coexist.
    static RadioLibFSKRadio<TRadio>* _instance;

    void (*_irqCb)(void*) = nullptr;
    void*  _irqCtx        = nullptr;
    bool   _initialized   = false;
    volatile bool _rxFlag = false;
    uint8_t _localAddr    = 0xFFu; // broadcast-only until setLocalAddress()
    RfProfile _profile;
};

template <typename TRadio>
RadioLibFSKRadio<TRadio>* RadioLibFSKRadio<TRadio>::_instance = nullptr;

#endif // NATIVE_BUILD
