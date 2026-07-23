#pragma once
#include <stdint.h>
#include "../RFConfig.h"
#include "../../port/hal/nvs/INVBackend.h"
#include "../../port/Logger.h"

// Persisted record: fixed fingerprint + counter value. Slot-mapped NV backends
// add a 1-byte "written" marker on top. Checked here so shrinking RF_NV_SLOT_BYTES
// fails the build instead of silently breaking seq persistence (nonce reuse
// after reboot).
struct MonoCtrRecord {
    uint32_t magic;
    uint32_t value;
};
static_assert(sizeof(MonoCtrRecord) + 1 <= RF_NV_SLOT_BYTES,
              "RF_NV_SLOT_BYTES too small for MonotonicCounter's persisted "
              "record + 1 marker byte — raise RF_NV_SLOT_BYTES");

// Replay-safe monotonic counter.
// - begin() skips ahead of the last committed value by `skip`, so a crash
//   before the next commit can't reuse a value. Commits every `skip`
//   increments to amortize NV wear.
// - read()==false means "never written" OR "read failed" (INVBackend.h).
//   Treating both as "never written" would let a transient error/bit-rot
//   reset the counter to `skip`, reusing GCM nonces from the current key.
// - Magic fingerprint catches readable-but-wrong bytes (foreign/corrupted) as
//   a hard failure via corrupted(), never a silent reset to 0 — but not
//   read() itself returning false for an unreadable existing record; that's
//   an inherent gap in a boolean present/absent API, not papered over.
class MonotonicCounter {
public:
    static constexpr uint32_t kMagic = 0x524E4331u;  // "RNC1"

    MonotonicCounter(INVBackend& nv, uint16_t id, uint32_t skip)
        : _nv(nv), _id(id), _skip(skip) {}

    void begin() {
        MonoCtrRecord rec{};
        uint32_t saved = 0;
        if (_nv.read(_id, &rec, sizeof(rec))) {
            if (rec.magic == kMagic) {
                saved = rec.value;
            } else {
                _corrupted = true;
                LOG_E("MonoCtr", "NV record at id=%u failed the integrity check "
                      "(unrecognised fingerprint) — refusing to guess a safe seq; "
                      "wipe the slot deliberately to reprovision this counter",
                      (unsigned)_id);
            }
        }
        _lastSaved = saved;
        _current   = saved + _skip;
        if (!_corrupted) _commit();
    }

    uint32_t get() const { return _current; }

    // True when begin() found a readable-but-unrecognised record at this id
    // (see class comment). A caller binding this counter to a fixed-key
    // nonce must refuse to run rather than trust get() while this is true.
    bool corrupted() const { return _corrupted; }

    void increment() {
        ++_current;
        if (_current >= _lastSaved + _skip) _commit();
    }

private:
    void _commit() {
        // Only advance _lastSaved on a CONFIRMED write.
        MonoCtrRecord rec{kMagic, _current};
        if (_nv.write(_id, &rec, sizeof(rec))) {
            _lastSaved = _current;
        } else {
            LOG_E("MonoCtr", "NV write failed (id=%u) — will retry next increment(); "
                  "seq will regress after reboot if power is lost before a retry succeeds",
                  (unsigned)_id);
        }
    }

    INVBackend& _nv;
    uint16_t    _id;
    uint32_t    _skip;
    uint32_t    _current   = 0;
    uint32_t    _lastSaved = 0;
    bool        _corrupted = false;
};
