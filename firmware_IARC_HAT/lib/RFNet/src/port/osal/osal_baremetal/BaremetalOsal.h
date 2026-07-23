#pragma once
// Single-threaded bare-metal OSAL — default on non-ESP Arduino (AVR, ARM).
// - createTask() always fails; drive the node via poll() instead.
// - Mutex is a no-op (single-threaded).
// - Queue push satisfies IQueue::push's ISR-safety contract (see IQueue.h) via a
//   brief interrupt-disable guard.
//
// Tick function: automatic (millis()) on Arduino; a required constructor argument elsewhere.
#include "../IOsal.h"
#include <stdint.h>
#include <string.h>

#if defined(ARDUINO)
#  include <Arduino.h>
   // RAII interrupt guard around the queue's critical sections.
   struct BaremetalIrqGuard {
       BaremetalIrqGuard()  { noInterrupts(); }
       ~BaremetalIrqGuard() { interrupts(); }
   };
#else
   struct BaremetalIrqGuard {};
#endif

class BaremetalQueue : public IQueue {
public:
    BaremetalQueue(size_t itemSize, size_t depth)
        : _itemSize(itemSize), _depth(depth),
          _buf(new uint8_t[itemSize * depth]),
          _head(0), _tail(0), _count(0) {}

    ~BaremetalQueue() override { delete[] _buf; }

    bool push(const void* item, OsMs /*timeoutMs*/ = OS_NO_WAIT) override {
        BaremetalIrqGuard g;
        if (_count >= _depth) return false;
        memcpy(_buf + _tail * _itemSize, item, _itemSize);
        _tail = (_tail + 1) % _depth;
        ++_count;
        return true;
    }

    bool pop(void* item, OsMs timeoutMs = OS_WAIT_FOREVER) override {
        const uint32_t start = _tickFn ? _tickFn() : 0;
        for (;;) {
            {
                BaremetalIrqGuard g;
                if (_count > 0) {
                    memcpy(item, _buf + _head * _itemSize, _itemSize);
                    _head = (_head + 1) % _depth;
                    --_count;
                    return true;
                }
            }
            if (timeoutMs == OS_NO_WAIT) return false;
            if (timeoutMs != OS_WAIT_FOREVER && _tickFn) {
                if ((_tickFn() - start) >= timeoutMs) return false;
            }
            // An ISR push can land between spins here.
        }
    }

    static uint32_t (*_tickFn)();

private:
    size_t   _itemSize;
    size_t   _depth;
    uint8_t* _buf;
    size_t   _head, _tail, _count;
};

class BaremetalMutex : public IMutex {
public:
    bool lock(OsMs /*timeoutMs*/ = OS_WAIT_FOREVER) override { return true; }
    void unlock() override {}
};

class BaremetalTimer : public ITimer {
public:
    void start(OsMs periodMs, bool repeat, void (*cb)(void*), void* arg) override {
        _cb      = cb;
        _arg     = arg;
        _period  = periodMs;
        _repeat  = repeat;
        _active  = true;
        _started = BaremetalQueue::_tickFn ? BaremetalQueue::_tickFn() : 0;
    }

    void stop() override { _active = false; }

    void service() {
        if (!_active || !_cb || !BaremetalQueue::_tickFn) return;
        uint32_t now = BaremetalQueue::_tickFn();
        if ((now - _started) >= _period) {
            _cb(_arg);
            if (_repeat) _started = now;
            else         _active  = false;
        }
    }

private:
    void (*_cb)(void*) = nullptr;
    void*    _arg      = nullptr;
    OsMs     _period   = 0;
    bool     _repeat   = false;
    bool     _active   = false;
    uint32_t _started  = 0;
};

class BaremetalOsal : public IOsal {
public:
#if defined(ARDUINO)
    // Defaults to millis(); override via the one-arg constructor or setTickFn().
    BaremetalOsal() { BaremetalQueue::_tickFn = _arduinoMillis; }
    explicit BaremetalOsal(uint32_t (*tickFn)()) {
        // nullptr falls back to millis().
        BaremetalQueue::_tickFn = tickFn ? tickFn : _arduinoMillis;
    }
#else
    // Deleted: without a tick source, timeouts would silently never fire.
    BaremetalOsal() = delete;
    explicit BaremetalOsal(uint32_t (*tickFn)()) { BaremetalQueue::_tickFn = tickFn; }
#endif

    ~BaremetalOsal() override = default;

    void setTickFn(uint32_t (*fn)()) { if (fn) BaremetalQueue::_tickFn = fn; }

    IQueue* createQueue(size_t itemSize, size_t depth) override {
        return new BaremetalQueue(itemSize, depth);
    }

    IMutex* createMutex() override { return new BaremetalMutex(); }

    ITimer* createTimer() override { return new BaremetalTimer(); }

    bool createTask(void (*/*fn*/)(void*), void* /*arg*/,
                    const char* /*name*/, uint32_t /*stack*/, uint8_t /*prio*/) override {
        // No scheduler. Must return false, or Engine treats poll() as a no-op.
        return false;
    }

    uint64_t tickMs() override {
        if (!BaremetalQueue::_tickFn) return 0;
        // Extends the 32-bit tick source (wraps ~every 49.7 days) to IOsal's
        // 64-bit no-wrap contract.
        uint32_t t = BaremetalQueue::_tickFn();
        if (t < _lastTick32) _tickWraps++;
        _lastTick32 = t;
        return ((uint64_t)_tickWraps << 32) | t;
    }

    void delayMs(uint32_t ms) override {
        if (!BaremetalQueue::_tickFn) return;
        uint32_t start = BaremetalQueue::_tickFn();
        while ((BaremetalQueue::_tickFn() - start) < ms) {}
    }

    // xorshift32, lazily seeded from elapsed tick time on first use (not a fixed constant).
    // - Fixed seed: every device / every reboot repeats the same jitter/backoff sequence, so a
    //   fleet power-cycling together (shared outage, battery swap, OTA rollout) collides in
    //   lockstep instead of decorrelating.
    // - Tick-derived seed is still weak entropy, not cryptographic — call setSeed() with real
    //   entropy (ADC noise, a HW RNG) if available.
    uint32_t random() override {
        if (!_rngSeeded) {
            uint32_t t = BaremetalQueue::_tickFn ? BaremetalQueue::_tickFn() : 0;
            _rngState  = t ^ 0x9e3779b9u;
            if (_rngState == 0) _rngState = 0xDEADBEEFu;  // xorshift can't start at 0
            _rngSeeded = true;
        }
        _rngState ^= _rngState << 13;
        _rngState ^= _rngState >> 17;
        _rngState ^= _rngState << 5;
        return _rngState;
    }

    // Inject real entropy (ADC noise, a HW RNG) instead of the tick-derived fallback above.
    // Re-seeds immediately; 0 is rejected (xorshift's fixed point) and falls back to the
    // constant instead.
    void setSeed(uint32_t seed) {
        _rngState  = seed ? seed : 0xDEADBEEFu;
        _rngSeeded = true;
    }

private:
#if defined(ARDUINO)
    // millis() returns unsigned long, not uint32_t.
    static uint32_t _arduinoMillis() { return (uint32_t)millis(); }
#endif

    // 32-to-64-bit extension state for the tick source; see tickMs().
    uint32_t _lastTick32 = 0;
    uint32_t _tickWraps  = 0;

    // xorshift32 state for random(); see there.
    uint32_t _rngState  = 0xDEADBEEFu;
    bool     _rngSeeded = false;
};
