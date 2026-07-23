#pragma once
#include <stdint.h>
#include <stddef.h>

static constexpr uint16_t MAX_PAYLOAD_SIZE = 256;

// Sentinel: "RSSI not provided by backend". Kept as a macro — mirrored in
// core/routing/IRoutingStrategy.h, which must stay includable without this header.
#ifndef RF_RSSI_UNKNOWN
#define RF_RSSI_UNKNOWN INT16_MIN
#endif

struct RadioPacket {
    uint8_t  data[MAX_PAYLOAD_SIZE];
    uint16_t length;
    // RX signal strength in dBm. RF_RSSI_UNKNOWN = "not provided by backend" —
    // routing strategies consulting RSSI must treat this value as unknown.
    int16_t rssi = RF_RSSI_UNKNOWN;
};

// Convert a backend RSSI reading (float dBm) to RadioPacket::rssi.
// - Positive readings clamp to 0 dBm (nothing legitimate is louder).
// - Low end clamps at -32767 dBm, only to keep a physically impossible
//   reading from aliasing the RF_RSSI_UNKNOWN sentinel.
static inline int16_t rssiToInt16(float rssiDbm) {
    if (rssiDbm >= 0.0f)      return 0;
    if (rssiDbm <= -32767.0f) return -32767;
    return (int16_t)rssiDbm;
}

// Outcome of a blocking TX attempt. Three failure modes, kept distinct
// because recovery differs:
// - CHANNEL_BUSY: back off / accept loss (environment loud, not a fault).
// - RADIO_ERROR: reset the radio (HW/SPI trouble).
// - BAD_FRAME: fix the caller (precondition violation — length, null buffer, not initialised).
enum class TxResult : uint8_t {
    OK = 0,
    CHANNEL_BUSY,   // CCA/CAD detected activity on all attempts
    RADIO_ERROR,    // hardware/transmit error
    BAD_FRAME,      // caller-side contract violation (length/null/not init)
};

// Outcome of a CCA probe. Kept separate from TxResult: caller decides policy
// (retry now / defer / send anyway for critical frames), not the HAL.
enum class CcaResult : uint8_t {
    CLEAR = 0,   // channel quiet, safe to transmit
    BUSY  = 1,   // activity detected (LoRa preamble or RSSI above threshold)
};

// Typed radio event drained from the backend after a hardware IRQ wake-up.
// Backends translate chip-specific flag bits/GDO state/status registers into this set.
// Engine only loops pollEvent() until NONE — never inspects raw chip flags.
// New chip = implement pollEvent(); engine stays untouched.
struct RadioEvent {
    enum Type : uint8_t {
        NONE = 0,    // No more events pending — caller stops draining.
        RX_DONE,     // Frame arrived; call readPacket() to fetch it and learn the CRC result.
        TX_DONE,     // Transmission finished (informational).
        CRC_ERROR,   // Frame received but CRC failed; no packet to read.
    };
    Type type;
};

// ─── L2 framing contract (read this before adding a backend) ─────────────
//
// Every backend MUST present a uniform wire view to Engine:
//
//   on the air:  [ L2 dst (1 B) ] [ L3 frame (header+payload+tag) ]
//   to Engine:                    [ L3 frame                      ]
//
// • send(data, len, addr): `data` is the L3 frame. HAL prepends `addr` as
//   on-air byte 0 (chip packet engine on FSK, manual copy on LoRa). Engine
//   never includes the L2 byte in `data` and never needs to know whether
//   filtering is HW or SW.
//
// • readPacket(out): HAL inspects byte 0 of the on-air frame (the L2 dst),
//   drops frames matching neither the local address nor ADDR_BROADCAST
//   (0xFF), delivers only the L3 portion in `out->data`/`out->length`.
//   Engine still re-checks via PacketParser::isForMe() against the L3 dst —
//   the HAL-side filter is an energy optimisation, not a security boundary.
//
// • getAirtimeMs(frameLen): `frameLen` is the L3 frame size. HAL adds the
//   1-byte L2 overhead internally so duty-cycle accounting reflects what
//   actually goes on the air.
//
// • setLocalAddress(addr): HAL records `addr` for the readPacket filter
//   (and the chip's HW packet engine, if any). Returns true for HW-assisted
//   filtering, false for SW-only — both are functionally correct.
// ─────────────────────────────────────────────────────────────────────────

