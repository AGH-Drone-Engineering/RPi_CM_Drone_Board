#pragma once
#include "../IClock.h"

// Host build (Linux/macOS/Windows-MinGW) — used by tests. Always valid.
#if !defined(__ZEPHYR__) && !defined(ESP32) && !defined(ARDUINO_ARCH_ESP32) && !defined(ARDUINO)
#include <sys/time.h>

class PosixClock : public IClock {
public:
    bool isValid() const override { return true; }

    uint64_t epochMs() const override {
        struct timeval tv{};
        if (gettimeofday(&tv, nullptr) != 0) return 0;
        return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
    }
};
#endif
