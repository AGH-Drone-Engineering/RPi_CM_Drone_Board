#ifndef NATIVE_BUILD
#include "SX1262LoRaRadio.h"
#include <hal/Arduino/ArduinoHal.h>
#include "port/Logger.h"

// Must enumerate every Channel value in declaration order (indexed by the
// enum cast).
//   EU868 g1 (h1.4): 868.0–868.6 MHz, 1% DC, +14 dBm max.
//   EU868 g3 (h1.5): 869.4–869.65 MHz, 10% DC, +27 dBm max.
//   EU433 ISM: 433.05–434.79 MHz, 10% DC, +10 dBm typical.
//   US915 ISM: 902–928 MHz, FCC Part 15 — no % duty-cycle limit; a 400 ms
//     dwell-time limit still applies but is NOT enforced by this driver or
//     anywhere else in RFNet — the integrator must handle it if using US915.
const SX1262LoRaRadio::ChannelInfo SX1262LoRaRadio::kChannelTable[] = {
    /* EU868_CH0  */ { 868100000UL, 100, 14 },
    /* EU868_CH1  */ { 868300000UL, 100, 14 },
    /* EU868_CH2  */ { 868500000UL, 100, 14 },
    /* EU869_DC10 */ { 869525000UL,  10, 27 },
    /* EU433_CH0  */ { 433050000UL,  10, 10 },
    /* US915_CH0  */ { 915200000UL,   0, 30 },
};

SX1262LoRaRadio::SX1262LoRaRadio(Channel channel,
                                 uint32_t cs, uint32_t irq,
                                 uint32_t rst, uint32_t gpio,
                                 RfProfile profile)
    : SX1262LoRaRadio(channel, new ArduinoHal(), cs, irq, rst, gpio, profile)
{}

SX1262LoRaRadio::SX1262LoRaRadio(Channel channel,
                                 uint32_t cs, uint32_t irq,
                                 uint32_t rst, uint32_t gpio,
                                 SPIClass& spi,
                                 RfProfile profile,
                                 SPISettings spiSettings)
    : SX1262LoRaRadio(channel, new ArduinoHal(spi, spiSettings),
                      cs, irq, rst, gpio, profile)
{}

SX1262LoRaRadio::SX1262LoRaRadio(Channel channel, ArduinoHal* hal,
                                 uint32_t cs, uint32_t irq,
                                 uint32_t rst, uint32_t gpio,
                                 RfProfile profile)
    : RadioLibLoRaRadio<::SX1262>(
          new ::Module(hal, cs, irq, rst, gpio), profile),
      _channel(channel),
      _channelInfo(&kChannelTable[static_cast<uint8_t>(channel)]),
      _ownedHal(hal)
{}

SX1262LoRaRadio::~SX1262LoRaRadio() {
    // Power down while the HAL is still alive; the base destructor's later
    // finish() is then a no-op.
    finish();
    delete _ownedHal;
}

bool SX1262LoRaRadio::init() {
    if (!RadioLibLoRaRadio<::SX1262>::init()) {
        return false;  // base already logged the specific RadioLib error code
    }
    if (selectChannel(_channel) != ChannelResult::OK) {
        return false;  // selectChannel logged the specific failure
    }
    LOG_I("SX1262", "init OK, ch=%u freq=%lu Hz", (unsigned)_channel,
          (unsigned long)_channelInfo->centerHz);
    return true;
}

SX1262LoRaRadio::ChannelResult SX1262LoRaRadio::selectChannel(Channel ch, ChannelInfo* outInfo) {
    const uint8_t idx = static_cast<uint8_t>(ch);
    if (idx >= kChannelTableSize) {
        LOG_W("SX1262", "selectChannel: channel %u out of range", (unsigned)idx);
        return ChannelResult::UNSUPPORTED;
    }
    const ChannelInfo* info = &kChannelTable[idx];

    const float mhz = (float)info->centerHz / 1000000.0f;
    int16_t err = radio.setFrequency(mhz);
    if (err != RADIOLIB_ERR_NONE) {
        LOG_W("SX1262", "selectChannel: setFrequency(%.3f MHz) failed err=%d", mhz, (int)err);
        return ChannelResult::RADIO_ERROR;
    }

    // SX1262 hardware range is [-9, 22] dBm; RadioLib rejects anything outside
    // it with an error instead of clamping (e.g. EU869_DC10's table value of
    // 27 dBm), so clamp here first to actually get the "capped, not an error"
    // behavior a channel table entry above 22 dBm implies.
    const int8_t txPowerDbm = (info->maxTxPowerDbm > 22)  ? (int8_t)22
                             : (info->maxTxPowerDbm < -9) ? (int8_t)-9
                             : info->maxTxPowerDbm;
    if ((err = radio.setOutputPower(txPowerDbm)) != RADIOLIB_ERR_NONE) {
        LOG_W("SX1262", "selectChannel: setOutputPower(%d dBm) failed err=%d",
              (int)txPowerDbm, (int)err);
        return ChannelResult::RADIO_ERROR;
    }

    _channel     = ch;
    _channelInfo = info;
    if (outInfo) *outInfo = *info;
    LOG_I("SX1262", "channel %u: %.3f MHz, DC denom=%u, maxPwr=%d dBm",
          (unsigned)ch, (float)info->centerHz / 1000000.0f,
          (unsigned)info->dutyCycleDenominator, (int)txPowerDbm);
    return ChannelResult::OK;
}
#endif // NATIVE_BUILD
