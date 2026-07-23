#pragma once
#ifndef NATIVE_BUILD
#include <RadioLib.h>
#include <SPI.h>
#include "../RadioLibLoRaRadio.h"

#if !(defined(ESP32) || defined(ESP8266))
    #define IRAM_ATTR
#endif

class ArduinoHal;

class SX1262LoRaRadio : public RadioLibLoRaRadio<::SX1262> {
public:
    // Adding a region/sub-band: add an enum entry plus a matching row in
    // the .cpp's kChannelTable[] (indexed directly by the enum cast).
    enum class Channel : uint8_t {
        EU868_CH0,   // 868.1 MHz,   1% DC (ETSI h1.4)
        EU868_CH1,   // 868.3 MHz,   1%
        EU868_CH2,   // 868.5 MHz,   1%
        EU869_DC10,  // 869.525 MHz, 10% (ETSI h1.5, allows 27 dBm)
        EU433_CH0,   // 433.05 MHz,  10%
        US915_CH0,   // 915.2 MHz,   FCC Part 15, no DC (dwell-time limit instead)
    };

    struct ChannelInfo {
        uint32_t centerHz;
        // 100 = 1%, 10 = 10%, 0 = no percentage duty-cycle limit (US915 still
        // has an FCC dwell-time limit this driver doesn't track — see .cpp).
        uint16_t dutyCycleDenominator;
        int8_t   maxTxPowerDbm;
    };

    enum class ChannelResult : int8_t { OK, UNSUPPORTED, RADIO_ERROR };

    SX1262LoRaRadio(Channel channel,
                    uint32_t cs, uint32_t irq,
                    uint32_t rst = -1, uint32_t gpio = -1,
                    RfProfile profile = RfProfile::NORMAL);

    // Overload for a custom SPI bus (e.g. non-default SPIClass with its own pin mapping) instead of board's default.
    SX1262LoRaRadio(Channel channel,
                    uint32_t cs, uint32_t irq,
                    uint32_t rst, uint32_t gpio,
                    SPIClass& spi,
                    RfProfile profile = RfProfile::NORMAL,
                    SPISettings spiSettings = RADIOLIB_DEFAULT_SPI_SETTINGS);

    ~SX1262LoRaRadio() override;

    bool init() override;

    uint16_t getRegulatoryDutyDenominator() const override {
        return _channelInfo->dutyCycleDenominator;
    }

    // Switches frequency; clamps TX power to the channel's regulatory ceiling.
    // Restart the duty tracker (or call before Engine::begin()) to pick up the new denominator.
    ChannelResult selectChannel(Channel ch, ChannelInfo* outInfo = nullptr);

protected:
    void attachChipIrq(void (*isr)()) override { radio.setDio1Action(isr); }

private:
    static const ChannelInfo kChannelTable[];
    static constexpr size_t  kChannelTableSize = 6;

    // Shared delegate for the public constructors; takes ownership of `hal`.
    SX1262LoRaRadio(Channel channel, ArduinoHal* hal,
                    uint32_t cs, uint32_t irq, uint32_t rst, uint32_t gpio,
                    RfProfile profile);

    Channel            _channel;
    const ChannelInfo* _channelInfo;
    // ::Module never takes ownership of the HAL, so we free it ourselves.
    ArduinoHal*        _ownedHal;
};
#endif // NATIVE_BUILD
