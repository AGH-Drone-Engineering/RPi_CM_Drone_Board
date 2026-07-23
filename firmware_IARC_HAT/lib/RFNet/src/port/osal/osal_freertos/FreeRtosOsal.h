#pragma once
// FreeRTOS OSAL — default on ESP32 / ESP-IDF.
// - push() satisfies IQueue::push's ISR-safety contract (see IQueue.h) via ISR-context
//   detection and xQueueSendFromISR.
// - createTask's stack size is bytes (ESP-IDF), not StackType_t words.
//
// Self-guarded to ESP32/ARDUINO_ARCH_ESP32: RFNode.h includes every OSAL unconditionally,
// and other targets lack freertos/*.h.
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include "../IOsal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_random.h"
#include "esp_timer.h"

class FreeRtosQueue : public IQueue {
public:
    explicit FreeRtosQueue(QueueHandle_t h) : _h(h) {}
    ~FreeRtosQueue() override { if (_h) vQueueDelete(_h); }

    bool push(const void* item, OsMs timeoutMs = OS_NO_WAIT) override {
        if (xPortInIsrContext()) {
            BaseType_t higherPrioWoken = pdFALSE;
            bool ok = xQueueSendFromISR(_h, item, &higherPrioWoken) == pdTRUE;
            portYIELD_FROM_ISR(higherPrioWoken);
            return ok;
        }
        TickType_t t = (timeoutMs == OS_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeoutMs);
        return xQueueSend(_h, item, t) == pdTRUE;
    }

    bool pop(void* item, OsMs timeoutMs = OS_WAIT_FOREVER) override {
        TickType_t t = (timeoutMs == OS_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeoutMs);
        return xQueueReceive(_h, item, t) == pdTRUE;
    }

private:
    QueueHandle_t _h;
};

class FreeRtosMutex : public IMutex {
public:
    explicit FreeRtosMutex(SemaphoreHandle_t h) : _h(h) {}
    ~FreeRtosMutex() override { if (_h) vSemaphoreDelete(_h); }

    bool lock(OsMs timeoutMs = OS_WAIT_FOREVER) override {
        TickType_t t = (timeoutMs == OS_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeoutMs);
        return xSemaphoreTake(_h, t) == pdTRUE;
    }

    void unlock() override { xSemaphoreGive(_h); }

private:
    SemaphoreHandle_t _h;
};

class FreeRtosTimer : public ITimer {
public:
    FreeRtosTimer() = default;

    ~FreeRtosTimer() override {
        if (_h) {
            xTimerStop(_h, 0);
            xTimerDelete(_h, 0);
        }
    }

    void start(OsMs periodMs, bool repeat, void (*cb)(void*), void* arg) override {
        _cb     = cb;
        _arg    = arg;
        _repeat = repeat;

        if (_h) { xTimerStop(_h, 0); xTimerDelete(_h, 0); }

        _h = xTimerCreate(
            "osal_tmr",
            pdMS_TO_TICKS(periodMs),
            repeat ? pdTRUE : pdFALSE,
            this,
            [](TimerHandle_t t) {
                auto* self = static_cast<FreeRtosTimer*>(pvTimerGetTimerID(t));
                if (self && self->_cb) self->_cb(self->_arg);
            });

        if (_h) xTimerStart(_h, 0);
    }

    void stop() override {
        if (_h) xTimerStop(_h, 0);
    }

private:
    TimerHandle_t  _h      = nullptr;
    void (*_cb)(void*)     = nullptr;
    void*          _arg    = nullptr;
    bool           _repeat = false;
};

class FreeRtosOsal : public IOsal {
public:
    ~FreeRtosOsal() override = default;

    IQueue* createQueue(size_t itemSize, size_t depth) override {
        QueueHandle_t h = xQueueCreate(depth, itemSize);
        return h ? new FreeRtosQueue(h) : nullptr;
    }

    IMutex* createMutex() override {
        SemaphoreHandle_t h = xSemaphoreCreateMutex();
        return h ? new FreeRtosMutex(h) : nullptr;
    }

    ITimer* createTimer() override { return new FreeRtosTimer(); }

    bool createTask(void (*fn)(void*), void* arg,
                    const char* name, uint32_t stackBytes, uint8_t priority) override {
        // Do not divide stackBytes by sizeof(StackType_t) (see file header).
        //
        // FreeRTOS tasks must never return — wrap Engine's worker (which does) so it
        // self-deletes the task instead.
        struct Tramp { void (*fn)(void*); void* arg; };
        auto* t = new Tramp{fn, arg};
        BaseType_t ok = xTaskCreate(
            [](void* pv) {
                auto* tr = static_cast<Tramp*>(pv);
                tr->fn(tr->arg);
                delete tr;
                vTaskDelete(nullptr);
            },
            name, stackBytes, t, priority, nullptr);
        if (ok != pdPASS) { delete t; return false; }
        return true;
    }

    uint64_t tickMs() override {
        // esp_timer is 64-bit; xTaskGetTickCount is 32-bit and wraps.
        return (uint64_t)esp_timer_get_time() / 1000u;
    }

    void delayMs(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }

    uint32_t random() override { return esp_random(); }
};

#endif // ESP32 || ARDUINO_ARCH_ESP32
