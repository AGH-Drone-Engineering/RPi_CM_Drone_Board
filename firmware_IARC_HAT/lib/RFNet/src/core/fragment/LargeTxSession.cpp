#include "LargeTxSession.h"

LargeTxSession::LargeTxSession() {}

void LargeTxSession::start(uint8_t dst, const uint8_t* data, uint16_t totalLen,
                            uint8_t msgId, uint8_t total, bool requireAck,
                            uint8_t hopCount, uint32_t ackTimeoutMs,
                            uint8_t fragRetryMax) {
    _waitingForPool = false;
    _dst            = dst;
    _data           = data;
    _totalLen       = totalLen;
    _msgId          = msgId;
    _total          = total;
    _nextIdx        = 0;
    _requireAck     = requireAck;
    _hopCount       = hopCount;
    _ackTimeoutMs   = ackTimeoutMs;
    _fragRetries    = 0;
    _fragRetryMax   = fragRetryMax;
    // Release store: published last, paired with the acquire load in inUse().
    __atomic_store_n(&_used, true, __ATOMIC_RELEASE);
}

bool LargeTxSession::onFragmentResult(bool txSuccess, bool* outSuccess) {
    if (!txSuccess) {
        if (_fragRetries < _fragRetryMax) {
            _fragRetries++;
            return false;  // session continues — caller re-sends same fragment
        }
        *outSuccess = false;
        return true;  // retry budget exhausted — caller should release
    }

    _fragRetries = 0;
    _nextIdx++;

    if (_nextIdx >= _total) {
        *outSuccess = true;
        return true;  // all fragments delivered
    }

    return false;  // more fragments remain
}
