#pragma once
#include <stdint.h>
#include "../RFConfig.h"
#include "../../port/osal/IOsal.h"
#include "../../port/osal/IMutex.h"
#include "../../port/hal/clock/IClock.h"
#include "../../port/hal/nvs/INVBackend.h"
#include "../../port/Logger.h"

// ETSI EN 300 220 duty-cycle enforcement via "minimum off-time" (Toff) rule:
// after transmitting for Ton, no further TX allowed until Toff = Ton * (denom - 1)
// has elapsed (denom: 100 = 1%, 10 = 10%).
// With a valid IClock + NV backend, off-deadline survives reboot; without one
// it's tick-based and resets on power-cycle.
class DutyCycleTracker {
public:
    DutyCycleTracker(IOsal& osal) : _osal(osal) {}

    // clock/nv/mux optional (null = no wallclock/persistence/locking).
    // windowMs: compliance window for isAttainable(). commitMinDeltaMs: min change
    // in off-deadline before re-committing to NV (amortises flash wear).
    // initialDenom: whether to load persisted state at init (0 = skip).
    void init(uint16_t initialDenom,
              IClock*  clock            = nullptr,
              INVBackend* nv            = nullptr,
              uint16_t nvId             = 2,
              uint32_t commitMinDeltaMs = 2000,
              uint32_t windowMs         = 3600000,
              IMutex*  mux              = nullptr) {
        _mux              = mux;
        _clock            = clock;
        _nv               = nv;
        _nvId             = nvId;
        _commitMinDeltaMs = commitMinDeltaMs;
        _windowMs         = windowMs;
        _clockValid       = (_clock && _clock->isValid());
        _offUntilMs       = 0;   // free to transmit
        _lastSavedOffMs   = 0;

        if (initialDenom != 0 && _nv) _loadFromNv();
    }

    // True if the current off-time has elapsed (or denom == 0, disabled).
    bool canTransmit(uint16_t denom, uint32_t toaMs) const {
        (void)toaMs;
        if (denom == 0) return true;
        _Lock g(_mux);
        _syncBase();
        return _now() >= _offUntilMs;
    }

    // Milliseconds until a frame of `toaMs` may be transmitted (0 = now).
    uint32_t getWaitMs(uint16_t denom, uint32_t toaMs) const {
        (void)toaMs;
        if (denom == 0) return 0;
        _Lock g(_mux);
        _syncBase();
        uint64_t now = _now();
        if (now >= _offUntilMs) return 0;
        uint64_t wait = _offUntilMs - now;
        return wait > UINT32_MAX ? UINT32_MAX : (uint32_t)wait;
    }

    // Same check as canTransmit(), without needing a frame size.
    bool hasAnyBudget(uint16_t denom) const {
        if (denom == 0) return true;
        _Lock g(_mux);
        _syncBase();
        return _now() >= _offUntilMs;
    }

    // Whether a frame of `toaMs` could ever be sent, vs. just needing to wait.
    bool isAttainable(uint16_t denom, uint32_t toaMs) const {
        if (denom == 0) return true;
        return toaMs <= (_windowMs / denom);
    }

    // Record a transmission of `timeOnAirMs` and arm the off-time before the
    // next one. Must be called exactly once per frame actually put on air.
    void addTransmission(uint16_t denom, uint32_t timeOnAirMs) {
        if (denom == 0) return;
        _Lock g(_mux);
        _syncBase();
        // Start counting from max(now, current deadline) so back-to-back sends
        // accumulate their off-times instead of overlapping.
        uint64_t base = _now();
        if (_offUntilMs > base) base = _offUntilMs;
        _offUntilMs = base + (uint64_t)timeOnAirMs * (denom > 0 ? (denom - 1u) : 0u);
        _maybeCommit();
    }

private:
    // RAII lock guard; null mutex skips locking.
    struct _Lock {
        IMutex* m;
        explicit _Lock(IMutex* mx) : m(mx) { if (m) m->lock(); }
        ~_Lock() { if (m) m->unlock(); }
        _Lock(const _Lock&) = delete;
        _Lock& operator=(const _Lock&) = delete;
    };

