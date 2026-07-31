#include <RFNode.h>
#include <Wire.h>

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

// ---------------------------------------------------------------------------
// Killswitch transmitter pinout (ESP32-S3-WROOM-1, U1).
// From IARC_Drone_KillSwitch_Transmitter/KillSwitch_pcb. Values are GPIOs.
// ---------------------------------------------------------------------------

// LoRa module (SX1262).
#define LORA_CS 38   // LORA_SPI_CS
#define LORA_IRQ 4   // LORA_DIO1
#define LORA_RST 39  // LORA_RESET
#define LORA_BUSY 5  // LORA_BUSY

#define LORA_RF_SW 2 // LORA_RF_SW
#define LORA_CLK 40  // LORA_SPI_CLK
#define LORA_MOSI 41 // LORA_SPI_MOSI
#define LORA_MISO 42 // LORA_SPI_MISO

// I2C to the MCP23017 (U2). R2/R3 (2K2) pull both lines up on the board.
#define I2C_SCL 15
#define I2C_SDA 16

// Front panel. SW4 shorts to GND when armed; both LEDs are active high.
#define SW_ARM 6
#define LED_ACT 7
#define LED_ARM 48

// SW1 DIP bank, each switch shorts its GPIO to GND when closed.
#define DIP_SEL1 9
#define DIP_SEL2 10
#define DIP_SEL3 11
#define DIP_SEL4 12
#define DIP_SEL5 13
#define DIP_SEL6 14
#define DIP_SEL7 21
#define DIP_SEL8 47

#define BOOT_MODE 0
#define ESP_TXD0 43
#define ESP_RXD0 44

// ---------------------------------------------------------------------------
// MCP23017 (U2) at 0x20 (A0..A2 grounded).
//   GPB0..3 = SW_KILL1..4   inputs, active low, internal pullups
//   GPB4..7 = LED_KILLED1..4
//   GPA0..3 = LED_FAULT1..4
//   GPA4..7 = LED_RX1..4
// INTA/INTB are unconnected, so the switches are polled.
// ---------------------------------------------------------------------------
#define MCP_ADDR 0x20
#define MCP_IODIRA 0x00
#define MCP_IODIRB 0x01
#define MCP_GPPUB 0x0D
#define MCP_GPIOB 0x13
#define MCP_OLATA 0x14
#define MCP_OLATB 0x15

#define MASK_KILLED(i) static_cast<uint8_t>(1u << (4 + (i)))
#define MASK_FAULT(i) static_cast<uint8_t>(1u << (i))
#define MASK_RX(i) static_cast<uint8_t>(1u << (4 + (i)))

static constexpr uint8_t DRONE_COUNT = 4;

// Destination address per kill switch, SW_KILL1..4 in order.
static const uint8_t DRONE_ADDR[DRONE_COUNT] = {0x01, 0x02, 0x03, 0x04};

// Own node address. 0xFF is broadcast, so the transmitter takes the one below.
static constexpr uint8_t TX_ADDR = 0xFE;

static const char *KILL_PAYLOAD = "KILL";

static constexpr uint32_t DEBOUNCE_MS = 25;
static constexpr uint32_t POLL_MS = 10;

static SPIClass loraSPI;
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0,
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

enum class KillState : uint8_t
{
    IDLE,
    INFLIGHT,
    DONE,
};

static KillState killState[DRONE_COUNT] = {};

// Set from the RFNode worker task, consumed in loop(). The loop owns every I2C
// access so the callbacks never touch the bus.
static volatile bool ackFlag[DRONE_COUNT] = {};
static volatile bool failFlag[DRONE_COUNT] = {};

// Shadow of the two output latches. Bits are only ever set by the events below,
// so everything starts dark.
static uint8_t latchA = 0;
static uint8_t latchB = 0;

