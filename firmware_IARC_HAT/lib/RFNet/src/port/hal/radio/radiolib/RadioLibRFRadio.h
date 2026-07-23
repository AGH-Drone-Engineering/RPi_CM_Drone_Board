#pragma once
// Target-only header: pulls RadioLib (Arduino SPI/GPIO); excluded from
// native builds via the platformio src filter.
#ifndef NATIVE_BUILD
#include <utility>
#include <string.h>
#include <RadioLib.h>
#include "core/RFConfig.h"
#include "port/hal/radio/IRadio.h"
#include "port/Logger.h"

// Generic scaffolding for RadioLib's SX12xx-family API. Implements IRadio's
// L2 framing contract (see IRadio.h) via buffer prepend/strip and a default
// RSSI-only cca(). LoRa specifics (SF/BW/CR, CAD) added one layer down in
// RadioLibLoRaRadio.h.
//
// Subclass responsibilities:
//   - Construct ::Module with the platform RadioLibHal + pin assignments
//   - Override attachChipIrq() with the chip's DIO action setter
//     (setDio1Action on SX1262, setDio0Action on SX1276, etc.)
//   - Override init() for chip-specific config (channel/frequency,
//     modulation parameters)
template <typename TRadio>
class RadioLibRFRadio : public IRadio
{
public:
    explicit RadioLibRFRadio(::Module *mod)
        : radio(mod), _mod(mod) {}

    ~RadioLibRFRadio() override
    {
        finish();
        delete _mod;
    }

    bool init() override
    {
        if (_initialized)
            return true;
        int16_t err = radio.begin();
        if (err != RADIOLIB_ERR_NONE) {
            LOG_E("Radio", "begin() failed err=%d", (int)err);
            return false;
        }
        _instance = this;
        attachChipIrq(&_dispatchIrq);
        _initialized = true;
        return true;
    }

    bool finish() override
    {
        if (!_initialized)
            return true;
        // SX12xx family has no end(); sleep() is the documented power-down.
        radio.sleep();
        _instance = nullptr;
        _initialized = false;
        return true;
    }

    void setIrqCallback(void (*cb)(void *), void *ctx) override
    {
        _irqCb = cb;
        _irqCtx = ctx;
    }

    // Default modulation-agnostic CCA: RSSI sweep only. LoRa subclasses
    // override to add a CAD probe for weak preambles below the RSSI floor.
    CcaResult cca(uint32_t timeoutMs, int8_t rssiThresholdDbm) override
    {
        if (!_initialized)
            return CcaResult::CLEAR;
        // RX may be mid-flight or holding an unread frame; restarting it
        // here would destroy it.
        if (_rxInProgress())
            return CcaResult::BUSY;
        return _rssiSweep(timeoutMs, (float)rssiThresholdDbm,
                          /*rearmFirst=*/false);
    }

    // Blocking TX. Prepends the L2 dst byte; no CCA inside (caller does
    // cca() first). Always re-arms RX after TX.
    TxResult send(const uint8_t *data, uint16_t length, uint8_t addr = 0) override
    {
        if (!_initialized || !data)
            return TxResult::BAD_FRAME;
        if (length == 0)
            return TxResult::BAD_FRAME;
        // +1 for the L2 byte. Bounded by the chip's frame limit, not just
        // the buffer — oversized frames fail here as BAD_FRAME, not a
        // confusing RADIO_ERROR from transmit().
        uint32_t wireMax = MAX_PAYLOAD_SIZE;
        const uint16_t chipMax = getMaxPayloadSize();
        if (chipMax != 0 && chipMax < wireMax)
            wireMax = chipMax;
        if ((uint32_t)length + 1u > wireMax)
            return TxResult::BAD_FRAME;

        _txBuf[0] = addr;
        memcpy(_txBuf + 1, data, length);

        // Chip pulses TX_DONE at the end of our own transmit; swallow it
        // (see _txInFlight below).
        _txInFlight = true;
        int16_t err = radio.transmit(_txBuf, (size_t)length + 1u);
        _txInFlight = false;
        TxResult result;
        if (err == RADIOLIB_ERR_NONE)
        {
            result = TxResult::OK;
        }
        else
        {
            LOG_E("Radio", "transmit failed err=%d len=%u addr=0x%02X",
                  (int)err, (unsigned)length + 1u, (unsigned)addr);
            result = TxResult::RADIO_ERROR;
        }
        radio.startReceive();
        return result;
    }

    void startReceive() override
    {
        if (_initialized)
            radio.startReceive();
    }
    void standby() override
    {
        if (_initialized)
            radio.standby();
    }

    bool readPacket(RadioPacket *outPacket) override
    {
        if (!_initialized || !outPacket)
            return false;

        const uint16_t wireLen = (uint16_t)radio.getPacketLength();
        // Frame must contain at least the L2 byte. Anything shorter is junk.
        if (wireLen < 1 || wireLen > MAX_PAYLOAD_SIZE)
        {
            radio.startReceive();
            return false;
        }

        int16_t err = radio.readData(_txBuf, wireLen);
        radio.startReceive();
        if (err != RADIOLIB_ERR_NONE) {
            LOG_W("Radio", "readData failed err=%d len=%u", (int)err, (unsigned)wireLen);
            return false;
        }

        // SW L2 filter — energy-saving early reject; Engine still re-checks
        // via L3.dst.
        const uint8_t l2dst = _txBuf[0];
        if (l2dst != _localAddr && l2dst != /*ADDR_BROADCAST*/ 0xFFu)
        {
            return false;
        }

        const uint16_t l3Len = wireLen - 1u;
        memcpy(outPacket->data, _txBuf + 1, l3Len);
        outPacket->length = l3Len;
        outPacket->rssi = rssiToInt16(radio.getRSSI(true));
        return true;
    }

