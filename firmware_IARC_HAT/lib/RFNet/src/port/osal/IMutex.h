#pragma once
#include "IQueue.h"

class IMutex {
public:
    virtual ~IMutex() = default;

    virtual bool lock(OsMs timeoutMs = OS_WAIT_FOREVER) = 0;
    virtual void unlock() = 0;
};
