#include "Reassembler.h"
#include "../../port/Logger.h"
#include <string.h>

Reassembler::Reassembler() {
    for (auto& s : _slots)   s = {};
    for (auto& a : _aborted) a = {};
}

void Reassembler::markAborted(uint8_t src, uint8_t msgId, uint32_t nowMs) {
    _aborted[_abortedHead] = { true, src, msgId, nowMs };
    _abortedHead = static_cast<uint8_t>((_abortedHead + 1) % ABORTED_DEPTH);
}

bool Reassembler::isAborted(uint8_t src, uint8_t msgId, uint32_t nowMs) const {
    for (const auto& a : _aborted) {
        if (!a.used || a.src != src || a.msgId != msgId) continue;
        if (nowMs - a.atMs > _timeoutMs) continue;
        return true;
    }
    return false;
}

Reassembler::Slot* Reassembler::findOrAlloc(uint8_t src, uint8_t msgId, uint32_t nowMs) {
    Slot* freeSlot = nullptr;
    Slot* lruUsed  = nullptr;

    for (auto& s : _slots) {
        if (s.used && s.src == src && s.msgId == msgId) return &s;
        if (!s.used) {
            if (!freeSlot) freeSlot = &s;
        } else {
            if (!lruUsed || s.lastActivityMs < lruUsed->lastActivityMs) lruUsed = &s;
        }
    }

    Slot* chosen = freeSlot ? freeSlot : lruUsed;
    if (chosen && chosen->used) {
        LOG_W("Reassembler",
              "evicting slot src=0x%02X msgId=%u for new src=0x%02X msgId=%u",
              chosen->src, chosen->msgId, src, msgId);
        markAborted(chosen->src, chosen->msgId, nowMs);
    }
    if (chosen) {
        resetSlotMeta(*chosen, nowMs);
    }
    return chosen;
}

// buf is not zeroed here: every read is gated by bitmap, so stale bytes
// are never observed.
void Reassembler::resetSlotMeta(Slot& s, uint32_t nowMs) {
    s.used           = false;
    s.total          = 0;
    s.bitmap         = 0;
    s.lastFragLen    = 0;
    s.lastActivityMs = nowMs;
}

Reassembler::Result Reassembler::ingest(uint8_t src, uint8_t msgId,
                                         uint8_t idx, uint8_t total,
                                         const uint8_t* data, uint8_t len,
                                         uint32_t nowMs) {
    // ── Validation happens BEFORE any slot lookup/allocation ──────────────
    // Rejecting first avoids letting a stream of invalid fragments evict
    // in-progress reassemblies via LRU.
    if (idx >= total || total > RF_MAX_FRAGMENTS || total < 1) {
        return { Status::Dropped, nullptr, 0 };
    }

    // total > REASM_MAX_TOTAL (peer's RF_MAX_FRAGMENTED_PAYLOAD is larger).
    if (total > REASM_MAX_TOTAL) {
        LOG_W("Reassembler",
              "total=%u exceeds local cap %u (peer's RF_MAX_FRAGMENTED_PAYLOAD"
              " larger than ours) src=0x%02X msgId=%u",
              total, REASM_MAX_TOTAL, src, msgId);
        return { Status::Dropped, nullptr, 0 };
    }

    // Non-last fragments must be exactly RF_MAX_PAYLOAD; a mismatch means a
    // peer with a different RF_MAX_PAYLOAD.
    bool isLast = (idx == total - 1u);
    if (!isLast && len != RF_MAX_PAYLOAD) {
        LOG_W("Reassembler",
              "non-last fragment wrong size: idx=%u len=%u src=0x%02X",
              idx, len, src);
        return { Status::Dropped, nullptr, 0 };
    }
    if (isLast && (len == 0 || len > RF_MAX_PAYLOAD)) {
        LOG_W("Reassembler",
              "last fragment invalid size: idx=%u len=%u src=0x%02X",
              idx, len, src);
        return { Status::Dropped, nullptr, 0 };
    }

    // Already lost: refuse before allocating a slot.
    if (isAborted(src, msgId, nowMs)) {
        return { Status::Aborted, nullptr, 0 };
    }

    Slot* s = findOrAlloc(src, msgId, nowMs);
    if (!s) return { Status::Dropped, nullptr, 0 };

    bool isNew = !s->used;

    // Deliberately does NOT reset on idx==0: fragments can arrive out of
    // order (mesh + jitter), so a repeated idx=0 is normal, not a restart.

    // total mismatch: sender restarted with this msgId.
    if (!isNew && s->total != 0 && s->total != total) {
        LOG_W("Reassembler",
              "total mismatch src=0x%02X msgId=%u: had %u got %u",
              src, msgId, s->total, total);
        resetSlotMeta(*s, nowMs);
        isNew = true;
    }

    if (isNew) {
        s->used   = true;
        s->src    = src;
        s->msgId  = msgId;
        s->total  = total;
        s->bitmap = 0;
    }

    uint32_t bit = (1u << idx);
    if (s->bitmap & bit) {
        const uint8_t storedLen = isLast ? s->lastFragLen : (uint8_t)RF_MAX_PAYLOAD;
        const bool matches = (len == storedLen) &&
            memcmp(s->buf + (size_t)idx * RF_MAX_PAYLOAD, data, len) == 0;
        if (matches) return { Status::Incomplete, nullptr, 0 };

        LOG_W("Reassembler",
              "msgId/total collision src=0x%02X msgId=%u total=%u idx=%u: content "
              "differs from the in-progress slot — treating as a new message",
              src, (unsigned)msgId, (unsigned)total, (unsigned)idx);
        resetSlotMeta(*s, nowMs);
        s->used   = true;
        s->src    = src;
        s->msgId  = msgId;
        s->total  = total;
        s->bitmap = 0;
        bit = (1u << idx);
    }

    // In bounds by construction: validation above bounds idx and len; the
    // header static_asserts pin REASM_BUF_SIZE to match.
    memcpy(s->buf + (size_t)idx * RF_MAX_PAYLOAD, data, len);
    if (isLast) s->lastFragLen = len;

    s->bitmap          |= bit;
    s->lastActivityMs   = nowMs;

    uint32_t fullMask = (total < 32u) ? ((1u << total) - 1u) : 0xFFFFFFFFu;
    if ((s->bitmap & fullMask) != fullMask) return { Status::Incomplete, nullptr, 0 };

    uint16_t totalLen = (uint16_t)((total - 1u) * RF_MAX_PAYLOAD + s->lastFragLen);
    const uint8_t* resultData = s->buf;

    s->used = false;

    return { Status::Complete, resultData, totalLen };
}

void Reassembler::tick(uint32_t nowMs, uint32_t timeoutMs) {
    _timeoutMs = timeoutMs;
    for (auto& s : _slots) {
        if (!s.used) continue;
        if (nowMs - s.lastActivityMs > timeoutMs) {
            LOG_W("Reassembler",
                  "timeout src=0x%02X msgId=%u bitmap=0x%08lX",
                  s.src, s.msgId, (unsigned long)s.bitmap);
            markAborted(s.src, s.msgId, nowMs);
            s.used = false;
        }
    }
}
