#pragma once
#include <stdint.h>
#include "../RFConfig.h"

// Per-peer sliding-window anti-replay for DATA frames (ack=0).
// - Per peer: 24-bit lastSeq + WINDOW_BITS history bitmap (bit i = lastSeq-i
//   accepted). Older-than-window frames dropped. New peer: free slot or LRU
//   evict. Wraparound: signed-distance comparison, correct across 16M rollover.
//
// SECURITY LIMITATIONS (RAM only, not persisted):
// - First contact seeds lastSeq unconditionally → a keyless attacker who
//   captured one authenticated frame (GCM replay needs no key) replays it
//   once per acceptance-state reset: reboot, or LRU eviction past
//   RF_REPLAY_MAX_PEERS (set >= deployment's originator count).
// - Transit forwarders see unauthenticated headers → attacker can forge
//   (src, far-future seq) to jump a victim's lastSeq, suppressing its real
//   frames there (targeted DoS). Fix: give transit nodes the key.
// - SAFETY-CRITICAL: don't rely on this alone — add an app-level freshness
//   token. MonotonicCounter (sender nonce) IS persisted, so seq itself can't
//   be replayed fresh; only receiver acceptance state resets.

class ReplayWindow {
public:
    bool isReplay(uint8_t src, uint32_t seq) {
        seq &= SEQ_MASK;
        Peer* p = findOrAlloc(src);
        p->lru  = ++_tick;

        if (!p->valid) {
            p->valid   = true;
            p->src     = src;
            p->lastSeq = seq;
            p->bitmap  = 1u;
            return false;
        }

        uint32_t raw = (seq - p->lastSeq) & SEQ_MASK;
        int32_t  d   = (raw & SEQ_SIGN) ? (int32_t)(raw | 0xFF000000u)
                                        : (int32_t)raw;

        if (d > 0) {
            uint32_t shift = (uint32_t)d;
            p->bitmap  = (shift >= WINDOW_BITS) ? 0u
                                                : (p->bitmap << shift);
            p->bitmap |= 1u;
            p->lastSeq = seq;
            return false;
        }

        uint32_t back = (uint32_t)(-d);
        if (back >= WINDOW_BITS) return true;
        uint32_t mask = (1u << back);
        if (p->bitmap & mask) return true;
        p->bitmap |= mask;
        return false;
    }

    void clear() {
        for (auto& p : _t) p.valid = false;
        _tick = 0;
    }

private:
    static constexpr uint8_t  MAX_PEERS   = RF_REPLAY_MAX_PEERS;
    static constexpr uint8_t  WINDOW_BITS = RF_REPLAY_WINDOW_BITS;
    static constexpr uint32_t SEQ_MASK    = 0xFFFFFFu;
    static constexpr uint32_t SEQ_SIGN    = 0x800000u;

    struct Peer {
        uint8_t  src;
        uint32_t lastSeq;
        uint32_t bitmap;   // history bitmap; see WINDOW_BITS above
        uint32_t lru;
        bool     valid;
    };
    Peer     _t[MAX_PEERS] = {};
    uint32_t _tick         = 0;

    Peer* findOrAlloc(uint8_t src) {
        Peer* empty = nullptr;
        for (auto& p : _t) {
            if (p.valid && p.src == src) return &p;
            if (!p.valid && !empty)      empty = &p;
        }
        if (empty) return empty;

        Peer* victim = &_t[0];
        for (auto& p : _t) if (p.lru < victim->lru) victim = &p;
        victim->valid = false;
        return victim;
    }
};
