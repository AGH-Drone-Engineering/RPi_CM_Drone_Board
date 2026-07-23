#pragma once
#include <stdint.h>
#include <stddef.h>
#include "IQueue.h"
#include "IMutex.h"
#include "ITimer.h"

class IOsal {
public:
    virtual ~IOsal() = default;

    virtual IQueue* createQueue(size_t itemSize, size_t depth) = 0;

    virtual IMutex* createMutex() = 0;

    virtual ITimer* createTimer() = 0;

    virtual bool createTask(void (*fn)(void*), void* arg,
                            const char* name, uint32_t stackBytes, uint8_t priority) = 0;

    // Monotonic ms since boot, 64-bit.
    // - MUST NOT wrap for device's realistic lifetime (wrap stalls duty-cycle deadlines for ~49.7 days).
    // - Ports with a native 32-bit tick must extend it internally.
    virtual uint64_t tickMs() = 0;

    virtual void delayMs(uint32_t ms) = 0;

    virtual uint32_t random() = 0;
};
