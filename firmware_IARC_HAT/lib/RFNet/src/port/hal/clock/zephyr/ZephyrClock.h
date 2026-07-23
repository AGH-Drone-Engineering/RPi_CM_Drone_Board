#pragma once
#include "../IClock.h"

#if defined(__ZEPHYR__)
#include <time.h>
#include <zephyr/kernel.h>

class ZephyrClock : public IClock {
public:
    bool isValid() const override {
        struct timespec ts{};
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return false;
        // Anything before 2020-01-01 means the RTC was never synced.
        return ts.tv_sec > 1577836800;
    }

    uint64_t epochMs() const override {
        struct timespec ts{};
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
    }
};
#endif