    RadioEvent pollEvent() override
    {
        if (!_initialized)
            return {RadioEvent::NONE};
        const uint32_t flags = radio.getIrqFlags();
        if (flags & (1UL << RADIOLIB_IRQ_RX_DONE))
        {
            if (flags & (1UL << RADIOLIB_IRQ_CRC_ERR))
            {
                radio.clearIrqFlags((1UL << RADIOLIB_IRQ_RX_DONE) |
                                    (1UL << RADIOLIB_IRQ_CRC_ERR));
                // Explicit re-arm: this path skips readPacket(), which
                // normally does it.
                radio.startReceive();
                return {RadioEvent::CRC_ERROR};
            }
            radio.clearIrqFlags(1UL << RADIOLIB_IRQ_RX_DONE);
            return {RadioEvent::RX_DONE};
        }
        if (flags & (1UL << RADIOLIB_IRQ_TX_DONE))
        {
            radio.clearIrqFlags(1UL << RADIOLIB_IRQ_TX_DONE);
            return {RadioEvent::TX_DONE};
        }
        return {RadioEvent::NONE};
    }

    // +1 for the L2 byte. Round up: 0 ms reads as "airtime unknown" to
    // Engine.
    uint32_t getAirtimeMs(uint16_t frameLen) const override
    {
        const uint32_t us =
            (uint32_t)const_cast<TRadio &>(radio).getTimeOnAir((size_t)frameLen + 1u);
        return (us + 999u) / 1000u;
    }

    // Physical per-frame limit on SX126x/SX127x; override if a chip differs
    // (e.g. some LR11xx modes cap lower).
    uint16_t getMaxPayloadSize() const override { return 255; }

    bool setLocalAddress(uint8_t addr) override
    {
        _localAddr = addr;
        return false; // SW filter only (FSK backends' HW filter returns true)
    }

protected:
    // Hook for chip-specific IRQ pin wiring. Implementations call e.g.
    //   radio.setDio1Action(isr);   // SX1262
    //   radio.setDio0Action(isr);   // SX1276
    virtual void attachChipIrq(void (*isr)()) = 0;

    // True while a frame is mid-receive (HEADER_VALID) or unread (RX_DONE
    // latched). Cleared by the next startReceive() — always called by
    // readPacket() and the CRC_ERROR path.
    bool _rxInProgress()
    {
        const uint32_t flags = radio.getIrqFlags();
        return (flags & ((1UL << RADIOLIB_IRQ_HEADER_VALID) |
                         (1UL << RADIOLIB_IRQ_RX_DONE))) != 0;
    }

    // Shared RSSI sweep for this layer's cca() and the LoRa CAD-then-RSSI
    // path. Returns BUSY on the first sample over threshold.
    //
    // rearmFirst: LoRa's CAD probe leaves the chip in standby; the base
    // path is already in RX and must not restart it.
    CcaResult _rssiSweep(uint32_t timeoutMs, float rssiThresholdDbm,
                         bool rearmFirst = true)
    {
        if (rearmFirst)
            radio.startReceive();
        auto *hal = _mod->hal;
        hal->delay(RF_CCA_SETTLE_MS);  // let AGC settle before sampling
        uint32_t remaining = timeoutMs;
        while (remaining > 0)
        {
            if (radio.getRSSI(false) >= rssiThresholdDbm)
            {
                // Already in RX — don't restart; may be an incoming frame
                // mid-air.
                return CcaResult::BUSY;
            }
            uint32_t step = remaining > RF_CCA_SWEEP_STEP_MS
                          ? RF_CCA_SWEEP_STEP_MS : remaining;
            hal->delay(step);
            remaining -= step;
        }
        // send() will handle the state transition next; no standby() needed.
        return CcaResult::CLEAR;
    }

    TRadio radio;
    ::Module *_mod;

    // True during a CAD probe (toggles DIO); the ISR trampoline swallows
    // that edge. Unused by a pure RSSI sweep.
    volatile bool _ccaInFlight = false;

    // True during transmit(); trampoline swallows our own TX_DONE edge.
    volatile bool _txInFlight = false;

private:
    // ISR trampoline; swallows the edge during our own CCA probes/TX.
    static void _dispatchIrq()
    {
        if (!_instance)
            return;
        if (_instance->_ccaInFlight || _instance->_txInFlight)
            return;
        if (_instance->_irqCb)
            _instance->_irqCb(_instance->_irqCtx);
    }

    // One ISR slot per template instantiation — two backends sharing the
    // same TRadio can't coexist.
    static RadioLibRFRadio<TRadio> *_instance;

    void (*_irqCb)(void *) = nullptr;
    void *_irqCtx = nullptr;
    bool _initialized = false;
    uint8_t _localAddr = 0xFFu; // broadcast-only until setLocalAddress()

    // TX/RX staging buffer, sized to fit the largest fragment plus its L2
    // byte.
    uint8_t _txBuf[MAX_PAYLOAD_SIZE];
};

template <typename TRadio>
RadioLibRFRadio<TRadio> *RadioLibRFRadio<TRadio>::_instance = nullptr;

#endif // NATIVE_BUILD
