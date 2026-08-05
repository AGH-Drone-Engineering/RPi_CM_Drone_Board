#include <RFNode.h>
#include <cstring>
#include <esp_attr.h>
#include <esp_system.h>
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
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
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

static bool killed = false;

// Kept across warm resets (watchdog, panic, esp_restart, the DTR/RTS reset a host
// triggers on the serial-JTAG) but not across a power cycle.
#define KILL_LATCH_MAGIC 0x4B4C4421u // "KLD!"
RTC_NOINIT_ATTR static uint32_t killLatch;

static bool killLatchEngaged(esp_reset_reason_t rr)
{
    // RTC RAM is undefined after a power cycle, so POWERON is authoritative. Any
    // other cause keeps the latch, brownout included.
    if (rr == ESP_RST_POWERON)
    {
        killLatch = 0;
        return false;
    }
    return killLatch == KILL_LATCH_MAGIC;
}

// Safe from any task and safe to call twice. Deliberately does not block: where the
// board survives a kill, a live radio still answers the panel's next KILL.
static void engageKillswitch()
{
    // FC first, then the latch while we are sure we still have power, then our own.
    digitalWrite(KILLSWITCH_FC_CTL, LOW);
    killLatch = KILL_LATCH_MAGIC;
    digitalWrite(KILLSWITCH_PSU_CTL, LOW);
    __atomic_store_n(&killed, true, __ATOMIC_RELEASE);
}

// Never leave the aircraft powered when the kill path is not up. Releasing the PSU
// latch powers the board down, so a failed boot looks like a board that will not
// turn on rather than a silently armed drone.
[[noreturn]] static void bootAbort(const char *reason)
{
    digitalWrite(KILLSWITCH_FC_CTL, LOW);
    LOG_E("main", "boot_error reason=%s — disarmed, releasing PSU latch", reason);
    delay(50); // give the USB-CDC FIFO a chance to drain if a host is attached
    digitalWrite(KILLSWITCH_PSU_CTL, LOW);
    while (1)
        delay(1000);
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
    const esp_reset_reason_t rr = esp_reset_reason();
    const bool latched = killLatchEngaged(rr);
    if (latched)
        __atomic_store_n(&killed, true, __ATOMIC_RELEASE);

    digitalWrite(KILLSWITCH_FC_CTL, LOW);
    pinMode(KILLSWITCH_FC_CTL, OUTPUT);
    digitalWrite(KILLSWITCH_PSU_CTL, latched ? LOW : HIGH);
    pinMode(KILLSWITCH_PSU_CTL, OUTPUT);

    Serial.begin(115200); // USB-CDC: boot/debug diagnostics only
    Logger::setWriteFn(&logToUsbCdc);

    // Default RX ring buffer can't hold a full max-size protocol frame
    Serial0.setRxBufferSize(4096);
    Serial0.begin(RPI_UART_BAUD, SERIAL_8N1, RPI_UART_RX, RPI_UART_TX);

    // Boot diagnostics are only worth waiting for when a host could be watching. On
    // a warm reset the FC is unpowered until setup() finishes, so skip it.
    if (rr == ESP_RST_POWERON)
        delay(2000);

    // Reachable only when something other than the PSU keeps us alive, e.g. bench USB.
    if (latched)
        LOG_W("main", "kill latch engaged (reset=%d) — staying disarmed, power-cycle to clear", (int)rr);

    uint8_t myAddr = readAddrJumpers();
    if (myAddr == 0)
    {
        // All five jumpers open/low. Starting with addr 0 would just fail in
        // begin() with a generic invalid_config, so name the real cause.
        bootAbort("addr_jumpers_unset (JP1..JP5 all low, need addr 1..31)");
    }
    nodeCfg.addr = myAddr;

    loraSPI.begin(LORA_CLK, LORA_MISO, LORA_MOSI, LORA_CS);

    static RFNode nodeObj(radio, nodeCfg);
    static UartRfBridge bridgeObj(nodeObj, Serial0, myAddr);
    node = &nodeObj;
    bridge = &bridgeObj;

    // Wire RFNode events to the bridge (handler bodies live in UartRfBridge).
    node->onReceive(
        // A KILL from the ground panel trips the killswitch; everything reaches the bridge.
        [](const RxInfo &info, const uint8_t *data, size_t len, void *ctx)
        {
            if (info.from == KILL_MASTER_ADDR && !info.broadcast &&
                len == KILL_PAYLOAD_LEN && memcmp(data, KILL_PAYLOAD, KILL_PAYLOAD_LEN) == 0)
            {
                engageKillswitch();
                LOG_W("main", "KILL from 0x%02X — killswitch engaged (FC cut, PSU latch released)",
                      info.from);
                // Falls through so the RPi can flush its filesystem before losing power.
            }
            UartRfBridge::onReceiveTrampoline(info, data, len, ctx);
        },
        bridge);
    node->onSendOk(UartRfBridge::onSendOkTrampoline, bridge);
    node->onSendFail(UartRfBridge::onSendFailTrampoline, bridge);

    radio.setTransmitProfile(RadioLibLoRaRadio<SX1262>::RfProfile::HIGH_SPEED);

    BeginStatus bs = node->begin();
    if (bs != BeginStatus::OK)
        bootAbort(beginStatusToStr(bs));

    if (!node->startWorkerTask())
        bootAbort("worker_task_start_failed");

    // Kill path is live, so the aircraft may have power. Re-checked because RX is
    // armed in the worker, so a KILL can land before this line.
    if (__atomic_load_n(&killed, __ATOMIC_ACQUIRE))
    {
        LOG_W("main", "kill engaged during boot — staying disarmed addr=0x%02X", myAddr);
        return;
    }

    digitalWrite(KILLSWITCH_FC_CTL, HIGH);
    LOG_I("main", "armed addr=0x%02X (from jumpers) build=%s", myAddr, CMDB_ESP_FIRMWARE_BUILD);
}

void loop()
{
    bridge->poll();

    // Undo anything that reconfigured the pins - a glitch, ESD from a hard landing.
    if (__atomic_load_n(&killed, __ATOMIC_ACQUIRE))
    {
        digitalWrite(KILLSWITCH_FC_CTL, LOW);
        digitalWrite(KILLSWITCH_PSU_CTL, LOW);
    }
}
