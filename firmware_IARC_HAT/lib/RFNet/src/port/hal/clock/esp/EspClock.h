#pragma once
#include "../IClock.h"

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

class EspClock : public IClock {
public:
    bool isValid() const override {
        // Stays true across subsequent re-syncs.
        return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
    }

    uint64_t epochMs() const override {
        struct timeval tv{};
        if (gettimeofday(&tv, nullptr) != 0) return 0;
        return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
    }
};
#endif
