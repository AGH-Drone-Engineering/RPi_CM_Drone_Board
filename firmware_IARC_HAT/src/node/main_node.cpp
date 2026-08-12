#include <RFNode.h>
#include <cstring>
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

// Killswitch pins
#define KILLSWITCH_FC_CTL 1
#define KILLSWITCH_PSU_CTL 6

// RPi <-> ESP link: hardware UART0 (Serial0), remapped off its default pins.
// The module's native TXD0/RXD0 (IO43/IO44, pads 37/36) go to J4
// (J_ESP_DBG_UART), not to the Pi - see pcb_IARC_HAT/comm.kicad_sch. The link
// to the host runs over the 40-pin header J8: net GPIO8 (header pin 24, the
// Pi's TXD3) lands on U1 pad 11 = IO18, and net GPIO9 (header pin 21, the Pi's
// RXD3) on U1 pad 10 = IO17. That is UART3 on the Pi, i.e. /dev/ttyAMA3, which
// is what loracom opens by default.
#define RPI_UART_TX 17
#define RPI_UART_RX 18
#define RPI_UART_BAUD 115200

#define KILL_PAYLOAD "KILL"
static constexpr size_t KILL_PAYLOAD_LEN = sizeof(KILL_PAYLOAD) - 1;

// Only the ground panel may kill
static constexpr uint8_t KILL_MASTER_ADDR = 0xFE;

// Node address is set by the five solder jumpers JP1..JP5 (schematic labels
// ADDR1/2/4/8/16, pcb_IARC_HAT/comm.kicad_sch), least significant bit first.
static const uint8_t ADDR_PINS[] = {10, 11, 12, 13, 14};
static constexpr size_t ADDR_BITS = sizeof(ADDR_PINS) / sizeof(ADDR_PINS[0]);

static SPIClass loraSPI;
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU869_DC10,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, loraSPI);

// addr is left at its default here and filled in from the jumpers in setup() -
// reading GPIOs during static init would run before the Arduino core is up.
static RFNodeConfig nodeCfg = []()
{
    RFNodeConfig c;
    c.mode = PacketMode::P2P;
    c.security = RFSecurityConfig::FromPassword("bajer");
    c.dutyCycle.enabled = false;
    return c;
}();

// Both need the jumper-derived address, so they're constructed in setup() once
// it's known; loop() reaches them through these.
static RFNode *node = nullptr;
static UartRfBridge *bridge = nullptr;

// Runs on the RF worker task. Safe to call twice - it only ever drives pins low.
static void engageKillswitch()
{
    digitalWrite(KILLSWITCH_FC_CTL, LOW);
    digitalWrite(KILLSWITCH_PSU_CTL, LOW);
}

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

void setup()
{
    Serial.begin(115200); // USB-CDC: boot/debug diagnostics only
    // Diagnostics must never cost us RPi UART bytes. HWCDC::write blocks up to
    // tx_timeout_ms (default 100) per call when a CDC host is attached but not
    // draining, and loop() is the only thing servicing the UART RX ring - a few
    // such stalls during a host burst overflow it. 1 ms caps the damage; lines
    // are dropped instead of stalling the parser. (Unplugged CDC never blocks.)
    Serial.setTxTimeoutMs(1);
    Logger::setWriteFn(&logToUsbCdc);

    pinMode(KILLSWITCH_FC_CTL, OUTPUT);
    pinMode(KILLSWITCH_PSU_CTL, OUTPUT);
    
    digitalWrite(KILLSWITCH_FC_CTL, LOW);
    digitalWrite(KILLSWITCH_PSU_CTL, HIGH);

    // Default RX ring buffer can't hold a full max-size protocol frame. Sized
    // well past that so a host burst survives a stalled loop() too: 8 kB is
    // ~700 ms of line time at 115200, and nothing in poll() should hold the task
    // anywhere near that long.
    Serial0.setRxBufferSize(8192);
    // TX side of the same problem: with the 256 B default, sendFrame's write()
    // blocks until the FIFO drains (a max-size GETMSG reply is ~260 ms of wire
    // time at 115200) while nothing reads the RX ring. 4 kB holds any single
    // reply, so write() returns at once and the UART drains in the background.
    // Both setters must precede begin().
    Serial0.setTxBufferSize(4096);
    Serial0.begin(RPI_UART_BAUD, SERIAL_8N1, RPI_UART_RX, RPI_UART_TX);
    delay(2000);

    uint8_t myAddr = readAddrJumpers();
    if (myAddr == 0)
    {
        // All five jumpers open/low. Starting with addr 0 would just fail in
        // begin() with a generic invalid_config, so name the real cause.
        LOG_E("main", "boot_error reason=addr_jumpers_unset (JP1..JP5 all low, need addr 1..31)");
        while (1)
            delay(1000);
    }
    nodeCfg.addr = myAddr;

    loraSPI.begin(LORA_CLK, LORA_MISO, LORA_MOSI, LORA_CS);

    static RFNode nodeObj(radio, nodeCfg);
    static UartRfBridge bridgeObj(nodeObj, Serial0, myAddr);
    node = &nodeObj;
    bridge = &bridgeObj;

    // Wire RFNode events to the bridge (handler bodies live in UartRfBridge).
    node->onReceive(
        // A KILL from the ground panel drops both gates; everything reaches the bridge.
        [](const RxInfo &info, const uint8_t *data, size_t len, void *ctx)
        {
            if (info.from == KILL_MASTER_ADDR && !info.broadcast &&
                len == KILL_PAYLOAD_LEN && memcmp(data, KILL_PAYLOAD, KILL_PAYLOAD_LEN) == 0)
            {
                engageKillswitch();
                LOG_W("main", "KILL from 0x%02X — killswitch engaged (FC/PSU low)", info.from);
            }
            UartRfBridge::onReceiveTrampoline(info, data, len, ctx);
        },
        bridge);
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

    if (!node->startWorkerTask())
    {
        LOG_E("main", "boot_error reason=worker_task_start_failed");
        while (1)
            delay(1000);
    }

    // Radio is up, so the aircraft may have power.
    digitalWrite(KILLSWITCH_FC_CTL, HIGH);
    LOG_I("main", "armed addr=0x%02X (from jumpers) build=%s", myAddr, CMDB_ESP_FIRMWARE_BUILD);
}

void loop()
{
    bridge->poll();
}
