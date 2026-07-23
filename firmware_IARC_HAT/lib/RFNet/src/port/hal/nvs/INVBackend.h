#pragma once
#include <stdint.h>
#include <stddef.h>

// Multi-slot raw persistence port. Each slot addressed by a uint16_t id;
// payload is an arbitrary byte blob, size fixed per slot by the caller
// (library uses small structs, typically 4–8 B).
// IDs allocated by the library live in RFNvConfig (RFTypes.h).
// Host apps sharing the same backend should use distinct ids.
class INVBackend {
public:
    virtual ~INVBackend() = default;

    virtual void begin() = 0;

    // false = slot empty / never written / read failed.
    virtual bool read(uint16_t id, void* out, size_t len) = 0;

    // false = write failed (storage full / not ready / oversized payload).
    // Callers persisting protocol-critical state (e.g. the monotonic seq
    // counter) use this to fail loudly instead of silently losing data.
    virtual bool write(uint16_t id, const void* in, size_t len) = 0;

    // True if this backend currently retains data across reboot.
    // - Real NV returns true, but MUST flip to false if its own begin() failed
    //   (a dead store persists nothing).
    // - Otherwise Engine::begin() refuses an encrypted node, since a reset seq
    //   counter would reuse GCM nonces.
    virtual bool isPersistent() const { return true; }
};