static bool mcpWrite(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool mcpRead(uint8_t reg, uint8_t &val)
{
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return false;
    if (Wire.requestFrom(static_cast<int>(MCP_ADDR), 1) != 1)
        return false;
    val = static_cast<uint8_t>(Wire.read());
    return true;
}

static bool mcpBegin()
{
    return mcpWrite(MCP_IODIRA, 0x00) && // port A all outputs
           mcpWrite(MCP_IODIRB, 0x0F) && // GPB0..3 in, GPB4..7 out
           mcpWrite(MCP_GPPUB, 0x0F) &&  // pullups on the kill switches
           mcpWrite(MCP_OLATA, 0x00) &&
           mcpWrite(MCP_OLATB, 0x00);
}

static void flushLeds()
{
    static uint8_t lastA = 0xFF;
    static uint8_t lastB = 0xFF;

    if (latchA != lastA && mcpWrite(MCP_OLATA, latchA))
        lastA = latchA;
    if (latchB != lastB && mcpWrite(MCP_OLATB, latchB))
        lastB = latchB;
}

static int droneIndexFor(uint8_t addr)
{
    for (int i = 0; i < DRONE_COUNT; ++i)
    {
        if (DRONE_ADDR[i] == addr)
            return i;
    }
    return -1;
}

static void onSendOk(const SentInfo &info, void *)
{
    int i = droneIndexFor(info.to);
    if (i >= 0)
        ackFlag[i] = true;
}

static void onSendFail(const SentInfo &info, TxFailReason, void *)
{
    int i = droneIndexFor(info.to);
    if (i >= 0)
        failFlag[i] = true;
}

static void sendKill(int i)
{
    if (node->sendAck(DRONE_ADDR[i], KILL_PAYLOAD) == SendStatus::OK)
    {
        killState[i] = KillState::INFLIGHT;
    }
    else
    {
        latchA |= MASK_FAULT(i);
        killState[i] = KillState::IDLE;
    }
}

static void halt(const char *reason)
{
    LOG_E("main", "boot_error reason=%s", reason);
    while (1)
        delay(1000);
}

void setup()
{
    Serial.begin(115200); // USB-CDC: boot/debug diagnostics only

    pinMode(SW_ARM, INPUT_PULLUP);
    pinMode(LED_ARM, OUTPUT);
    pinMode(LED_ACT, OUTPUT);
    digitalWrite(LED_ARM, LOW);
    digitalWrite(LED_ACT, LOW);

    delay(2000);

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!mcpBegin())
        halt("mcp23017_init_failed");

    nodeCfg.addr = TX_ADDR;

    loraSPI.begin(LORA_CLK, LORA_MISO, LORA_MOSI, LORA_CS);

    static RFNode nodeObj(radio, nodeCfg);
    node = &nodeObj;

    node->onSendOk(onSendOk);
    node->onSendFail(onSendFail);

    radio.setTransmitProfile(RadioLibLoRaRadio<SX1262>::RfProfile::HIGH_SPEED);

    BeginStatus bs = node->begin();
    if (bs != BeginStatus::OK)
        halt(beginStatusToStr(bs));
    node->startWorkerTask();

    digitalWrite(LED_ACT, HIGH);
    LOG_I("main", "ready addr=0x%02X", TX_ADDR);
}

void loop()
{
    static uint8_t swStable = 0;
    static uint8_t swRaw = 0;
    static uint32_t swSince = 0;

    const bool armed = digitalRead(SW_ARM) == LOW;
    digitalWrite(LED_ARM, armed ? HIGH : LOW);

    uint8_t gpb = 0;
    if (mcpRead(MCP_GPIOB, gpb))
    {
        uint8_t raw = static_cast<uint8_t>(~gpb) & 0x0F;
        if (raw != swRaw)
        {
            swRaw = raw;
            swSince = millis();
        }
        else if (millis() - swSince >= DEBOUNCE_MS)
        {
            swStable = raw;
        }
    }

    for (int i = 0; i < DRONE_COUNT; ++i)
    {
        const bool held = (swStable & (1u << i)) != 0;

        // Radio events are handled regardless of the arm switch.
        if (ackFlag[i])
        {
            ackFlag[i] = false;
            latchA |= MASK_RX(i);
            killState[i] = KillState::DONE;
        }

        if (failFlag[i])
        {
            failFlag[i] = false;
            latchA |= MASK_FAULT(i);
            if (held)
                sendKill(i);
            else
                killState[i] = KillState::IDLE;
        }

        // The switches only do anything while armed.
        if (!armed)
            continue;

        if (!held)
        {
            // Back to safe — clear this channel and let it fire again.
            killState[i] = KillState::IDLE;
            ackFlag[i] = false;
            failFlag[i] = false;
            latchA &= static_cast<uint8_t>(~(MASK_FAULT(i) | MASK_RX(i)));
            latchB &= static_cast<uint8_t>(~MASK_KILLED(i));
        }
        else if (killState[i] == KillState::IDLE)
        {
            latchB |= MASK_KILLED(i);
            sendKill(i);
        }
    }

    flushLeds();
    delay(POLL_MS);
}
