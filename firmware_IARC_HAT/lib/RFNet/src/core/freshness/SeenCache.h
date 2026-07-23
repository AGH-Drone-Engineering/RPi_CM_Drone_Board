#pragma once
#include <stdint.h>
#include "../RFConfig.h"

// ACK dedupe. ACK seq echoes the originator's counter (non-monotonic from
// this src's view), so a sliding window doesn't apply — FIFO of recent
// (src, dst, seq, ack) tuples instead, sized to ackTimeoutMs relevance.
// dst MUST be part of the identity: originators' counters collide routinely,
// so without it, B's ACKs to two originators sharing an echoed seq would
// dedupe as one.
//
// Data frames use ReplayWindow instead (per-peer monotonic counter + window).

class SeenCache {
public:
    bool checkAndMark(uint8_t src, uint8_t dst, uint32_t seq, uint8_t ack) {
        seq &= 0xFFFFFF;
        ack &= 0x1;
        for (uint8_t i = 0; i < SIZE; ++i) {
            if (_buf[i].valid && _buf[i].src == src && _buf[i].dst == dst &&
                _buf[i].seq == seq && _buf[i].ack == ack)
                return true;
        }
        _buf[_head] = { src, dst, seq, ack, 1 };
        _head = static_cast<uint8_t>((_head + 1) % SIZE);
        return false;
    }

    void clear() {
        for (auto& e : _buf) e.valid = 0;
        _head = 0;
    }

private:
    static constexpr uint8_t SIZE = RF_SEEN_CACHE_SIZE;
    struct Entry {
        uint8_t  src;
        uint8_t  dst;
        uint32_t seq   : 24;
        uint32_t ack   : 1;
        uint32_t valid : 1;
    };
    Entry   _buf[SIZE] = {};
    uint8_t _head      = 0;
};
