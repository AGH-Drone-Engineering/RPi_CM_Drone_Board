// Mesh, default routing — plain flooding, zero setup.
//
// In Mesh mode every node forwards a frame it didn't originate, up to
// cfg.mesh.hopCount hops, as long as it hasn't seen that (src, seq) before
// (duplicate suppression is automatic — you never get exponential re-floods).
// Leave cfg.mesh.routing at its default (nullptr) and RFNode installs a
// built-in ManagedFloodingStrategy for you — this is the "it just works"
// mesh setup; reach for ../mesh/rssi_suppression.cpp or
// ../mesh/static_routing.cpp only when plain flooding costs too much airtime
// for your topology.
//
// A message sent with sendAck() follows the same forwarding path in both
// directions — the ACK is routed back hop-by-hop, not sent directly.
//
// Run this same firmware on 3+ boards with different MY_ADDR (0x01, 0x02,
// 0x03…) to see multi-hop forwarding in action.
//
// Target: ESP32 + SX1262

#include <RFNode.h>

#define MY_ADDR  0x01
#define DST_ADDR 0x03  // may be out of direct range — reached via 0x02

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

    c.mesh.hopCount      = 3;    // max hops; 0 = no forwarding (P2P behaviour)
    c.mesh.forwardJitter = true; // stagger re-broadcasts — keep true in production
    // c.mesh.routing left at nullptr — default flooding, see file header.

    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    // info.from is always the ORIGINAL sender, never an intermediate
    // forwarder — forwarding is transparent to the application layer.
    Serial.printf("[RX] from=0x%02X bcast=%d rssi=%d  \"%.*s\"\n",
                  info.from, (int)info.broadcast, info.rssi, (int)len, data);
}

static void onSendFail(const SentInfo& info, TxFailReason reason, void*) {
    if (reason == TxFailReason::ACK_TIMEOUT) {
        Serial.printf("[ACK TIMEOUT] to=0x%02X — unreachable within %lu ms "
                      "(mesh timeout scales with hop count)\n",
                      info.to, (unsigned long)info.ackTimeoutMs);
    }
}

void setup() {
    Serial.begin(115200);
    node.onReceive(onReceive);
    node.onSendFail(onSendFail);

    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    node.startWorkerTask();
}

void loop() {
    // Unicast with ACK — forwarded up to c.mesh.hopCount hops each way.
    (void)node.sendAck(DST_ADDR, "hello mesh");  // demo; see send.cpp for SendStatus handling
    delay(10000);

    // Broadcast — every node in the entire mesh receives and re-forwards it.
    (void)node.sendBroadcast("network announcement");  // demo
    delay(10000);
}
