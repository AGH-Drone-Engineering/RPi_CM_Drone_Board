#include <RFNode.h>
#include "UartRfBridge.h"

// Ground-station variant of main_node.cpp: the host protocol runs over USB-CDC
// to a phone instead of over the RPi UART. Built by env:phone, which compiles
// this src/node/ with main_node.cpp filtered out; the two share UartRfBridge.

// Fallback used only when JP1..JP5 read 0 (unpopulated). 5 is the ground-station
// address the Pi tooling defaults to; drones take 1..4.
#ifndef NODE_ADDR
#define NODE_ADDR 5
#endif

static void logToUsbCdc(const char *str)
{
    Serial.print(str);
}

// Once USB carries framed traffic, runtime logs move to the spare RPi UART so
// they cannot interleave with frames.
static void logToDebugUart(const char *str)
{
    Serial0.print(str);
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

// Killswitch pin
#define KILLSWITCH_FC_CTL 1
#define KILLSWITCH_PSU_CTL 6

// Debug UART, same pins as the RPi link in main_node.cpp - unused for the protocol
// here, so it carries the runtime log instead.
#define RPI_UART_TX 17
#define RPI_UART_RX 18
#define RPI_UART_BAUD 115200

// Same jumper wiring as main_node.cpp: ADDR1/2/4/8/16 on these pins, LSB first.
static const uint8_t ADDR_PINS[] = {10, 11, 12, 13, 14};
static constexpr size_t ADDR_BITS = sizeof(ADDR_PINS) / sizeof(ADDR_PINS[0]);

static uint8_t readAddrJumpers()
{
    for (uint8_t pin : ADDR_PINS)
        pinMode(pin, INPUT_PULLDOWN);
    delay(1); // let the pulldown settle before sampling

    uint8_t addr = 0;
    for (size_t bit = 0; bit < ADDR_BITS; ++bit)
    {
        if (digitalRead(ADDR_PINS[bit]) == HIGH)
            addr |= static_cast<uint8_t>(1u << bit);
    }
    return addr;
}

static SPIClass loraSPI;
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU869_DC10,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, loraSPI);

static RFNodeConfig nodeCfg = []()
{
    RFNodeConfig c;
    c.mode = PacketMode::P2P;
    c.security = RFSecurityConfig::FromPassword("bajer");
    c.dutyCycle.enabled = false;
    return c;
}();

static RFNode *node = nullptr;
static UartRfBridge *bridge = nullptr;

void setup()
{
    // The host protocol runs here, so the CDC buffers must hold a full max-size
    // frame - same reason main_node.cpp enlarges the UART ones. Both setters have
    // to precede begin(), which allocates a 256 B default for whatever is still
    // unset.
    //
    // TX matters as much as RX: HWCDC::write copies the frame into this ring and
    // returns - but only while it fits. Past that it blocks in 1 ms rounds up to
    // tx_timeout_ms (100 by default) waiting for the phone to drain, and poll()
    // is the only thing servicing the RX side meanwhile. 4 kB covers any single
    // reply, so a stalled reader costs nothing until it stops reading entirely.
    //
    // Deliberately NOT mirroring main_node.cpp's setTxTimeoutMs(1) here: there
    // Serial carries only the log, so a dropped write costs a log line. Here it
    // carries the protocol, and a truncated write is a corrupt frame - the 100 ms
    // default is the lesser evil.
    Serial.setRxBufferSize(8192);
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    Logger::setWriteFn(&logToUsbCdc);

    pinMode(KILLSWITCH_FC_CTL, OUTPUT);
    pinMode(KILLSWITCH_PSU_CTL, OUTPUT);

    digitalWrite(KILLSWITCH_FC_CTL, HIGH);
    digitalWrite(KILLSWITCH_PSU_CTL, HIGH);

    // Carries the runtime log once the bridge is up (see logToDebugUart). With the
    // 256 B default, HardwareSerial::write blocks until the FIFO drains - ~22 ms at
    // 115200 for a full buffer - and that comes straight out of poll()'s budget on
    // a chatty run. Must precede begin().
    Serial0.setTxBufferSize(4096);
    Serial0.begin(RPI_UART_BAUD, SERIAL_8N1, RPI_UART_RX, RPI_UART_TX);
    delay(2000);

    // Unlike main_node.cpp this does not halt on unset jumpers - a ground-station
    // board is often unpopulated, so it falls back to the compiled-in address.
    uint8_t myAddr = readAddrJumpers();
    const bool fromJumpers = myAddr != 0;
    if (!fromJumpers)
        myAddr = NODE_ADDR;
    nodeCfg.addr = myAddr;

    loraSPI.begin(LORA_CLK, LORA_MISO, LORA_MOSI, LORA_CS);

    static RFNode nodeObj(radio, nodeCfg);
    static UartRfBridge bridgeObj(nodeObj, Serial, myAddr);
    node = &nodeObj;
    bridge = &bridgeObj;

    node->onReceive(UartRfBridge::onReceiveTrampoline, bridge);
    node->onSendOk(UartRfBridge::onSendOkTrampoline, bridge);
    node->onSendFail(UartRfBridge::onSendFailTrampoline, bridge);

    radio.setTransmitProfile(RadioLibLoRaRadio<SX1262>::RfProfile::HIGH_SPEED);

    BeginStatus bs = node->begin();
    if (bs != BeginStatus::OK)
    {
        LOG_E("main", "boot_error reason=%s", beginStatusToStr(bs));
        while (1)
            delay(1000);
    }
    // Was unchecked: without the worker the node answers the phone normally but
    // never touches the radio, which looks like a dead link rather than a boot
    // failure. Halt loudly, as main_node.cpp does.
    if (!node->startWorkerTask())
    {
        LOG_E("main", "boot_error reason=worker_task_start_failed");
        while (1)
            delay(1000);
    }

    LOG_I("main", "ready addr=0x%02X (%s)", myAddr,
          fromJumpers ? "from jumpers" : "compiled-in fallback");
    LOG_I("main", "usb_bridge active, logs move to UART TX=%d", RPI_UART_TX);
    Logger::setWriteFn(&logToDebugUart);
}

void loop()
{
    bridge->poll();
}
