// Driving model B: cooperative poll() — no background task at all.
//
// Use this when you don't want (or can't afford) a second RTOS task, or on a
// target with no RTOS at all (see ../advanced/custom_osal.cpp). poll() services
// one batch of radio events, TX requests, ACK/forward retries and timeouts
// per call — RX delivery and ACK latency are bounded entirely by how often
// you call it. Never put a blocking delay() before it in loop().
//
// Mutually exclusive with startWorkerTask() — never call both.
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
    // No startWorkerTask() call — poll() below is the only thing driving it.
}

void loop() {
    // Must run every iteration, before any blocking work.
    node.poll();

    static uint32_t lastSend = 0;
    uint32_t now = millis();
    if (now - lastSend >= 3000) {
        lastSend = now;
        (void)node.send(DST_ADDR, "hello");  // fire-and-forget demo; see send.cpp for SendStatus handling
    }
    // No delay() here — a blocking delay before the next poll() call directly
    // adds to RX/ACK latency. If you need periodic work, gate it on millis()
    // like above instead of delay().
}