    // Epoch-ms when the clock is valid, else ticks since boot.
    uint64_t _now() const {
        return (_clock && _clock->isValid()) ? _clock->epochMs()
                                             : _osal.tickMs();
    }

    // Rebase _offUntilMs when clock validity changes.
    void _syncBase() const {
        bool nowValid = (_clock && _clock->isValid());
        if (nowValid == _clockValid) return;

        if (nowValid && !_clockValid) {
            // tick → epoch: carry over the remaining wait.
            uint64_t tickNow   = _osal.tickMs();
            uint64_t remaining = (_offUntilMs > tickNow) ? (_offUntilMs - tickNow) : 0;
            _offUntilMs = _clock->epochMs() + remaining;
            _loadFromNv();
        } else {
            // epoch → tick: drop the deadline instead of guessing.
            _offUntilMs = _osal.tickMs();
        }
        _clockValid = nowValid;
    }

    // Persisted record: the off-deadline as an epoch second (0 = no valid
    // wallclock at save time, so it can't be honoured across reboot).
    struct PersistedState {
        uint16_t version;
        uint16_t reserved;
        uint32_t offUntilEpochS;
    };
    static constexpr uint16_t STATE_VERSION = 2;   // bump when PersistedState's layout changes
    static_assert(sizeof(PersistedState) + 1 <= RF_NV_SLOT_BYTES,
                  "RF_NV_SLOT_BYTES too small for DutyCycleTracker::PersistedState"
                  " + 1 marker byte — raise RF_NV_SLOT_BYTES");

    // Restore the persisted off-deadline, if any. Called from init() and
    // again from _syncBase() once the clock becomes valid.
    void _loadFromNv() const {
        if (!_nv) return;
        PersistedState s{};
        if (!_nv->read(_nvId, &s, sizeof(s))) return;
        if (s.version != STATE_VERSION)       return;
        if (s.offUntilEpochS == 0)            return;   // saved without a clock
        if (!(_clock && _clock->isValid()))   return;   // can't compare without one

        uint64_t nowMs      = _clock->epochMs();
        uint64_t offUntilMs = (uint64_t)s.offUntilEpochS * 1000u;
        // Only restore if still in the future and later than what's pending.
        if (offUntilMs > nowMs && offUntilMs > _offUntilMs) {
            _offUntilMs     = offUntilMs;
            _lastSavedOffMs = offUntilMs;
        }
    }

    void _maybeCommit() {
        if (!_nv) return;
        // Nothing meaningful to persist without a wallclock (tick resets on
        // reboot, so the deadline can't be honoured across a power cycle).
        if (!(_clock && _clock->isValid())) return;

        uint64_t delta = (_offUntilMs > _lastSavedOffMs)
                             ? (_offUntilMs - _lastSavedOffMs)
                             : (_lastSavedOffMs - _offUntilMs);
        if (delta < _commitMinDeltaMs) return;

        PersistedState s{
            STATE_VERSION,
            0,
            (uint32_t)((_offUntilMs + 999u) / 1000u),
        };
        if (!_nv->write(_nvId, &s, sizeof(s))) {
            LOG_W("Duty", "NV write failed (id=%u) — off-time won't survive reboot",
                  (unsigned)_nvId);
        }
        _lastSavedOffMs = _offUntilMs;
    }

    IOsal&      _osal;
    IMutex*     _mux              = nullptr;
    IClock*     _clock            = nullptr;
    INVBackend* _nv               = nullptr;
    uint16_t    _nvId             = 0;
    uint32_t    _windowMs         = 3600000;
    uint32_t    _commitMinDeltaMs = 0;

    // Deadline before which no transmission is allowed (epoch-ms or tick-ms).
    mutable uint64_t _offUntilMs     = 0;
    mutable bool     _clockValid     = false;
    mutable uint64_t _lastSavedOffMs = 0;
};
