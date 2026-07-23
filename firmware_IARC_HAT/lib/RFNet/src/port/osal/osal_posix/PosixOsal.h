#pragma once
// POSIX OSAL for host-side unit/integration tests (Linux, macOS, Windows-MinGW).
// - push/pop block via condvar timeouts.
// - Timers run on dedicated pthreads.
//
// Self-guarded to host builds, matching PosixClock.h's guard — RFNode.h includes every
// OSAL unconditionally and embedded targets lack pthread.h.
#if !defined(__ZEPHYR__) && !defined(ESP32) && !defined(ARDUINO_ARCH_ESP32) && !defined(ARDUINO)
#include "../IOsal.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <random>

class PosixQueue : public IQueue {
public:
    PosixQueue(size_t itemSize, size_t depth)
        : _itemSize(itemSize), _depth(depth),
          _buf(new uint8_t[itemSize * depth]),
          _head(0), _tail(0), _count(0)
    {
        pthread_mutex_init(&_mtx, nullptr);
        pthread_cond_init(&_notEmpty, nullptr);
        pthread_cond_init(&_notFull, nullptr);
    }

    ~PosixQueue() override {
        pthread_cond_destroy(&_notFull);
        pthread_cond_destroy(&_notEmpty);
        pthread_mutex_destroy(&_mtx);
        delete[] _buf;
    }

    // No real ISR context on host builds; blocking here is safe.
    bool push(const void* item, OsMs timeoutMs = OS_NO_WAIT) override {
        pthread_mutex_lock(&_mtx);
        if (_count >= _depth) {
            if (timeoutMs == OS_NO_WAIT) { pthread_mutex_unlock(&_mtx); return false; }
            if (timeoutMs == OS_WAIT_FOREVER) {
                while (_count >= _depth) pthread_cond_wait(&_notFull, &_mtx);
            } else {
                struct timespec ts = _deadline(timeoutMs);
                while (_count >= _depth) {
                    if (pthread_cond_timedwait(&_notFull, &_mtx, &ts) != 0) {
                        pthread_mutex_unlock(&_mtx); return false;
                    }
                }
            }
        }
        memcpy(_buf + _tail * _itemSize, item, _itemSize);
        _tail = (_tail + 1) % _depth;
        ++_count;
        pthread_cond_signal(&_notEmpty);
        pthread_mutex_unlock(&_mtx);
        return true;
    }

    bool pop(void* item, OsMs timeoutMs = OS_WAIT_FOREVER) override {
        pthread_mutex_lock(&_mtx);
        if (_count == 0) {
            if (timeoutMs == OS_NO_WAIT) { pthread_mutex_unlock(&_mtx); return false; }
            if (timeoutMs == OS_WAIT_FOREVER) {
                while (_count == 0) pthread_cond_wait(&_notEmpty, &_mtx);
            } else {
                struct timespec ts = _deadline(timeoutMs);
                while (_count == 0) {
                    if (pthread_cond_timedwait(&_notEmpty, &_mtx, &ts) != 0) {
                        pthread_mutex_unlock(&_mtx); return false;
                    }
                }
            }
        }
        memcpy(item, _buf + _head * _itemSize, _itemSize);
        _head = (_head + 1) % _depth;
        --_count;
        pthread_cond_signal(&_notFull);
        pthread_mutex_unlock(&_mtx);
        return true;
    }

private:
    size_t          _itemSize, _depth;
    uint8_t*        _buf;
    size_t          _head, _tail, _count;
    pthread_mutex_t _mtx;
    pthread_cond_t  _notEmpty;
    pthread_cond_t  _notFull;

    static struct timespec _deadline(OsMs ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += (ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        return ts;
    }
};

class PosixMutex : public IMutex {
public:
    PosixMutex()  { pthread_mutex_init(&_mtx, nullptr); }
    ~PosixMutex() { pthread_mutex_destroy(&_mtx); }

