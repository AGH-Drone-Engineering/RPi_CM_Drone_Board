// Mesh routing — flooding with RSSI-based suppression.
//
// Plain flooding (../mesh/flooding.cpp) has every node re-broadcast every
// unseen frame. In a dense mesh that wastes airtime: if you heard the
// previous hop very strongly, your neighbours almost certainly heard it too,
// so your retransmission doesn't extend coverage — only edge nodes (weak
// RSSI from the sender) actually need to keep flooding.
//
// ManagedFloodingStrategy implements exactly that: forward unconditionally
// unless the last-hop RSSI is above your threshold. Unknown RSSI
// (RF_RSSI_UNKNOWN, a backend that doesn't report it) is always treated as
// weak → forward, so this degrades safely to plain flooding on radios
// without RSSI.
//
// Target: ESP32 + SX1262

#include <RFNode.h>

#define MY_ADDR  0x01
#define DST_ADDR 0x03

#define LORA_CS   41
#define LORA_IRQ  39
#define LORA_RST  42
#define LORA_BUSY 40

static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

// Above -60 dBm: "I heard that very clearly, my neighbours probably did too"
// — skip the re-broadcast. Tune per deployment density; there's no universal
// right value, only a trade-off between coverage and airtime.
static ManagedFloodingConfig floodCfg = []() {
    ManagedFloodingConfig c;
    c.suppressIfRssiAbove = -60;
    return c;
}();
static ManagedFloodingStrategy routing(floodCfg);

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr = MY_ADDR;
    c.mode = PacketMode::Mesh;

    c.mesh.hopCount = 3;
    c.mesh.routing  = &routing;   // overrides the built-in default instance

    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.printf("[RX] from=0x%02X rssi=%d  \"%.*s\"\n",
                  info.from, info.rssi, (int)len, data);
}

void setup() {
    Serial.begin(115200);
    node.onReceive(onReceive);

    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    node.startWorkerTask(4096, 5);
}

void loop() {
    (void)node.sendAck(DST_ADDR, "hello mesh");  // demo; see send.cpp for SendStatus handling
    delay(5000);
}
