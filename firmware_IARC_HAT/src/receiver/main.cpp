#include <RFNode.h>

// CMDB HAT
#define LORA_CS 38
#define LORA_IRQ 4
#define LORA_RST 39
#define LORA_BUSY 5

#define LORA_RF_SW 2
#define LORA_CLK 40
#define LORA_MOSI 41
#define LORA_MISO 42

#define MY_ADDR 0x02
#define DST_ADDR 0x01

static FreeRtosOsal osal;
static SPIClass loraSPI;
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, loraSPI);

static RFNodeConfig nodeCfg = []()
{
    RFNodeConfig c;
    c.addr     = MY_ADDR;
    c.mode     = PacketMode::Mesh;
    c.security = RFSecurityConfig::FromPassword("secret_password");
    c.dutyCycle.enabled = false;
    return c;
}();

static RFNode node(radio, osal, nodeCfg);

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
    Serial.printf("[OK] RF node started. My address: 0x%02X (RECEIVER)\n", MY_ADDR);
}

void loop()
{
    delay(1000);
}
