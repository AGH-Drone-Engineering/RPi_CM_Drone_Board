#pragma once
#include <stdint.h>
#include "../RFConfig.h"

// Reassembles fragmented incoming messages from multiple senders.
// - RF_REASM_SLOTS concurrent slots; evicted after inactivity timeout (tick()).
// - Lost slot (eviction/timeout): (src, msgId) remembered so further fragments
//   return Status::Aborted — callers must not ACK those.
// - msgId collision: msgId is 8 bits (Engine::_nextOutgoingMsgId wraps every
//   256 large sends/source); total can coincidentally match too, so a NEW
//   message can reuse (src, msgId, total) of an OLD unfinished slot.
//   ingest() content-compares overlapping repeated indices to catch this, but
//   a disjoint-index collision can still splice two messages into a false
//   Complete — no whole-message MIC. Mitigation, not a guarantee.
//   Needs 256 large sends within one reassembly timeout to trigger; under
//   duty-cycle enforcement (EU868/EU433 default) off-time stretches that to
//   hours (negligible). Realistic only with duty cycle disabled or a
//   denominator-0 channel (e.g. US915).
// - Thread-safety: single worker thread only.
class Reassembler {
public:
    enum class Status { Incomplete, Complete, Dropped, Aborted };

    struct Result {
        Status         status;
        const uint8_t* data;      // valid only when Complete, until next ingest
        uint16_t       totalLen;  // valid only when Complete
    };

    Reassembler();

    // Feed one received fragment. src+msgId identify the message, idx/total
    // its position, data/len the already-decrypted payload.
    Result ingest(uint8_t src, uint8_t msgId, uint8_t idx, uint8_t total,
                  const uint8_t* data, uint8_t len, uint32_t nowMs);

    // Drop slots inactive longer than timeoutMs (also the TTL for aborted
    // entries). Scale timeoutMs to your ACK timeout.
    void tick(uint32_t nowMs, uint32_t timeoutMs = RF_REASM_TIMEOUT_MS);

public:
    // Max fragment count accepted locally, derived from RF_MAX_FRAGMENTED_PAYLOAD
    // (keeps RX/TX limits from drifting apart). ingest() drops totals above this.
    static constexpr uint8_t REASM_MAX_TOTAL =
        (uint8_t)((RF_MAX_FRAGMENTED_PAYLOAD + RF_MAX_PAYLOAD - 1) / RF_MAX_PAYLOAD);

private:
    // Per-slot buffer: REASM_MAX_TOTAL fragments of RF_MAX_PAYLOAD each
    // (RF_MAX_FRAGMENTED_PAYLOAD rounded up). Scales with the knob, not the
    // 16-fragment worst case.
    static constexpr uint16_t REASM_BUF_SIZE =
        (uint16_t)REASM_MAX_TOTAL * RF_MAX_PAYLOAD;

    // ingest()'s memcpy bounds rely on REASM_BUF_SIZE == REASM_MAX_TOTAL *
    // RF_MAX_PAYLOAD; firing means a config change broke that derivation.
    static_assert(REASM_BUF_SIZE >= RF_MAX_FRAGMENTED_PAYLOAD,
                  "slot buffer no longer covers RF_MAX_FRAGMENTED_PAYLOAD — "
                  "reassembly of a max-size message would overflow");
    static_assert((uint32_t)REASM_MAX_TOTAL * RF_MAX_PAYLOAD == REASM_BUF_SIZE,
                  "REASM_BUF_SIZE must be exactly REASM_MAX_TOTAL fragments — "
                  "ingest()'s bounds proof relies on this equality");
    // Re-checked here (RFConfig.h already guarantees it) because ingest()'s
    // bitmap is 32-bit and the wire total field is 4-bit.
    static_assert(REASM_MAX_TOTAL >= 1 && REASM_MAX_TOTAL <= RF_MAX_FRAGMENTS,
                  "REASM_MAX_TOTAL out of the wire format's 1..16 range");

    struct Slot {
        bool     used;
        uint8_t  src;
        uint8_t  msgId;
        uint8_t  total;
        uint32_t bitmap;          // bit i set = fragment idx i received
        uint8_t  buf[REASM_BUF_SIZE];
        uint8_t  lastFragLen;     // length of the last fragment (idx == total-1)
        uint32_t lastActivityMs;
    };
    Slot _slots[RF_REASM_SLOTS];

    // Recently lost (evicted/timed-out) messages; entries expire after
    // _timeoutMs so a wrapped msgId isn't blocked forever.
    // Depth = 2x slot count: a smaller ring could evict an entry while its
    // fragments are still in flight, letting them re-allocate a slot and get
    // ACKed — a false SUCCESS for a dropped message. 2x makes that rare, not
    // impossible: plain FIFO, no rate limiting, so enough evictions within
    // one timeout can still wrap the ring and overwrite the entry first.
    // Mitigation, not a guarantee.
    static constexpr uint8_t ABORTED_DEPTH = 2 * RF_REASM_SLOTS;
    static_assert(RF_REASM_SLOTS <= 127,
                  "ABORTED_DEPTH = 2*RF_REASM_SLOTS must fit uint8_t indexing"
                  " — raise the index types if you truly need >127 slots");
    struct Aborted {
        bool     used;
        uint8_t  src;
        uint8_t  msgId;
        uint32_t atMs;
    };
    Aborted  _aborted[ABORTED_DEPTH];
    uint8_t  _abortedHead = 0;
    uint32_t _timeoutMs   = RF_REASM_TIMEOUT_MS;

    Slot* findOrAlloc(uint8_t src, uint8_t msgId, uint32_t nowMs);
    void  markAborted(uint8_t src, uint8_t msgId, uint32_t nowMs);
    bool  isAborted(uint8_t src, uint8_t msgId, uint32_t nowMs) const;
    static void resetSlotMeta(Slot& s, uint32_t nowMs);
};