class IRadio {
public:
    virtual ~IRadio() = default;

    virtual bool init()   = 0;
    virtual bool finish() = 0;

    // Set the ISR callback. Implementation attaches this to the HW interrupt
    // pin (e.g. DIO1 for SX1262). MUST run in ISR context and be as fast as possible.
    virtual void setIrqCallback(void (*cb)(void*), void* ctx) = 0;

    // Probe the channel before TX. Chip/modulation-specific: LoRa does CAD +
    // RSSI sweep, FSK does RSSI sweep only.
    // `timeoutMs`: caps the RSSI sweep duration (CAD is chip-internal, SF-dependent).
    // `rssiThresholdDbm`: busy floor — frames louder than this count as activity.
    // Engine (not the HAL) decides whether to call this before each send().
    virtual CcaResult cca(uint32_t timeoutMs, int8_t rssiThresholdDbm) = 0;

    // Send a packet. Blocks until transmitted or an error occurs.
    // `data`/`length`: L3 frame only — HAL prepends the L2 dst byte (see
    // contract above). No CCA inside; caller must call cca() first if needed.
    //
    // Returns RADIO_ERROR on HW/transmit failure, BAD_FRAME on caller
    // precondition violation (zero/oversized length, null data, not
    // initialised). Never returns CHANNEL_BUSY — that's cca()'s job.
    virtual TxResult send(const uint8_t* data, uint16_t length,
                          uint8_t addr = 0) = 0;

    // Put the radio into continuous RX mode. Returns immediately.
    virtual void startReceive() = 0;

    // Put the radio into standby mode (stops RX/TX).
    virtual void standby() = 0;

    // Read the packet from the radio's HW buffer via SPI. Must be called
    // from normal thread context (NOT ISR).
    // Returns true if a frame addressed to this node (or broadcast) was
    // delivered; false if the chip buffer was empty/oversized or the L2
    // filter dropped the frame.
    // Only valid after pollEvent() returns RX_DONE — otherwise yields stale
    // data (e.g. SX126x RxBufferStatus caches the last length).
    virtual bool readPacket(RadioPacket* outPacket) = 0;

    // Drain one pending HW event in task context, consuming the underlying
    // chip flag/state. Returns NONE when nothing remains, so caller can loop
    // until drained. Must NOT be called from ISR.
    virtual RadioEvent pollEvent() = 0;

    // Time-on-air for an L3 frame of `frameLen` bytes at the radio's current
    // modulation, in ms. HAL adds L2 overhead internally — caller doesn't
    // need to know about it. Backends without a meaningful answer may
    // return 0 (Engine skips jitter, uses loose duty estimates).
    virtual uint32_t getAirtimeMs(uint16_t frameLen) const = 0;

    // Per-frame chip payload limit (bytes), incl. L2 byte — largest frame
    // the chip can physically TX/RX in one shot. Engine cross-checks this
    // at begin() against RF_MAX_PAYLOAD + headers + tag to fail fast on
    // misconfiguration. 0 = unknown (no check performed).
    virtual uint16_t getMaxPayloadSize() const { return 0; }

    // Regulatory duty-cycle denominator for the radio's current channel
    // (100 = 1%, 10 = 10%, 0 = no limit/unknown). This is the *legal floor* —
    // Engine may apply a stricter override but must not loosen below it.
    // Backends that don't know their region return 0; user must then set an
    // override explicitly.
    virtual uint16_t getRegulatoryDutyDenominator() const { return 0; }

    // Program the local node address for L2-byte filtering (HW packet engine
    // on FSK chips, SW comparison on LoRa). Returns true if a HW filter was
    // installed (RX energy saving), false if filtered in SW — both modes
    // deliver the same frames to Engine.
    virtual bool setLocalAddress(uint8_t addr) { (void)addr; return false; }
};
