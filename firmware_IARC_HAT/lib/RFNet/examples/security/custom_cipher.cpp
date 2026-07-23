// Security via a custom ICypher — inject your own AES-GCM backend.
//
// RFNode picks DefaultCipher automatically (STM32 HAL CRYP > mbedTLS >
// software AES, whichever is available — see port/crypto/DefaultCipher.h).
// Use RFSecurityConfig::FromCipher() instead when you need to:
//   - force a specific backend rather than the auto-picked default
//   - share one cipher instance across multiple RFNode objects
//   - plug in your own ICypher implementation (a different HW accelerator,
//     a test double that logs every encrypt/decrypt call, etc.)
//
// FromCipher() takes ownership of NOTHING — you own the ICypher instance's
// lifetime and must keep it alive for as long as the RFNode exists. You are
// also responsible for calling setKey() yourself; RFNode's begin() does NOT
// derive or set a key for you on this path (unlike FromPassword/FromKey).
//
// Below: mbedTLS explicitly, available on ESP-IDF out of the box. On an
// STM32 target with HAL CRYP, swap in AesGcmStm32Hal instead — same ICypher
// interface, no other code changes needed.
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

static const uint8_t networkKey[CRYPTO_KEY_SIZE] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

static AesGcmMbedTls hwCipher;
// On STM32: static AesGcmStm32Hal hwCipher;  (same interface, different chip)

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr     = MY_ADDR;
    c.mode     = PacketMode::P2P;
    c.security = RFSecurityConfig::FromCipher(hwCipher);
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

    // FromCipher does NOT set the key for you — do it before begin().
    if (!hwCipher.setKey(networkKey)) {
        Serial.println("[ERR] cipher setKey() failed");
        while (true) delay(1000);
    }

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
