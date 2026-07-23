// Custom IClock — wall-clock time from a source other than SNTP.
//
// The duty-cycle tracker (see ../advanced/duty_cycle.cpp) uses IClock to survive
// reboots: it stores its Toff off-time deadline in epoch-ms, so on resume it
// knows how much off-time remains and keeps waiting. Built-in backends: EspClock
// (SNTP), PosixClock (host tests), ZephyrClock, NullClock (always invalid — safe
// no-op fallback).
//
// Write your own when time comes from somewhere else: an external RTC chip over
// I2C/SPI, a GPS fix, a cellular modem's network time. Only two methods are
// required, and isValid()==false is ALWAYS a safe answer — it just means the
// tracker falls back to a tick-based deadline that resets on reboot, never an
// unsafe one.
//
// Target: ESP32 + SX1262

#include <RFNode.h>

#define MY_ADDR  0x01
#define DST_ADDR 0x02

#define LORA_CS   41
#define LORA_IRQ  39
#define LORA_RST  42
#define LORA_BUSY 40

static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

// Stub wrapping a fictional I2C RTC (e.g. DS3231) — replace the body with
// your driver's calls.
class ExternalRtcClock : public IClock {
public:
    // Call once your RTC has been read successfully / set (e.g. right after
    // the first successful I2C transaction at boot, or after a GPS fix).
    void markSynced() { _synced = true; }

    bool isValid() const override { return _synced; }

    uint64_t epochMs() const override {
        if (!_synced) return 0;
        // TODO: replace with your RTC driver, e.g.:
        //   uint32_t epochS = myRtcDriver.readUnixTime();
        //   return (uint64_t)epochS * 1000ULL;
        return 0;
    }

private:
    bool _synced = false;
};

static ExternalRtcClock rtcClock;
static PreferencesBackend nvs;

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr       = MY_ADDR;
    c.mode       = PacketMode::P2P;
    c.clock      = &rtcClock;
    c.nv.backend = &nvs;

    c.dutyCycle.enabled = true;

    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.printf("[RX] from=0x%02X \"%.*s\"\n", info.from, (int)len, data);
}

void setup() {
    Serial.begin(115200);

    // TODO: initialise your RTC driver here, then:
    // if (myRtcDriver.begin() && myRtcDriver.hasValidTime()) {
    //     rtcClock.markSynced();
    // }
    // Until markSynced() is called, isValid() returns false and the Toff
    // deadline is tick-based (lost on reboot) — the node still runs correctly,
    // just without cross-reboot off-time persistence.

    node.onReceive(onReceive);

    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    node.startWorkerTask(4096, 5);
}

void loop() {
    (void)node.sendAck(DST_ADDR, "hello");  // demo; see send.cpp for SendStatus handling
    delay(5000);
}
