#include <RFNode.h>
#include "UartRfBridge.h"

// Logger write function for USB-CDC (Serial) output.
static void logToUsbCdc(const char *str)
{
    Serial.print(str);
}

static const char *beginStatusToStr(BeginStatus bs)
{
    switch (bs)
    {
    case BeginStatus::OK:
        return "ok";
    case BeginStatus::ALREADY_STARTED:
        return "already_started";
    case BeginStatus::INVALID_CONFIG:
        return "invalid_config";
    case BeginStatus::SECURITY_INIT_FAILED:
        return "security_init_failed";
    case BeginStatus::RADIO_INIT_FAILED:
        return "radio_init_failed";
    case BeginStatus::OUT_OF_MEMORY:
        return "out_of_memory";
    case BeginStatus::NV_NOT_PERSISTENT:
        return "nv_not_persistent";
    case BeginStatus::NV_SEQ_CORRUPTED:
        return "nv_seq_corrupted";
    default:
        return "unknown";
    }
}

// LoRa module pinout on the IARC HAT (SX1262):
#define LORA_CS 38
#define LORA_IRQ 4
#define LORA_RST 39
#define LORA_BUSY 5

#define LORA_RF_SW 2
#define LORA_CLK 40
#define LORA_MOSI 41
#define LORA_MISO 42

// RPi <-> ESP link: hardware UART0 (Serial0)
#define RPI_UART_TX 43
#define RPI_UART_RX 44
#define RPI_UART_BAUD 115200

#define MY_ADDR 0x01

static SPIClass loraSPI;
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, loraSPI);

static RFNodeConfig nodeCfg = []()
{
    RFNodeConfig c;
    c.addr = MY_ADDR;
    c.mode = PacketMode::P2P;
    c.security = RFSecurityConfig::FromPassword("bajer");
    c.dutyCycle.enabled = false;
    return c;
}();

static RFNode node(radio, nodeCfg);
static UartRfBridge bridge(node, Serial0, MY_ADDR);

void setup()
{
    Serial.begin(115200); // USB-CDC: boot/debug diagnostics only
    Logger::setWriteFn(&logToUsbCdc);

    // Default RX ring buffer can't hold a full max-size protocol frame
    Serial0.setRxBufferSize(4096);
    Serial0.begin(RPI_UART_BAUD, SERIAL_8N1, RPI_UART_RX, RPI_UART_TX);
    delay(2000);

    loraSPI.begin(LORA_CLK, LORA_MISO, LORA_MOSI, LORA_CS);

    // Wire RFNode events to the bridge (handler bodies live in UartRfBridge).
    node.onReceive(UartRfBridge::onReceiveTrampoline, &bridge);
    node.onSendOk(UartRfBridge::onSendOkTrampoline, &bridge);
    node.onSendFail(UartRfBridge::onSendFailTrampoline, &bridge);

    radio.setTransmitProfile(RadioLibLoRaRadio<SX1262>::RfProfile::HIGH_SPEED);

    BeginStatus bs = node.begin();
    if (bs != BeginStatus::OK)
    {
        LOG_E("main", "boot_error reason=%s", beginStatusToStr(bs));
        while (1)
            delay(1000);
    }
    node.startWorkerTask();

    LOG_I("main", "ready addr=0x%02X", MY_ADDR);
}

void loop()
{
    bridge.poll();
}
