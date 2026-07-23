// Mesh routing — static table for a known topology.
//
// Flooding (plain or RSSI-suppressed) is the right default when you don't
// know the network's shape in advance. If you DO know it — a fixed chain or
// star of boards you deployed yourself — a static routing table skips the
// broadcast entirely: each end-to-end destination maps to one specific L2
// next-hop neighbour, and only that neighbour is addressed on the wire
// (real unicast at L2, not "everyone hears it, only one node cares").
//
// Topology assumed here:  0x01 (this node) <-> 0x02 <-> 0x03
// To reach 0x03, this node must hop via 0x02.
//
// On a lookup miss, `fallback` decides what happens:
//   FloodBroadcast — degrade to flooding for unlisted destinations (safer
//                    default when the table might be incomplete)
//   Drop           — misroutes instead of refusing: nextHop() returns the
//                    unlisted destination as the L2 next hop, so the frame
//                    is queued and sent normally; an out-of-range target
//                    just times out (ACK_TIMEOUT), same as any other loss
//
// Target: ESP32 + SX1262

#include <RFNode.h>

#define MY_ADDR  0x01
#define DST_ADDR 0x03  // reached via 0x02, per the table below

#define LORA_CS   41
#define LORA_IRQ  39
#define LORA_RST  42
#define LORA_BUSY 40

static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

static constexpr StaticRoutingStrategy::Entry kRoutes[] = {
    { 0x03, 0x02 },   // to reach 0x03, hop via 0x02
    // Add one row per known destination. Node 0x02 would carry its own
    // table (e.g. { 0x01, 0x01 } to hop back, no entry needed for itself).
};

static StaticRoutingStrategy routing(
    kRoutes, sizeof(kRoutes) / sizeof(kRoutes[0]),
    StaticRoutingStrategy::Fallback::FloodBroadcast);

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr = MY_ADDR;
    c.mode = PacketMode::Mesh;

    c.mesh.hopCount = 3;
    c.mesh.routing  = &routing;

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
    (void)node.sendAck(DST_ADDR, "hello via known route");  // demo; see send.cpp for SendStatus handling
    delay(5000);
}
