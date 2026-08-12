#include <RFNode.h>
#include <cstdio>
#include <cstring>

// Build stamp, same generator and same fallback as UartRfBridge.h - which this
// variant does not include, since it bridges nothing.
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef CMDB_ESP_FIRMWARE_BUILD
#define CMDB_ESP_FIRMWARE_BUILD "unknown"
#endif

// TEMPORARY - passive channel monitor, built by env:sniffer.
//
// Same board, same radio settings and same key as main_node.cpp, but it accepts
// every frame on the air instead of only its own and never transmits. The
// promiscuity itself lives in RFNet behind RF_PROMISCUOUS (see
// lib/RFNet/src/core/RFConfig.h); this file only sets the node up and dumps what
// arrives.
//
// Output goes to USB-CDC, so `pio device monitor -e sniffer` is the whole UI.
// One line per frame, emitted by Engine because that is the only place holding
// both the header fields and the decrypted payload:
//   [I][Sniff] src=0x01 dst=0x05 seq=42 len=17 rssi=-73 AREQ ENC | 47 45 54 ...
// ACK frames appear the same way, flagged ACK. This file adds a line only for a
// message reassembled from fragments, which Engine cannot see as a whole.
//
// This is NOT a node. It cannot ACK (the path is compiled out), so never leave
// it running on an address a real sender targets - every send to it fails with
// ACK_TIMEOUT after burning the full retry budget.

#if !RF_PROMISCUOUS
#error "main_sniffer.cpp requires -D RF_PROMISCUOUS=1 (see env:sniffer in platformio.ini)"
#endif

// Boot diagnostics: everything through, so a failed begin() is visible.
static void logBoot(const char *str)
{
    Serial.print(str);
}

// Installed once the monitor is live, to keep the capture readable.
//
// Filtering by module rather than by level, because LOGGER_LEVEL cannot separate
// these: the frame line from Engine is LOG_I, which is exactly the level its own
// bookkeeping sits at, so LOGGER_LEVEL=2 would drop the capture and keep the
// noise. Doing it in the write function keeps it inside this temporary file
// instead of teaching Logger a module-filter API it does not otherwise need.
static void logSniffOnly(const char *str)
{
    // _logLine (port/Logger.h) emits "[I][Sniff] ..." for the levelled macros
    // and "[Sniff] ..." for the bare log(). Errors pass whatever their module:
    // a monitor that went quiet because the radio faulted must not look like a
    // quiet channel.
    const bool isError = str[0] == '[' && str[1] == 'E';
    if (!isError && strstr(str, "[Sniff]") == nullptr)
        return;
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

// LoRa module pinout on the IARC HAT (SX1262) - identical to main_node.cpp.
#define LORA_CS 38
#define LORA_IRQ 4
#define LORA_RST 39
#define LORA_BUSY 5

#define LORA_RF_SW 2
#define LORA_CLK 40
#define LORA_MOSI 41
#define LORA_MISO 42

#define KILLSWITCH_FC_CTL 1
#define KILLSWITCH_PSU_CTL 6

// Address the monitor claims. Every filter that would use it is compiled out,
// so it only has to be a legal RFNet address (1..0xFE) - but it must not collide
// with a real node: a sender targeting it would never get an ACK back. 0x7F is
// outside the 1..31 range the JP1..JP5 jumpers can produce.
#ifndef SNIFFER_ADDR
#define SNIFFER_ADDR 0x7F
#endif

static SPIClass loraSPI;

// NETWORK INVARIANT: channel, mode and password must match the nodes being
// watched, or nothing decodes. Kept in sync with main_node.cpp by hand.
static SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU869_DC10,
                             LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, loraSPI);

static RFNodeConfig nodeCfg = []()
{
    RFNodeConfig c;
    c.addr = SNIFFER_ADDR;
    c.mode = PacketMode::P2P;
    c.security = RFSecurityConfig::FromPassword("bajer");
    c.dutyCycle.enabled = false;
    return c;
}();

static RFNode *node = nullptr;

// Engine prints one complete line per frame on the air, payload included, so
// single-frame traffic needs nothing here. What Engine cannot show is a message
// that arrived in fragments: it logs each fragment separately and only RFNet
// knows when the reassembled whole is ready. That is the one case this adds.
//
// Runs on the RF worker task, so keep it short - a slow callback stalls reception.
static void onFrame(const RxInfo &info, const uint8_t *data, size_t len, void *)
{
    if (len <= RF_MAX_PAYLOAD)
        return; // came in one frame; Engine already dumped these exact bytes

    // 3 chars per byte ("AB "), plus NUL. A reassembled message reaches
    // RF_MAX_FRAGMENTED_PAYLOAD, well past this - the head identifies the
    // traffic, and the fragment lines above carry the rest verbatim.
    static char hex[RF_MAX_PAYLOAD * 3 + 1];

    size_t pos = 0;
    for (size_t i = 0; i < RF_MAX_PAYLOAD; ++i)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
    hex[pos] = '\0';

    // Module "Sniff", not "main", so logSniffOnly lets it through.
    LOG_I("Sniff", "src=0x%02X REASSEMBLED len=%u | %s...", info.from,
          (unsigned)len, hex);
}

void setup()
{
    Serial.begin(115200);
    Logger::setWriteFn(&logBoot);

    // Same states main_node.cpp settles on once it is armed. The monitor has no
    // killswitch logic, but if this image ever lands on a HAT wired to an
    // aircraft, leaving the gates floating would be worse than holding them on.
    pinMode(KILLSWITCH_FC_CTL, OUTPUT);
    pinMode(KILLSWITCH_PSU_CTL, OUTPUT);
    digitalWrite(KILLSWITCH_FC_CTL, HIGH);
    digitalWrite(KILLSWITCH_PSU_CTL, HIGH);

    delay(2000); // let the USB-CDC host attach before the boot banner

    loraSPI.begin(LORA_CLK, LORA_MISO, LORA_MOSI, LORA_CS);

    static RFNode nodeObj(radio, nodeCfg);
    node = &nodeObj;
    node->onReceive(onFrame, nullptr);

    radio.setTransmitProfile(RadioLibLoRaRadio<SX1262>::RfProfile::HIGH_SPEED);

    BeginStatus bs = node->begin();
    if (bs != BeginStatus::OK)
    {
        LOG_E("main", "boot_error reason=%s", beginStatusToStr(bs));
        while (1)
            delay(1000);
    }

    // Double the default 4096: both log lines per frame are formatted on this
    // task, and _logLine puts a LOGGER_BUFFER_SIZE-byte line buffer on the stack
    // (1024 here, to fit a full-payload hex dump) on top of Engine's own frame
    // buffers.
    if (!node->startWorkerTask(8192))
    {
        LOG_E("main", "boot_error reason=worker_task_start_failed");
        while (1)
            delay(1000);
    }

    LOG_W("main", "PROMISCUOUS monitor addr=0x%02X build=%s - RX only, never ACKs",
          (unsigned)SNIFFER_ADDR, CMDB_ESP_FIRMWARE_BUILD);
    LOG_W("main", "capture follows; only [Sniff] lines and errors from here on");

    // Everything above was boot diagnostics. From here the log is the capture.
    Logger::setWriteFn(&logSniffOnly);
}

void loop()
{
    // Everything happens on the RF worker task.
    delay(1000);
}
