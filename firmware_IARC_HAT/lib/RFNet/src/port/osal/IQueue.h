#pragma once
#include <stdint.h>
#include <stddef.h>

using OsMs = uint32_t;
static constexpr OsMs OS_NO_WAIT    = 0;
static constexpr OsMs OS_WAIT_FOREVER = UINT32_MAX;

class IQueue {
public:
    virtual ~IQueue() = default;

    // Copied by value; item size fixed at queue creation.
    //
    // ISR SAFETY — REQUIRED: on platforms delivering the radio IRQ in interrupt
    // context (see IRadio::setIrqCallback), the engine calls push() directly
    // from that ISR with OS_NO_WAIT.
    // - Must be non-blocking and ISR-safe: never take a blocking lock or call a
    //   "from-task" RTOS primitive on the ISR path — use the *FromISR variant
    //   (e.g. xQueueSendFromISR on FreeRTOS).
    // - From an ISR, timeoutMs is ignored / treated as OS_NO_WAIT.
    // - Returns false if the queue is full.
    virtual bool push(const void* item, OsMs timeoutMs = OS_NO_WAIT) = 0;

    // Task context only (worker loop / poll()) — never from an ISR.
    virtual bool pop(void* item, OsMs timeoutMs = OS_WAIT_FOREVER) = 0;
};
