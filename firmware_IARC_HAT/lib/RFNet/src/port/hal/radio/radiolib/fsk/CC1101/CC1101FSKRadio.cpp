#ifndef NATIVE_BUILD
#include "CC1101FSKRadio.h"
#include <hal/Arduino/ArduinoHal.h>
#include "port/Logger.h"

// Must enumerate every Channel value in declaration order (indexed by the
// enum cast).
//   EU868 g1 (h1.4): 868.0–868.6 MHz, 1% DC, +10 dBm (chip max).
//   EU868 g3 (h1.5): 869.4–869.65 MHz, 10% DC — region allows +27 dBm, but
//     the CC1101 caps at +10 dBm; an external PA would be needed for g3.
//   EU433 ISM: 433.05–434.79 MHz, 10% DC, +10 dBm.
//   US915 ISM: 902–928 MHz, FCC Part 15 — no % duty-cycle limit; a 400 ms
//     dwell-time limit still applies but is NOT enforced by this driver or
//     anywhere else in RFNet — the integrator must handle it if using US915.
const CC1101FSKRadio::ChannelInfo CC1101FSKRadio::kChannelTable[] = {
    /* EU868_CH0  */ { 868100000UL, 100, 10 },
    /* EU868_CH1  */ { 868300000UL, 100, 10 },
    /* EU868_CH2  */ { 868500000UL, 100, 10 },
    /* EU869_DC10 */ { 869525000UL,  10, 10 },
    /* EU433_CH0  */ { 433050000UL,  10, 10 },
    /* US915_CH0  */ { 915200000UL,   0, 10 },
};

CC1101FSKRadio::CC1101FSKRadio(Channel channel,
                               uint32_t cs, uint32_t gdo0,
                               uint32_t rst, uint32_t gdo2,
                               RfProfile profile)
    : CC1101FSKRadio(channel, new ArduinoHal(), cs, gdo0, rst, gdo2, profile)
{}

CC1101FSKRadio::CC1101FSKRadio(Channel channel, ArduinoHal* hal,
                               uint32_t cs, uint32_t gdo0,
                               uint32_t rst, uint32_t gdo2,
                               RfProfile profile)
    : RadioLibFSKRadio<::CC1101>(
          new ::Module(hal, cs, gdo0, rst, gdo2), profile),
      _channel(channel),
      _channelInfo(&kChannelTable[static_cast<uint8_t>(channel)]),
      _ownedHal(hal)
{}

CC1101FSKRadio::~CC1101FSKRadio() {
    // Power down while the HAL is still alive; the base destructor's later
    // finish() is then a no-op.
    finish();
    delete _ownedHal;
}

float CC1101FSKRadio::getLiveRssiDbm() {
    // Status registers need the burst bit (CMD_ACCESS_STATUS_REG) — same
    // mapping RadioLib's own private SPI helpers use. Only meaningful while chip is in RX.
    const uint8_t raw = _mod->SPIreadRegister(RADIOLIB_CC1101_REG_RSSI |
                                              RADIOLIB_CC1101_CMD_ACCESS_STATUS_REG);
    // Datasheet §17.3: signed byte in 0.5 dB steps with a 74 dB offset.
    return (raw >= 128) ? (((float)raw - 256.0f) / 2.0f) - 74.0f
                        : (((float)raw) / 2.0f) - 74.0f;
}

bool CC1101FSKRadio::isReceivingFrame() {
    const uint8_t pktStatus = _mod->SPIreadRegister(RADIOLIB_CC1101_REG_PKTSTATUS |
                                                    RADIOLIB_CC1101_CMD_ACCESS_STATUS_REG);
    return (pktStatus & 0x08u) != 0; // bit 3 = SFD
}

bool CC1101FSKRadio::init() {
    if (!RadioLibFSKRadio<::CC1101>::init()) {
        return false;  // base already logged the specific RadioLib error code
    }
    if (selectChannel(_channel) != ChannelResult::OK) {
        return false;  // selectChannel logged the specific failure
    }
    LOG_I("CC1101", "init OK, ch=%u freq=%lu Hz", (unsigned)_channel,
          (unsigned long)_channelInfo->centerHz);
    return true;
}

namespace {
// Nearest entry in the CC1101's discrete power ladder. RadioLib's
// setOutputPower() requires an exact match against this ladder and errors on
// anything else, so callers must snap to it themselves first.
int8_t snapToCC1101PowerLadder(int8_t dBm) {
    static const int8_t kLadder[] = { -30, -20, -15, -10, 0, 5, 7, 10 };
    int8_t best     = kLadder[0];
    int    bestDist = (int)dBm - (int)kLadder[0];
    if (bestDist < 0) bestDist = -bestDist;
    for (size_t i = 1; i < sizeof(kLadder) / sizeof(kLadder[0]); ++i) {
        int dist = (int)dBm - (int)kLadder[i];
        if (dist < 0) dist = -dist;
        if (dist < bestDist) { bestDist = dist; best = kLadder[i]; }
    }
    return best;
}
} // namespace

CC1101FSKRadio::ChannelResult CC1101FSKRadio::selectChannel(Channel ch, ChannelInfo* outInfo) {
    const uint8_t idx = static_cast<uint8_t>(ch);
    if (idx >= kChannelTableSize) {
        LOG_W("CC1101", "selectChannel: channel %u out of range", (unsigned)idx);
        return ChannelResult::UNSUPPORTED;
    }
    const ChannelInfo* info = &kChannelTable[idx];

    const float mhz = (float)info->centerHz / 1000000.0f;
    int16_t err = radio.setFrequency(mhz);
    if (err == RADIOLIB_ERR_INVALID_FREQUENCY) {
        LOG_W("CC1101", "selectChannel: %.3f MHz not supported by this chip", mhz);
        return ChannelResult::UNSUPPORTED;
    }
    if (err != RADIOLIB_ERR_NONE) {
        LOG_W("CC1101", "selectChannel: setFrequency(%.3f MHz) failed err=%d", mhz, (int)err);
        return ChannelResult::RADIO_ERROR;
    }

    // Snap to the nearest entry in the discrete power ladder
    // {-30,-20,-15,-10,0,5,7,10} dBm; RadioLib requires an exact match and
    // errors on anything else (values above +10 dBm land on +10 dBm here).
    const int8_t txPowerDbm = snapToCC1101PowerLadder(info->maxTxPowerDbm);
    if ((err = radio.setOutputPower(txPowerDbm)) != RADIOLIB_ERR_NONE) {
        LOG_W("CC1101", "selectChannel: setOutputPower(%d dBm) failed err=%d",
              (int)txPowerDbm, (int)err);
        return ChannelResult::RADIO_ERROR;
    }

    _channel     = ch;
    _channelInfo = info;
    if (outInfo) *outInfo = *info;
    LOG_I("CC1101", "channel %u: %.3f MHz, DC denom=%u, maxPwr=%d dBm",
          (unsigned)ch, (float)info->centerHz / 1000000.0f,
          (unsigned)info->dutyCycleDenominator, (int)txPowerDbm);
    return ChannelResult::OK;
}
#endif // NATIVE_BUILD
