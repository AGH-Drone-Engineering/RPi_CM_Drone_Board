#pragma once
#include "../IClock.h"

// Fallback for platforms with no RTC (bare-metal AVR, etc.). Always reports
// invalid; callers fall back to tickMs-based time tracking.
class NullClock : public IClock {
public:
    bool     isValid() const override { return false; }
    uint64_t epochMs() const override { return 0; }
};
