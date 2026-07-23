#pragma once
#ifndef NATIVE_BUILD
#include <RadioLib.h>
#include "../RadioLibFSKRadio.h"

class ArduinoHal;

class CC1101FSKRadio : public RadioLibFSKRadio<::CC1101> {
public:
    // Adding a region/sub-band: add an enum entry plus a matching row in
    // the .cpp's kChannelTable[] (indexed directly by the enum cast).
    //
    // CC1101 supports three frequency bands: 300–348, 387–464 and
    // 779–928 MHz. Channels outside those windows return UNSUPPORTED.
    enum class Channel : uint8_t {
        EU868_CH0,   // 868.1 MHz, 1% DC (ETSI h1.4)
        EU868_CH1,   // 868.3 MHz, 1%
        EU868_CH2,   // 868.5 MHz, 1%
        EU869_DC10,  // 869.525 MHz, 10% (ETSI h1.5)
        EU433_CH0,   // 433.05 MHz, 10%
        US915_CH0,   // 915.2 MHz, FCC Part 15, no DC (dwell limit instead)
    };

    struct ChannelInfo {
        uint32_t centerHz;
        // 100 = 1%, 10 = 10%, 0 = no percentage duty-cycle limit (US915 still
        // has an FCC dwell-time limit this driver doesn't track — see .cpp).
        uint16_t dutyCycleDenominator;
        // See kChannelTable in the .cpp for entries exceeding the CC1101's
        // own +10 dBm hard limit.
        int8_t   maxTxPowerDbm;
    };

    enum class ChannelResult : int8_t { OK, UNSUPPORTED, RADIO_ERROR };

    // gdo2 (optional, -1 to omit): lets transmit() poll TX-complete directly.
    // Without it, transmit() falls back to a slower, less reliable timeout wait.
    CC1101FSKRadio(Channel channel,
                   uint32_t cs, uint32_t gdo0,
                   uint32_t rst = -1, uint32_t gdo2 = -1,
                   RfProfile profile = RfProfile::NORMAL);

    ~CC1101FSKRadio() override;

    bool init() override;

    uint16_t getRegulatoryDutyDenominator() const override {
        return _channelInfo->dutyCycleDenominator;
    }

    // Switches frequency; clamps TX power to the channel's regulatory ceiling.
    // Restart the duty tracker (or call before Engine::begin()) to pick up the new denominator.
    ChannelResult selectChannel(Channel ch, ChannelInfo* outInfo = nullptr);

protected:
    // Reads the RSSI status register directly — RadioLib's getRSSI()
    // returns the last packet's RSSI, not the live channel level.
    float getLiveRssiDbm() override;

    // PKTSTATUS.SFD: sync word found, end of packet not yet reached.
    bool isReceivingFrame() override;

private:
    static const ChannelInfo kChannelTable[];
    static constexpr size_t  kChannelTableSize = 6;

    // Shared delegate for the public constructors; takes ownership of `hal`.
    CC1101FSKRadio(Channel channel, ArduinoHal* hal,
                   uint32_t cs, uint32_t gdo0, uint32_t rst, uint32_t gdo2,
                   RfProfile profile);

    Channel            _channel;
    const ChannelInfo* _channelInfo;
    // ::Module never takes ownership of the HAL, so we free it ourselves.
    ArduinoHal*        _ownedHal;
};
#endif // NATIVE_BUILD
