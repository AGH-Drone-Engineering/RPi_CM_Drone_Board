// Mesh — per-message hop count override.
//
// cfg.mesh.hopCount sets the DEFAULT hop budget for every message a node
// originates (0 = no forwarding at all, i.e. P2P behaviour even in Mesh
// mode; 1-7 = max intermediate hops, 3-bit wire field). Override it per
// message via SendOptions.hops when some traffic needs a different reach
// than the rest — e.g. a "nodes still alive?" probe that should only ever
// be answered by direct neighbours, sent alongside normal multi-hop traffic.
//
// hopCount decrements by one at every forwarding node and a frame with
// hopCount==0 is never forwarded further — it only reaches whoever picks it
// up within that many hops, unicast or broadcast.
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

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr = MY_ADDR;
    c.mode = PacketMode::Mesh;

    // Default reach for anything that doesn't ask for a different hopCount.
    c.mesh.hopCount = 3;

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
    // Normal traffic — uses the default hopCount (3) from cfg.
    (void)node.sendAck(DST_ADDR, "normal multi-hop message");  // demo; see send.cpp for SendStatus handling
    delay(3000);

    // Limited-reach send — hops is a forward BUDGET, not a "direct only" flag:
    // hops=1 still allows ONE forward (a direct neighbour decrements 1→0 and
    // relays it), so the frame reaches up to ~2 radio hops. For a strictly
    // direct-neighbour-only probe use hops=0 (shown below), which forbids
    // forwarding entirely.
    SendOptions oneHop;
    oneHop.requireAck   = true;
    oneHop.hops         = 1;
    (void)node.send(DST_ADDR, "reachable within one forward?", oneHop);  // demo
    delay(5000);

    // hops=0 works in Mesh mode too — behaves like plain P2P for this one
    // message: no forwarding at all, even though the node's default is 3.
    SendOptions noForward;
    noForward.hops = 0;
    (void)node.send(DST_ADDR, "local-only, never forwarded", noForward);  // demo
    delay(5000);
}
