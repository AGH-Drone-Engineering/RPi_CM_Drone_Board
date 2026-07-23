#pragma once
#include "IQueue.h"

class ITimer {
public:
    virtual ~ITimer() = default;

    virtual void start(OsMs periodMs, bool repeat, void (*cb)(void*), void* arg) = 0;

    virtual void stop() = 0;
};
