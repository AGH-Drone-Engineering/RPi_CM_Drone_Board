// Bare-metal Arduino node (AVR / ARM, no RTOS) with BaremetalOsal.
//
// BaremetalOsal is the DefaultOsal on non-ESP Arduino, and on Arduino it
// wires millis() up as its tick source automatically — so the plain two-arg
// RFNode(radio, cfg) constructor is safe here. It's injected explicitly
// below only for clarity.
// (On a NON-Arduino bare-metal target the tick source is a required
// constructor argument instead: BaremetalOsal osal(myTickMs); — forgetting
// it is a compile error, not a node that never times out.)
//
// One hard requirement that doesn't apply on ESP32:
//   There is no scheduler to run a worker task on: startWorkerTask()
//   always returns false here. Drive the node with poll() from loop()
//   instead — see ../simple/poll_driven.cpp for the general pattern this
//   follows; the only difference is which OSAL is underneath.
//
// Target: Arduino AVR/ARM + SX1262 (non-ESP)

#include <RFNode.h>

#define MY_ADDR  0x01
#define DST_ADDR 0x02

#define LORA_CS   10
#define LORA_IRQ   2
#define LORA_RST   9
#define LORA_BUSY  3

static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

// Ticks come from millis() automatically. To use a better clock (e.g. a
// calibrated RTC), pass it to the constructor: BaremetalOsal osal(myTickMs);
static BaremetalOsal osal;

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr = MY_ADDR;
    c.mode = PacketMode::P2P;
    return c;
}();

static RFNode node(radio, osal, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.print("[RX] ");
    Serial.write(data, len);
    Serial.println();
}

void setup() {
    Serial.begin(115200);

    node.onReceive(onReceive);

    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    // No startWorkerTask() call — see the requirement above. poll() in
    // loop() is the only thing driving the node.
}

void loop() {
    // Must run every iteration, before any blocking delay().
    node.poll();

    static uint32_t lastSend = 0;
    uint32_t now = millis();
    if (now - lastSend >= 5000) {
        lastSend = now;
        (void)node.send(DST_ADDR, "hello");  // fire-and-forget demo; see send.cpp for SendStatus handling
    }
}
