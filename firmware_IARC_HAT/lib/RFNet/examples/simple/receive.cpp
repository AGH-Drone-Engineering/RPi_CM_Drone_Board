// Minimal receiver: print every incoming message.
//
// Target: ESP32 + SX1262

#include <RFNode.h>

#define MY_ADDR 0x02

#define LORA_CS   41
#define LORA_IRQ  39
#define LORA_RST  42
#define LORA_BUSY 40

static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr = MY_ADDR;
    c.mode = PacketMode::P2P;
    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.printf("[RX] from=0x%02X  \"%.*s\"\n", info.from, (int)len, data);
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
    // Nothing to do here — the worker task drives RX in the background.
    delay(1000);
}
