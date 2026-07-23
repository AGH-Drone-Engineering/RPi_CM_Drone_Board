// NVS persistence — the sequence counter (used in the GCM nonce) must
// survive reboots, which is what RFNvConfig.backend is for.
//
// RFNode wires a platform default automatically (DefaultNVBackend —
// PreferencesBackend/NVS-flash on ESP32, EepromBackend on plain Arduino,
// volatile RAM with a compile-time #warning on host builds). This example
// shows injecting one explicitly, plus the knobs that control NV wear.
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

// ESP32 Preferences (esp-idf NVS flash), namespace "lora", keys "k<id>".
// Survives deep sleep and power cycles.
static PreferencesBackend nvs;

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr     = MY_ADDR;
    c.mode     = PacketMode::P2P;
    c.security = RFSecurityConfig::FromPassword("my_strong_password");

    c.nv.backend = &nvs;

    // Distinct NVS keys for the seq counter and the duty-cycle state.
    // Only change these if your app ALSO uses the same backend/namespace for
    // something else and the ids would collide.
    c.nv.idSeq  = 1;
    c.nv.idDuty = 2;

    // Flash write every N seq increments, instead of every single one.
    c.nv.nonceCommitStep = 100;

    // Persist the duty-cycle off-time deadline only when it moves by at least
    // this many ms since the last save.
    c.nv.dutyMinCommitMs = 2000;

    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.printf("[RX] from=0x%02X \"%.*s\"\n", info.from, (int)len, data);
}

void setup() {
    Serial.begin(115200);
    node.onReceive(onReceive);

    // begin() opens the NVS namespace and loads (or initialises) the
    // persisted seq counter.
    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    node.startWorkerTask(4096, 5);
}

void loop() {
    (void)node.sendAck(DST_ADDR, "secret message");  // demo; see send.cpp for SendStatus handling
    delay(5000);
}
