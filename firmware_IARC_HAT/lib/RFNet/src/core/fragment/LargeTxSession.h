#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../RFConfig.h"

// State machine for a single outgoing fragmented large-message send.
// Stop-and-wait: start() begins; feed each fragment's TX/ACK result to
// onFragmentResult() before sending the next (nextIdx()). Failed fragments
// retry up to fragRetryMax times.
//
// Caller-supplied data pointer must stay valid until session ends
// (onFragmentResult() returns true, or release() called).
class LargeTxSession {
public:
    LargeTxSession();

    // Acquire load, paired with the release store in start().
    bool inUse()          const { return __atomic_load_n(&_used, __ATOMIC_ACQUIRE); }
    bool waitingForPool() const { return _waitingForPool; }

    void start(uint8_t dst, const uint8_t* data, uint16_t totalLen,
               uint8_t msgId, uint8_t total, bool requireAck,
               uint8_t hopCount, uint32_t ackTimeoutMs,
               uint8_t fragRetryMax = RF_FRAG_RETRY_MAX);

    void release() {
        _waitingForPool = false;
        __atomic_store_n(&_used, false, __ATOMIC_RELEASE);
    }
    void setWaitingForPool(bool v) { _waitingForPool = v; }

    // Feed the current fragment's TX/ACK result. Returns true when the
    // session is done (*outSuccess set); false means send nextIdx() again.
    bool onFragmentResult(bool txSuccess, bool* outSuccess);

    // Diagnostics only, not control flow.
    uint8_t fragRetries()   const { return _fragRetries; }
    uint8_t fragRetryMax()  const { return _fragRetryMax; }

    const uint8_t* userData()     const { return _data; }
    uint16_t       userLen()      const { return _totalLen; }
    uint8_t        peer()         const { return _dst; }
    uint8_t        msgId()        const { return _msgId; }
    uint8_t        total()        const { return _total; }
    uint8_t        nextIdx()      const { return _nextIdx; }
    bool           requireAck()   const { return _requireAck; }
    uint8_t        hopCount()     const { return _hopCount; }
    uint32_t       ackTimeoutMs() const { return _ackTimeoutMs; }

private:
    bool           _used           = false;
    bool           _waitingForPool = false;
    uint8_t        _dst            = 0;
    const uint8_t* _data           = nullptr;
    uint16_t       _totalLen       = 0;
    uint8_t        _msgId          = 0;
    uint8_t        _total          = 0;
    uint8_t        _nextIdx        = 0;
    bool           _requireAck     = false;
    uint8_t        _hopCount       = 0;
    uint32_t       _ackTimeoutMs   = 0;
    uint8_t        _fragRetries    = 0;
    uint8_t        _fragRetryMax   = RF_FRAG_RETRY_MAX;
};
