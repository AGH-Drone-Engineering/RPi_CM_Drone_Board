#include <RFNode.h>
// XIAO
// #define LORA_CS 41
// #define LORA_IRQ 39
// #define LORA_RST 42
// #define LORA_BUSY 40

// CMDB HAT
#define LORA_CS 38
#define LORA_IRQ 4
#define LORA_RST 39
#define LORA_BUSY 5

#define LORA_RF_SW 2
#define LORA_CLK 40
#define LORA_MOSI 41
#define LORA_MISO 42

#define MY_ADDR 0x01
#define DST_ADDR 0x02

static FreeRtosOsal osal;
static SPIClass loraSPI;
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, loraSPI);

// Per-node runtime config (RFNodeConfig, see RFNode.h) — addr must be unique
// per node on the link and in [0x01, 0xFE]; security/dutyCycle/mesh/
// reliability/nv default to built-in behavior unless set here.
static RFNodeConfig nodeCfg = []()
{
    RFNodeConfig c;
    c.addr     = MY_ADDR;
    c.mode     = PacketMode::P2P;
    c.security = RFSecurityConfig::FromPassword("secret_password");
    c.dutyCycle.enabled = false;  // EU air-time limiting off — confirm before deploying on a regulated band
    return c;
}();

static RFNode node(radio, osal, nodeCfg);
static uint32_t counter = 0;

static void onReceive(const RxInfo &info, const uint8_t *data, size_t len, void *)
{
    Serial.printf("[RX] from=0x%02X  bcast=%d  rssi=%d dBm  len=%zu  \"%.*s\"\n",
                  info.from, (int)info.broadcast, (int)info.rssi,
                  len, (int)len, (const char *)data);
}

static void onSendOk(const SentInfo &info, void *)
{
    Serial.printf("[TX OK] to=0x%02X seq=%lu len=%zu\n",
                  info.to, (unsigned long)info.seq, info.payloadLen);
}

static void onSendFail(const SentInfo &info, TxFailReason reason, void *)
{
    if (reason == TxFailReason::ACK_TIMEOUT)
    {
        Serial.printf("[ACK TIMEOUT] to=0x%02X seq=%lu timeoutMs=%lu\n",
                      info.to, (unsigned long)info.seq, (unsigned long)info.ackTimeoutMs);
        return;
    }
    const char *why = "?";
    switch (reason)
    {
    case TxFailReason::RADIO_ERROR:   why = "radio_error";       break;
    case TxFailReason::CHANNEL_BUSY:  why = "channel_busy";      break;
    case TxFailReason::DUTY_CYCLE:    why = "duty_cycle";        break;
    case TxFailReason::PENDING_LIST_FULL: why = "pending_list_full"; break;
    case TxFailReason::FRAME_BUILD_FAILED: why = "frame_build_fail"; break;
    case TxFailReason::ACK_TIMEOUT:   why = "ack_timeout";       break;
    default: break;
    }
    Serial.printf("[TX FAIL] to=0x%02X seq=%lu reason=%s\n",
                  info.to, (unsigned long)info.seq, why);
}

void setup()
{
    Serial.begin(115200);
    delay(2000);
    loraSPI.begin(LORA_CLK, LORA_MISO, LORA_MOSI, LORA_CS);
    node.onReceive(onReceive);
    node.onSendOk(onSendOk);
    node.onSendFail(onSendFail);

    radio.setTransmitProfile(RadioLibLoRaRadio<SX1262>::RfProfile::HIGH_SPEED);

    BeginStatus bs = node.begin();
    if (bs != BeginStatus::OK)
    {
        Serial.printf("[ERR] node.begin() failed: %d\n", (int)bs);
        while (1)
            delay(1000);
    }
    node.startWorkerTask(4096, 5);
    Serial.printf("[OK] RF node started. My address: 0x%02X (TRANSMITTER)\n", MY_ADDR);
}

void loop()
{
    ++counter;

    // Send api with default options (no ack, default timeout)
    Serial.println("Sending \"ping without ack\"...");
    SendStatus st = node.send(DST_ADDR, "ping without ack");
    if (st != SendStatus::OK)
        Serial.printf("[WARN] send \"ping without ack\" status=%u\n", (unsigned)st);
    delay(3000);

    // Send api with default options and ack
    Serial.println("Sending \"ping with ack\"...");
    st = node.sendAck(DST_ADDR, "ping with ack");
    if (st != SendStatus::OK)
        Serial.printf("[WARN] sendAck \"ping with ack\" status=%u\n", (unsigned)st);
    delay(3000);

    // Send api via broadcast 
    Serial.println("Sending \"ping with broadcast\"...");
    st = node.sendBroadcast("ping with broadcast");
    if (st != SendStatus::OK)
        Serial.printf("[WARN] sendBroadcast \"ping with broadcast\" status=%u\n", (unsigned)st);
    delay(3000);

    char msg[500];
    for (int i = 0; i < sizeof(msg) - 1; i++)
        msg[i] = 'A' + (counter + i) % 26;
    msg[sizeof(msg) - 1] = '\0';

    // Send large message (fragmented)
    Serial.println("Sending long message...");
    st = node.send(DST_ADDR, msg);
    if (st != SendStatus::OK)
        Serial.printf("[WARN] send status=%u\n", (unsigned)st);
    delay(13000);
}
