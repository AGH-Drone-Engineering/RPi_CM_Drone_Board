// Security via password — easiest setup, PBKDF2-SHA256 key derivation.
//
// RFSecurityConfig::FromPassword() derives a 128-bit AES-GCM key from a
// human-readable string. The iteration count (default
// RF_KDF_DEFAULT_ITERATIONS = 10 000) takes ~150 ms on ESP32-S3 — happens
// once, inside begin(). Raise it to trade startup time for stronger
// resistance to offline brute-force if the password is weak; with a strong,
// high-entropy password the count barely matters.
//
// A node with security configured DROPS unencrypted frames by default
// (requireEncrypted = true) — every node in the network must use the same
// password (hence the same derived key), or they simply can't talk.
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
    c.addr     = MY_ADDR;
    c.mode     = PacketMode::P2P;
    c.security = RFSecurityConfig::FromPassword("my_strong_password");

    // Custom iteration count — higher = slower begin() but stronger key
    // stretching against offline attacks on a weak password:
    // c.security = RFSecurityConfig::FromPassword("my_strong_password", 50000);

    // Accept unencrypted frames too (e.g. during a mixed-firmware rollout
    // where not every node has the password yet):
    // c.security.requireEncrypted = false;

    // Encryption needs a PERSISTENT NV backend (begin() refuses a volatile one —
    // nonce reuse across reboot). ESP32's default is persistent; see
    // nvs_persistence.cpp to wire one on targets without a persistent default.
    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.printf("[RX] from=0x%02X \"%.*s\"\n", info.from, (int)len, data);
}

void setup() {
    Serial.begin(115200);
    node.onReceive(onReceive);

    // begin() runs PBKDF2 here — may take ~150 ms depending on the iteration
    // count and MCU speed.
    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed — check password config");
        while (true) delay(1000);
    }
    node.startWorkerTask(4096, 5);
}

void loop() {
    (void)node.sendAck(DST_ADDR, "secret message");  // demo; see send.cpp for SendStatus handling
    delay(5000);
}