    bool lock(OsMs timeoutMs = OS_WAIT_FOREVER) override {
        if (timeoutMs == OS_WAIT_FOREVER) return pthread_mutex_lock(&_mtx) == 0;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeoutMs / 1000;
        ts.tv_nsec += (timeoutMs % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        return pthread_mutex_timedlock(&_mtx, &ts) == 0;
    }

    void unlock() override { pthread_mutex_unlock(&_mtx); }

private:
    pthread_mutex_t _mtx;
};

class PosixTimer : public ITimer {
public:
    PosixTimer() {
        pthread_mutex_init(&_mtx, nullptr);
        pthread_cond_init(&_cond, nullptr);
    }

    ~PosixTimer() override {
        stop();
        pthread_cond_destroy(&_cond);
        pthread_mutex_destroy(&_mtx);
    }

    void start(OsMs periodMs, bool repeat, void (*cb)(void*), void* arg) override {
        stop();
        _cb = cb; _arg = arg; _period = periodMs; _repeat = repeat; _running = true;
        pthread_create(&_thread, nullptr, _run, this);
    }

    void stop() override {
        if (!_running) return;
        pthread_mutex_lock(&_mtx);
        _running = false;
        pthread_cond_signal(&_cond);
        pthread_mutex_unlock(&_mtx);
        pthread_join(_thread, nullptr);
    }

private:
    void (*_cb)(void*) = nullptr;
    void*           _arg     = nullptr;
    OsMs            _period  = 0;
    bool            _repeat  = false;
    bool            _running = false;
    pthread_t       _thread  = 0;
    pthread_mutex_t _mtx;
    pthread_cond_t  _cond;

    static void* _run(void* pv) {
        auto* self = static_cast<PosixTimer*>(pv);
        pthread_mutex_lock(&self->_mtx);
        while (self->_running) {
            struct timespec ts = _deadline(self->_period);
            int rc = pthread_cond_timedwait(&self->_cond, &self->_mtx, &ts);
            if (!self->_running) break;
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&self->_mtx);
                if (self->_cb) self->_cb(self->_arg);
                pthread_mutex_lock(&self->_mtx);
                if (!self->_repeat) self->_running = false;
            }
        }
        pthread_mutex_unlock(&self->_mtx);
        return nullptr;
    }

    static struct timespec _deadline(OsMs ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        return ts;
    }
};

class PosixOsal : public IOsal {
public:
    ~PosixOsal() override = default;

    IQueue* createQueue(size_t itemSize, size_t depth) override {
        return new PosixQueue(itemSize, depth);
    }

    IMutex* createMutex() override { return new PosixMutex(); }

    ITimer* createTimer() override { return new PosixTimer(); }

    bool createTask(void (*fn)(void*), void* arg,
                    const char* /*name*/, uint32_t /*stack*/, uint8_t /*prio*/) override {
        pthread_t tid;
        struct Ctx { void (*fn)(void*); void* arg; };
        auto* ctx = new Ctx{fn, arg};
        if (pthread_create(&tid, nullptr,
            [](void* p) -> void* {
                auto* c = static_cast<Ctx*>(p);
                c->fn(c->arg);
                delete c;
                return nullptr;
            }, ctx) != 0) {
            delete ctx;
            return false;
        }
        // Detach: nobody joins worker threads.
        pthread_detach(tid);
        return true;
    }

    uint64_t tickMs() override {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
    }

    void delayMs(uint32_t ms) override { usleep(ms * 1000u); }

    // std::random_device-seeded mt19937, not libc rand(): unseeded rand() yields the identical
    // sequence on every process start, so a fleet of nodes restarting together (shared power
    // event, OTA rollout) would compute identical jitter/backoff delays and collide in
    // lockstep instead of decorrelating.
    uint32_t random() override { return _rng(); }

private:
    std::mt19937 _rng{std::random_device{}()};
};

#endif // !__ZEPHYR__ && !ESP32 && !ARDUINO_ARCH_ESP32 && !ARDUINO
