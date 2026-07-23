// Security via a raw pre-shared key — no KDF overhead at startup.
//
// RFSecurityConfig::FromKey() takes the 128-bit AES-GCM key directly: no
// PBKDF2 pass, so begin() is instant. Trade-off is on you: generate the key
// with a CSPRNG (never derive it from something guessable like a serial
// number) and provision it securely (flash it once via a signed factory
// image, or an authenticated bootstrap channel) — there's no password to
// remember and no rate-limiting fallback if it leaks.
//
// Losing this key means the network is permanently locked out — there is no
// way to recover it (that's the point: it never derives from anything you
// could regenerate).
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

// Generate with a CSPRNG, e.g.:  openssl rand -hex 16
// Every node in the network needs this exact same 16 bytes.
static const uint8_t networkKey[CRYPTO_KEY_SIZE] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr     = MY_ADDR;
    c.mode     = PacketMode::P2P;
    c.security = RFSecurityConfig::FromKey(networkKey);
    // NOTE: encryption needs a PERSISTENT NV backend — begin() refuses a volatile
    // one (the nonce counter would reset on reboot and reuse GCM nonces). ESP32's
    // default (Preferences) is persistent, so none is set here; see
    // nvs_persistence.cpp to wire one explicitly (required on targets with no
    // persistent default).
    return c;
}();

static RFNode node(radio, cfg);

static void onReceive(const RxInfo& info, const uint8_t* data, size_t len, void*) {
    Serial.printf("[RX] from=0x%02X \"%.*s\"\n", info.from, (int)len, data);
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
    (void)node.sendAck(DST_ADDR, "secret message");  // demo; see send.cpp for SendStatus handling
    delay(5000);
}
