// Four ways to send a message.
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
    // info.broadcast: true if this frame was addressed to 0xFF, not to us specifically.
    Serial.printf("[RX] from=0x%02X bcast=%d rssi=%d dBm  \"%.*s\"\n",
                  info.from, (int)info.broadcast, info.rssi, (int)len, data);
}

// Fires for every successful send — fire-and-forget, acked, or broadcast —
// just immediately after TX for no-ack, after ACK receipt for sendAck().
static void onSendOk(const SentInfo& info, void*) {
    Serial.printf("[TX OK] to=0x%02X seq=%lu\n", info.to, (unsigned long)info.seq);
}

static void onSendFail(const SentInfo& info, TxFailReason reason, void*) {
    Serial.printf("[TX FAIL] to=0x%02X reason=%d\n", info.to, (int)reason);
}

void setup() {
    Serial.begin(115200);
    node.onReceive(onReceive);
    node.onSendOk(onSendOk);
    node.onSendFail(onSendFail);

    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    node.startWorkerTask(4096, 5);
}

void loop() {
    // send() — fire-and-forget unicast. No ACK is requested, so the
    // receiver doesn't send one back. SendStatus reports only whether the
    // message was admitted (queued), not whether it was delivered.
    SendStatus st = node.send(DST_ADDR, "no-ack ping");
    if (st != SendStatus::OK)
        Serial.printf("[WARN] send() status=%u\n", (unsigned)st);
    delay(3000);

    // sendAck() — reliable unicast. Requests an ACK and waits for it with an
    // auto-resolved timeout. Delivery result arrives later via
    // onSendOk/onSendFail, not the return value here.
    st = node.sendAck(DST_ADDR, "acked ping");
    if (st != SendStatus::OK)
        Serial.printf("[WARN] sendAck() status=%u\n", (unsigned)st);
    delay(3000);

    // send() with SendOptions — the one real entry point: lets you request an
    // ACK, override the hop count, and set a per-message ACK timeout instead
    // of sendAck()'s auto-resolved one.
    Serial.println("Sending \"ping with timeout\"...");
    SendOptions opts;
    opts.requireAck = true;
    opts.ackTimeoutMs = 2000;
    st = node.send(DST_ADDR, "ping with timeout", opts);
    if (st != SendStatus::OK)
        Serial.printf("[WARN] send \"ping with timeout\" status=%u\n", (unsigned)st);
    delay(3000);

    // sendBroadcast() — address 0xFF, received by every node in range.
    // Always fire-and-forget: an ACK from many receivers to one sender
    // doesn't mean anything, so no ACK is ever requested for it.
    st = node.sendBroadcast("hello everyone");
    if (st != SendStatus::OK)
        Serial.printf("[WARN] sendBroadcast() status=%u\n", (unsigned)st);
    delay(3000);
}
