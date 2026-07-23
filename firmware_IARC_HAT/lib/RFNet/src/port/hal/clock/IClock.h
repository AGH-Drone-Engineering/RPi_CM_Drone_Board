#pragma once
#include <stdint.h>

// Wall-clock time port. Read-only: no setters, no begin() — implementations
// derive state from the platform (RTC, SNTP, etc.).
// isValid() may be false at cold boot and flip true later (e.g. ESP32 after SNTP sync).
// Callers caching results must re-check periodically.
class IClock {
public:
    virtual ~IClock() = default;

    // True once synchronised against a trusted source. Persisted-state regeneration logic must gate on this.
    virtual bool isValid() const = 0;

    // Milliseconds since Unix epoch. Meaningful only when isValid().
    // 64-bit avoids overflow until year 584.5M; safer than uint32 seconds.
    virtual uint64_t epochMs() const = 0;
};
