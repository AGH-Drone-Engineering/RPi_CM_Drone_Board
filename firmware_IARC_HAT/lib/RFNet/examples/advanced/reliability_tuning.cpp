// Reliability tuning — every RFReliabilityConfig knob, what it actually does.
//
// Defaults (all fields' compile-time RF_* fallbacks) are reasonable for a
// typical LoRa link. Reach for these only once you've measured a real
// problem (too many false ACK_TIMEOUTs, fragments dying under interference,
// outgoing ACKs getting lost under contention) — tuning blind just trades
// one failure mode for another.
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
    c.mode = PacketMode::Mesh;   // margins below matter most in Mesh

    // Fixed ACK timeout in ms. 0 (default) = auto-resolve from the frame's
    // time-on-air + mode (P2P: 2×ToA + margin; Mesh: scales with hop count —
    // see ackTimeoutMarginMeshMs below). Set a fixed value only if you know
    // your link budget precisely and want a tighter bound than auto-resolve
    // gives you; get this wrong and you'll see spurious ACK_TIMEOUTs on a
    // perfectly healthy link.
    c.reliability.ackTimeoutMs = 0;

    // Extra margin added ON TOP of 2×ToA(ack) in P2P mode. Covers scheduler
    // jitter, the peer's own CCA cost, and slow user callbacks. Raise this
    // if you see ACK_TIMEOUTs that a sniffer shows WERE actually answered —
    // just too late for the default margin.
    c.reliability.ackTimeoutMarginP2Pms = 150;

    // Same idea, but for Mesh: a mesh ACK has a much longer round trip (each
    // hop pays its own CCA + forward-jitter window + retry backoff). This
    // margin itself is added once (flat), not per hop — hop-count scaling
    // comes from the jitter-window/retry terms, not from this margin. Raise
    // it for deep/lossy meshes; the cost is slower failure detection for
    // genuinely unreachable peers.
    c.reliability.ackTimeoutMarginMeshMs = 300;

    // Per-fragment retry budget for large (fragmented) sends. On TX or ACK
    // failure, the SAME fragment is re-sent (not the whole message) up to
    // this many times before the session gives up — the receiver's
    // reassembler dedupes by (msgId, idx), so retransmits are always safe.
    // 0 = legacy "one failed fragment kills the whole session" behaviour.
    // Higher = more resilient to transient interference, at the cost of a
    // longer worst-case session duration under persistent interference.
    c.reliability.fragRetryMax = 2;

    // Retry budget for OUR OWN outgoing ACKs when CCA is busy or radio.send
    // fails. Without this, one lost ACK kills the sender's whole message —
    // especially painful for a fragmented session, where a single missing
    // fragment ACK times out the entire multi-second transfer.
    c.reliability.ackRetryMax = 3;

    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.printf("[RX] from=0x%02X \"%.*s\"\n", info.from, (int)len, data);
}

static void onSendFail(const SentInfo& info, TxFailReason reason, void*) {
    Serial.printf("[TX FAIL] to=0x%02X reason=%d\n", info.to, (int)reason);
}

void setup() {
    Serial.begin(115200);
    node.onReceive(onReceive);
    node.onSendFail(onSendFail);

    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    node.startWorkerTask(4096, 5);
}

void loop() {
    // Per-message override: SendOptions.ackTimeoutMs beats both the fixed
    // cfg value and auto-resolve, for this one message only.
    SendOptions opts;
    opts.requireAck   = true;
    opts.ackTimeoutMs = 8000;   // generous timeout for a known-slow peer
    (void)node.send(DST_ADDR, "tuned message", opts);  // demo; see send.cpp for SendStatus handling

    delay(10000);
}
