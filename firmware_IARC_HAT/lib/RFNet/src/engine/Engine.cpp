#include "Engine.h"
#include "../core/packet/PacketBuilder.h"
#include "../core/packet/PacketParser.h"
#include "../port/Logger.h"
#include <string.h>

Engine::~Engine() { end(); }

Engine::Engine(IRadio& radio, IOsal& osal, const EngineConfig& cfg)
    : _radio(radio), _osal(osal), _cfg(cfg),
      _nv(cfg.nv.backend ? cfg.nv.backend : &_nvOwned),
      _clock(cfg.clock ? cfg.clock : &_clockOwned),
      _duty(osal),
      _nvSeq(*_nv, _cfg.nv.idSeq, _cfg.nv.nonceCommitStep)
{
    for (auto& s : _pool)    s.used = false;
    for (auto& p : _pending) { p.msg = nullptr; p.used = false; }
}

BeginStatus Engine::begin() {
    if (_cfg.myAddr == ADDR_BROADCAST) {
        LOG_E("Engine", "begin: myAddr 0xFF reserved for broadcast");
        return BeginStatus::INVALID_CONFIG;
    }
    // 0x00 also rejected: untouched-config default (collision risk); CC1101-class
    // chips treat it as a second HW broadcast.
    if (_cfg.myAddr == 0x00) {
        LOG_E("Engine", "begin: myAddr 0x00 reserved — set an address in [0x01, 0xFE]");
        return BeginStatus::INVALID_CONFIG;
    }
    if (_cfg.hopCount > 0x07u) {
        LOG_E("Engine", "begin: hopCount %u exceeds 3-bit field (max 7)",
              (unsigned)_cfg.hopCount);
        return BeginStatus::INVALID_CONFIG;
    }
    if (!_poolMux) {
        _poolMux = _osal.createMutex();
        if (!_poolMux) { LOG_E("Engine", "begin: mutex alloc failed"); return BeginStatus::OUT_OF_MEMORY; }
    }
    if (!_dutyMux) {
        _dutyMux = _osal.createMutex();
        if (!_dutyMux) { LOG_E("Engine", "begin: duty mutex alloc failed"); return BeginStatus::OUT_OF_MEMORY; }
    }
    if (!_eventQueue) {
        _eventQueue = _osal.createQueue(sizeof(EngineEvent), RF_EVENT_QUEUE_DEPTH);
        if (!_eventQueue) { LOG_E("Engine", "begin: event queue alloc failed"); return BeginStatus::OUT_OF_MEMORY; }
    }

    _radio.setIrqCallback([](void* ctx) {
        auto* self = static_cast<Engine*>(ctx);
        if (self->_eventQueue) {
            EngineEvent evt{RFEvent::HW_IRQ, nullptr};
            self->_eventQueue->push(&evt, OS_NO_WAIT);
        }
    }, this);

    _nv->begin();

    // Volatile NV is a silent security/compliance trap — surfaced loudly here.
    if (!_nv->isPersistent()) {
        if (_cfg.sec) {
            // Same hazard as BeginStatus::NV_NOT_PERSISTENT: seq resets on reboot
            // -> GCM nonce reuse -> GHASH subkey leak -> tag forgery.
#ifdef RF_ALLOW_VOLATILE_NV
            LOG_W("Engine", "begin: NV backend is volatile and encryption is ON — seq "
                  "resets every reboot => GCM (key,nonce) REUSE. Continuing ONLY because "
                  "RF_ALLOW_VOLATILE_NV is defined; do NOT ship this build");
#else
            LOG_E("Engine", "begin: NV backend is volatile but encryption is ON — refusing to "
                  "start (seq would reset every reboot and reuse GCM nonces under a fixed key). "
                  "Provide a persistent INVBackend via cfg.nv.backend, or define "
                  "RF_ALLOW_VOLATILE_NV to override on non-production builds");
            return BeginStatus::NV_NOT_PERSISTENT;
#endif
        }
        if (_cfg.dutyCycleEnabled) {
            // Regulatory (not security): lost Toff deadline on reboot; repeated
            // restarts can exceed the legal air-time limit.
            LOG_W("Engine", "begin: NV backend is volatile — duty-cycle off-time is lost on "
                  "reboot; repeated restarts can exceed the regulatory air-time limit");
        }
    }

    // Only decides whether to load persisted state; _currentDutyDenom() re-resolves
    // the denom on later calls.
    _duty.init(_currentDutyDenom(), _clock, _nv,
               _cfg.nv.idDuty, _cfg.nv.dutyMinCommitMs,
               /*windowMs=*/3600000, _dutyMux);

    _nvSeq.begin();
    if (_cfg.sec && _nvSeq.corrupted()) {
        // Same hazard as NV_NOT_PERSISTENT: trusting an unrecognised seq record (or
        // silently resetting it to 0) risks GCM (key, nonce) reuse under the fixed key.
        LOG_E("Engine", "begin: seq counter NV record failed its integrity check — "
              "refusing to start an encrypted node on an unverifiable seq (see "
              "MonoCtr log above); wipe the NV slot deliberately to reprovision, "
              "or point nv.idSeq at an unused id");
        return BeginStatus::NV_SEQ_CORRUPTED;
    }
    // Seed msgId from the persistent seq counter (not 0): prevents a reboot colliding
    // with a receiver's live reassembly slot (fragments have no whole-message MIC).
    _nextOutgoingMsgId = (uint8_t)_nvSeq.get();
    if (!_radio.init()) return BeginStatus::RADIO_INIT_FAILED;

    // Catches a runtime frame-size limit RFConfig.h's compile-time static_assert
    // can't know (e.g. an FSK chip in fixed-length mode).
    const uint16_t chipMax = _radio.getMaxPayloadSize();
    const uint16_t frameMax = RF_MAX_PAYLOAD + PACKET_HEADER_SIZE_FRAG + CRYPTO_TAG_SIZE + 1;
    if (chipMax != 0 && chipMax < frameMax) {
        LOG_E("Engine", "begin: radio max frame %u < required %u — RF_MAX_PAYLOAD too large "
              "for this chip; shrink RF_MAX_PAYLOAD or use a chip with a larger FIFO",
              (unsigned)chipMax, (unsigned)frameMax);
        return BeginStatus::INVALID_CONFIG;
    }

    _radio.setLocalAddress(_cfg.myAddr);
    // Needed here too: poll()-driven apps have no worker task to arm RX before first TX.
    _radio.startReceive();
    LOG_I("Engine", "begin OK addr=0x%02X mode=%u", (unsigned)_cfg.myAddr,
          (unsigned)_cfg.mode);
    return BeginStatus::OK;
}

uint16_t Engine::_currentDutyDenom() const {
    if (!_cfg.dutyCycleEnabled) return 0;
    const uint16_t reg = _radio.getRegulatoryDutyDenominator();
    const uint16_t ovr = _cfg.dutyCycleDenominatorOverride;
    if (ovr == 0)     return reg;
    if (reg == 0)     return ovr;
    // Higher denominator = stricter; override may tighten but never loosen below
    // the regulatory floor.
    return (ovr >= reg) ? ovr : reg;
}

bool Engine::end() {
    // Signal a running worker to stop.
    if (_workerStarted) {
        _workerStarted      = false;
        _shuttingDownWorker = true;
        if (_eventQueue) {
            EngineEvent evt{RFEvent::SHUTDOWN, nullptr};
            _eventQueue->push(&evt, OS_NO_WAIT);
        }
    }

    if (_shuttingDownWorker) {
        for (int i = 0; i < RF_WORKER_SHUTDOWN_TIMEOUT_MS && !_workerExited; i++)
            _osal.delayMs(1);

        // Worker resuming later would use-after-free; detach the IRQ and bail
        // without freeing.
        if (!_workerExited) {
            LOG_E("Engine", "end: worker did not exit within %d ms — deferring teardown, "
                  "engine kept alive to avoid use-after-free (retry end() after your callback returns)",
                  RF_WORKER_SHUTDOWN_TIMEOUT_MS);
            _radio.setIrqCallback(nullptr, nullptr);
            return false;
        }
        _shuttingDownWorker = false;
    }

    if (_osObjectsFreed) return true;
    _radio.setIrqCallback(nullptr, nullptr);
    _radio.finish();
    delete _eventQueue; _eventQueue = nullptr;
    delete _poolMux;    _poolMux    = nullptr;
    delete _dutyMux;    _dutyMux    = nullptr;
    _osObjectsFreed = true;
    return true;
}

void Engine::onSendSuccess(EngineSendCb     cb, void* ctx) { _cbSuccess = cb; _ctxSuccess = ctx; }
void Engine::onSendFailed (EngineSendFailCb cb, void* ctx) { _cbFailed  = cb; _ctxFailed  = ctx; }
void Engine::onReceive    (EngineRecvCb     cb, void* ctx) { _cbRecv    = cb; _ctxRecv    = ctx; }

RFMessage* Engine::_poolAlloc(bool forFragment) {
    // Non-fragment callers see only the first POOL-SESSIONS slots; tail is the
    // fragment reserve (see declaration comment).
    const size_t limit = forFragment ? RF_POOL_SIZE
                                     : RF_POOL_SIZE - RF_LARGE_TX_SESSIONS;
    RFMessage* result = nullptr;
    _poolMux->lock();
    for (size_t i = 0; i < limit; i++) {
        if (!_pool[i].used) { _pool[i].used = true; result = &_pool[i].msg; break; }
    }
    _poolMux->unlock();
    return result;
}

void Engine::_poolFree(RFMessage* msg) {
    if (!msg) return;
    _poolMux->lock();
    for (auto& s : _pool) {
        if (&s.msg == msg) { s.used = false; break; }
    }
    _poolMux->unlock();
}

RFMessage* Engine::acquire() {
    RFMessage* msg = _poolAlloc();
    if (msg) {
        // Reset fields a caller might leave unset: prevents a recycled slot
        // smuggling stale waitAck/hopCount into the next send.
        msg->payloadLen   = 0;
        msg->waitAck      = false;
        msg->ackTimeoutMs = 0;
        msg->hopCount     = _cfg.hopCount;
    }
    return msg;
}

SendStatus Engine::send(RFMessage* msg) {
    if (!msg) return SendStatus::POOL_EXHAUSTED;
    if (msg->payloadLen == 0 || msg->payloadLen > RF_MAX_PAYLOAD) {
        LOG_W("Engine", "send: bad payloadLen %u", (unsigned)msg->payloadLen);
        _poolFree(msg); return SendStatus::BAD_LENGTH;
    }
    if (!_eventQueue) {
        LOG_E("Engine", "send: call begin() first");
        _poolFree(msg); return SendStatus::NOT_INITIALIZED;
    }
    msg->_isFragment     = false;
    msg->_fragSessionIdx = -1;
    msg->_largePayload   = nullptr;
    msg->_largePayloadLen = 0;
    msg->_jitterApplied  = false;
    EngineEvent evt{RFEvent::TX_REQ, msg};
    if (!_eventQueue->push(&evt, OS_NO_WAIT)) {
        LOG_W("Engine", "send: TX queue full");
        _poolFree(msg); return SendStatus::QUEUE_FULL;
    }
    return SendStatus::OK;
}

SendStatus Engine::send(uint8_t dst, const uint8_t* data, size_t len) {
    if (len == 0 || len > RF_MAX_PAYLOAD) {
        LOG_W("Engine", "send: bad len %u", (unsigned)len);
        return SendStatus::BAD_LENGTH;
    }
    RFMessage* msg = _poolAlloc();
    if (!msg) { LOG_W("Engine", "send: pool exhausted"); return SendStatus::POOL_EXHAUSTED; }
    msg->peer             = dst;
    msg->payloadLen       = static_cast<uint8_t>(len);
    msg->waitAck          = false;
    msg->ackTimeoutMs     = 0;
    // Inherit configured hop budget; 0 would silently disable mesh forwarding.
    msg->hopCount         = _cfg.hopCount;
    msg->_isFragment      = false;
    msg->_fragSessionIdx  = -1;
    msg->_largePayload    = nullptr;
    msg->_largePayloadLen = 0;
    msg->_jitterApplied   = false;
    memcpy(msg->payload, data, len);
    return send(msg);
}

SendStatus Engine::startLargeSend(uint8_t dst, const uint8_t* data, size_t len,
                                   bool requireAck, uint8_t hopCount,
                                   uint32_t ackTimeoutMs) {
    if (len == 0 || len > RF_MAX_FRAGMENTED_PAYLOAD) {
        LOG_W("Engine", "startLargeSend: bad len %u", (unsigned)len);
        return SendStatus::BAD_LENGTH;
    }
    if (!_eventQueue) {
        LOG_E("Engine", "startLargeSend: not initialized");
        return SendStatus::NOT_INITIALIZED;
    }

    // Fail fast here, not mid-session: aborting mid-flight wastes airtime on
    // fragments the receiver can't reassemble in time.
    const uint32_t totalToa = _computeWorstCaseToa(len);
    const uint16_t denom    = _currentDutyDenom();
    if (!_duty.isAttainable(denom, totalToa)) {
        LOG_W("Engine",
              "startLargeSend: total ToA %u ms exceeds duty limit — un-sendable",
              (unsigned)totalToa);
        return SendStatus::MESSAGE_TOO_LARGE_FOR_DUTY;
    }
    const uint32_t waitMs = _duty.getWaitMs(denom, totalToa);
    if (waitMs > 0) {
        LOG_W("Engine",
              "startLargeSend: insufficient duty budget, retry in %u ms",
              (unsigned)waitMs);
        return SendStatus::DUTY_CYCLE_INSUFFICIENT;
    }

    int8_t sessionIdx = -1;
    for (int8_t i = 0; i < (int8_t)RF_LARGE_TX_SESSIONS; i++) {
        if (!_largeTx[i].inUse()) { sessionIdx = i; break; }
    }
    if (sessionIdx < 0) {
        LOG_W("Engine", "startLargeSend: no free session slot");
        return SendStatus::LARGE_TX_BUSY;
    }

    uint8_t total = (uint8_t)((len + RF_MAX_PAYLOAD - 1) / RF_MAX_PAYLOAD);
    uint8_t msgId = _nextOutgoingMsgId++;

    uint8_t hops = hopCount;
    if (hops > 7) hops = 7;

    bool doAck = requireAck && (dst != ADDR_BROADCAST);

    _largeTx[sessionIdx].start(dst, data, (uint16_t)len, msgId, total,
                                doAck, hops, ackTimeoutMs, _cfg.fragRetryMax);

    RFMessage* msg = _poolAlloc(/*forFragment=*/true);
    if (!msg) {
        LOG_W("Engine", "startLargeSend: pool exhausted");
        _largeTx[sessionIdx].release();
        return SendStatus::POOL_EXHAUSTED;
    }

    _fillFragmentMsg(msg, sessionIdx);

    EngineEvent evt{RFEvent::TX_REQ, msg};
    if (!_eventQueue->push(&evt, OS_NO_WAIT)) {
        LOG_W("Engine", "startLargeSend: event queue full");
        _poolFree(msg);
        _largeTx[sessionIdx].release();
        return SendStatus::QUEUE_FULL;
    }
    return SendStatus::OK;
}

void Engine::_fillFragmentMsg(RFMessage* msg, int8_t sessionIdx) {
    LargeTxSession& sess = _largeTx[sessionIdx];
    uint8_t idx = sess.nextIdx();
    bool    isLast = (idx == sess.total() - 1u);

    msg->peer             = sess.peer();
    msg->waitAck          = sess.requireAck();
    msg->ackTimeoutMs     = sess.ackTimeoutMs();  // 0 = auto-resolve in _sendMsg
    msg->hopCount         = sess.hopCount();
    msg->_isFragment      = true;
    msg->_fragMsgId       = sess.msgId();
    msg->_fragIdx         = idx;
    msg->_fragTotal       = sess.total();
    msg->_fragSessionIdx  = sessionIdx;
    msg->_largePayload    = nullptr;
    msg->_largePayloadLen = 0;
    // Fragments never jitter (gated by _isFragment in _routeTxReq, not this
    // flag); set true here only for clarity/consistency, not because it's read.
    msg->_jitterApplied   = true;

    const size_t offset    = (size_t)idx * RF_MAX_PAYLOAD;
    const size_t remaining = sess.userLen() - offset;
    const uint8_t fragLen  = isLast ? (uint8_t)remaining
                                     : (uint8_t)RF_MAX_PAYLOAD;
    msg->payloadLen = fragLen;
    memcpy(msg->payload, sess.userData() + offset, fragLen);
}

bool Engine::_sendNextFragment(int8_t sessionIdx, uint32_t delayMs) {
    RFMessage* msg = _poolAlloc(/*forFragment=*/true);
    if (!msg) {
        _largeTx[sessionIdx].setWaitingForPool(true);
        return false;
    }
    _largeTx[sessionIdx].setWaitingForPool(false);
    _fillFragmentMsg(msg, sessionIdx);

    if (delayMs > 0) {
        if (_scheduleDelayedTx(msg, delayMs)) return true;
        // _delayedTx full: fall back to immediate rather than drop the fragment.
    }

    EngineEvent evt{RFEvent::TX_REQ, msg};
    if (!_eventQueue->push(&evt, OS_NO_WAIT)) {
        LOG_W("Engine", "fragment TX queue full, will retry");
        _poolFree(msg);
        _largeTx[sessionIdx].setWaitingForPool(true);
        return false;
    }
    return true;
}

bool Engine::_scheduleDelayedTx(RFMessage* msg, uint32_t delayMs) {
    const uint32_t wake = _osal.tickMs() + delayMs;
    for (auto& e : _delayedTx) {
        if (e.used) continue;
        e.msg      = msg;
        e.wakeTick = wake;
        e.used     = true;
        return true;
    }
    return false;
}

void Engine::_processDelayedTx() {
    const uint32_t now = _osal.tickMs();
    for (auto& e : _delayedTx) {
        if (!e.used) continue;
        if ((int32_t)(now - e.wakeTick) < 0) continue;
        e.used = false;
        RFMessage* msg = e.msg;
        // Re-evaluates send-vs-defer in case session state changed while parked;
        // _jitterApplied already set, so no re-jitter.
        _routeTxReq(msg);
        return;  // one per tick, mirrors _processForwards
    }
}

void Engine::_completeLargeSend(int8_t sessionIdx, bool success,
                                 uint32_t lastSeq, TxFailReason reason) {
    LargeTxSession& sess = _largeTx[sessionIdx];
    // Capture all fields before release() clears the slot.
    const uint8_t* data       = sess.userData();
    size_t         len        = sess.userLen();
    uint8_t        dst        = sess.peer();
    bool           requireAck = sess.requireAck();
    uint32_t       ackTo      = sess.ackTimeoutMs();
    sess.release();

    // Reuses the single-frame callback path; _poolFree is a no-op for it.
    RFMessage temp = {};
    temp.peer             = dst;
    temp._seq             = lastSeq;
    temp.waitAck          = requireAck;
    temp.ackTimeoutMs     = ackTo;
    temp._largePayload    = data;
    temp._largePayloadLen = len;
    temp._fragSessionIdx  = -1;

    if (success) {
        if (_cbSuccess) _cbSuccess(&temp, _ctxSuccess);
    } else {
        if (_cbFailed)  _cbFailed(&temp, reason, _ctxFailed);
    }

    _flushDeferred();
    _processOutgoingAcks();
}

void Engine::_driveFragmentSessions() {
    for (int8_t i = 0; i < (int8_t)RF_LARGE_TX_SESSIONS; i++) {
        if (_largeTx[i].inUse() && _largeTx[i].waitingForPool()) {
            _sendNextFragment(i);
        }
    }
}

void Engine::_fireSend(EngineSendCb cb, void* ctx, RFMessage* msg) {
    if (cb) cb(msg, ctx);
    _poolFree(msg);
}

void Engine::_fireSendFail(EngineSendFailCb cb, void* ctx, RFMessage* msg, TxFailReason reason) {
    if (cb) cb(msg, reason, ctx);
    _poolFree(msg);
}

uint32_t Engine::_computeWorstCaseToa(size_t userLen) const {
    if (userLen == 0) return 0;
    const uint16_t tagLen = _cfg.sec ? CRYPTO_TAG_SIZE : 0;

    if (userLen <= RF_MAX_PAYLOAD) {
        const uint16_t frameLen = (uint16_t)PACKET_HEADER_SIZE
                                + (uint16_t)userLen + tagLen;
        return _radio.getAirtimeMs(frameLen);
    }

    // Fragmented: every non-last fragment carries RF_MAX_PAYLOAD bytes,
    // last carries the remainder. Each fragment uses the 8-byte header.
    const uint8_t total       = (uint8_t)((userLen + RF_MAX_PAYLOAD - 1) / RF_MAX_PAYLOAD);
    const size_t  lastPayload = userLen - (size_t)(total - 1) * RF_MAX_PAYLOAD;
    const uint16_t fullLen = (uint16_t)PACKET_HEADER_SIZE_FRAG
                           + (uint16_t)RF_MAX_PAYLOAD + tagLen;
    const uint16_t lastLen = (uint16_t)PACKET_HEADER_SIZE_FRAG
                           + (uint16_t)lastPayload + tagLen;

    return (uint32_t)(total - 1) * _radio.getAirtimeMs(fullLen)
         + _radio.getAirtimeMs(lastLen);
}

uint32_t Engine::getDutyCycleWaitMs(size_t totalLen) const {
    const uint32_t toa   = _computeWorstCaseToa(totalLen);
    const uint16_t denom = _currentDutyDenom();
    if (!_duty.isAttainable(denom, toa)) return UINT32_MAX;
    return _duty.getWaitMs(denom, toa);
}

bool Engine::startWorkerTask(uint32_t stackSize, uint8_t priority) {
    // Reset before spawning: a stale true from a prior cycle would let end() free
    // OS objects while the new worker still runs.
    _workerExited = false;
    bool ok = _osal.createTask(_workerEntry, this, "engine", stackSize, priority);
    if (ok) {
        _workerStarted = true;
    } else {
        // Fails only if caller asked for a worker: no heap/stack, or a poll-only
        // OSAL (e.g. baremetal).
        LOG_W("Engine", "startWorkerTask: createTask failed — out of heap/stack, or a "
              "poll-only OSAL; drive the node with poll() instead");
    }
    return ok;
}

void Engine::_workerEntry(void* pv) {
    static_cast<Engine*>(pv)->_workerLoop();
}

OsMs Engine::_nextWakeMs() const {
    OsMs earliest = OS_WAIT_FOREVER;
    uint32_t now = _osal.tickMs();

    for (const auto& p : _pending) {
        if (!p.used) continue;
        uint32_t elapsed = now - p.msg->_sentTick;
        if (elapsed >= p.msg->ackTimeoutMs) return 0;
        OsMs remaining = p.msg->ackTimeoutMs - elapsed;
        if (remaining < earliest) earliest = remaining;
    }

    for (const auto& e : _fwdQueue) {
        if (!e.used) continue;
        int32_t diff = (int32_t)(e.wakeTick - now);
        if (diff <= 0) return 0;
        if ((OsMs)diff < earliest) earliest = (OsMs)diff;
    }

    for (const auto& a : _ackQueue) {
        if (!a.used) continue;
        int32_t diff = (int32_t)(a.wakeTick - now);
        if (diff <= 0) return 0;
        if ((OsMs)diff < earliest) earliest = (OsMs)diff;
    }

    for (const auto& d : _delayedTx) {
        if (!d.used) continue;
        int32_t diff = (int32_t)(d.wakeTick - now);
        if (diff <= 0) return 0;
        if ((OsMs)diff < earliest) earliest = (OsMs)diff;
    }

    // A pool-stalled session has no pending/delayed entry of its own; poll it
    // periodically so it can't wedge the worker forever.
    for (const auto& s : _largeTx) {
        if (s.inUse() && s.waitingForPool()) {
            if (earliest > RF_POOL_STALL_POLL_MS) earliest = RF_POOL_STALL_POLL_MS;
            break;
        }
    }

    return earliest;
}

bool Engine::_sessionInFlight() const {
    for (const auto& s : _largeTx) {
        if (s.inUse()) return true;
    }
    return false;
}

void Engine::_routeTxReq(RFMessage* msg) {
    // Fragments never defer — they ARE the session.
    if (msg->_isFragment || !_sessionInFlight()) {
        if (!msg->_isFragment && !msg->_jitterApplied && _cfg.originatedJitter) {
            const uint16_t tagLen = _cfg.sec ? CRYPTO_TAG_SIZE : 0;
            const uint16_t frameLen = (uint16_t)PACKET_HEADER_SIZE
                                    + (uint16_t)msg->payloadLen + tagLen;
            const uint32_t delay = _jitterDelayMs(frameLen);
            if (delay > 0) {
                msg->_jitterApplied = true;
                if (_scheduleDelayedTx(msg, delay)) return;
                // _delayedTx full — fall through to immediate send.
            }
        }
        _sendMsg(msg);
        return;
    }
    for (auto& d : _deferred) {
        if (!d.used) { d.msg = msg; d.used = true; return; }
    }
    // Implies the pool is also exhausted (every slot is here or in _pending).
    LOG_E("Engine", "_deferred full, dropping dst=0x%02X len=%u",
          msg->peer, (unsigned)msg->payloadLen);
    _fireSendFail(_cbFailed, _ctxFailed, msg, TxFailReason::PENDING_LIST_FULL);
}

void Engine::_flushDeferred() {
    // Snapshot first: re-parking mid-walk would corrupt an in-place iteration.
    RFMessage* batch[RF_POOL_SIZE];
    uint8_t    n = 0;
    for (auto& d : _deferred) {
        if (!d.used) continue;
        d.used = false;
        batch[n++] = d.msg;
    }
    for (uint8_t i = 0; i < n; i++) {
        _routeTxReq(batch[i]);
    }
}

void Engine::_scheduleAckRetry(const uint8_t* frame, uint16_t len, uint8_t radioAddr) {
    if (len > sizeof(_ackQueue[0].frame)) {
        LOG_W("Engine", "ACK retry: frame too large (%u), dropping", (unsigned)len);
        return;
    }
    const uint32_t backoff = _busyBackoffMs(len);
    const uint32_t now = _osal.tickMs();
    for (auto& a : _ackQueue) {
        if (a.used) continue;
        memcpy(a.frame, frame, len);
        a.len       = len;
        a.radioAddr = radioAddr;
        a.wakeTick  = now + backoff;
        a.retries   = 0;
        a.used      = true;
        return;
    }
    LOG_W("Engine", "_ackQueue full, dropping ACK to L2=0x%02X", radioAddr);
}

void Engine::_processOutgoingAcks() {
    // Avoid colliding with the ACK we're listening for.
    if (_sessionInFlight()) return;

    uint32_t now = _osal.tickMs();
    for (auto& a : _ackQueue) {
        if (!a.used) continue;
        if ((int32_t)(now - a.wakeTick) < 0) continue;
        uint32_t waitMs = 0;
        TxResult tr    = TxResult::OK;
        if (_txAndAccount(a.frame, a.len, a.radioAddr, &waitMs, &tr)) {
            a.used = false;
            continue;
        }
        if (waitMs > 0) {
            // Duty off-time not elapsed: reschedule after the exact wait; not
            // counted as a retry.
            a.wakeTick = now + waitMs;
            continue;
        }
        if (a.retries < _cfg.ackRetryMax) {
            a.wakeTick = now + _busyBackoffMs(a.len);
            a.retries++;
            continue;
        }
        LOG_W("Engine", "ACK retry exhausted L2=0x%02X (%u attempts)",
              a.radioAddr, (unsigned)a.retries);
        a.used = false;
    }
}

void Engine::poll() {
    if (_workerStarted) return;
    if (!_eventQueue) return;

    // Drain before TX work, same as _workerLoop (see there for why).
    _drainRadioEvents();

    EngineEvent evt;
    while (_eventQueue->pop(&evt, OS_NO_WAIT)) {
        if (evt.type == RFEvent::TX_REQ) {
            _routeTxReq(evt.txMsg);
        } else if (evt.type == RFEvent::HW_IRQ) {
            _drainRadioEvents();
        }
    }
    _processForwards();
    _processOutgoingAcks();
    _processDelayedTx();
    _checkTimeouts();
    _reasm.tick(_osal.tickMs(), _reasmTimeoutMs());
    _driveFragmentSessions();
}

void Engine::_workerLoop() {
    // Initial RX arm; HAL's send() auto-rearms RX on every later TX attempt.
    _radio.startReceive();

    while (_workerStarted) {
        EngineEvent evt;
        const bool got = _eventQueue->pop(&evt, _nextWakeMs());

        // Must precede TX_REQ handling: otherwise cca()'s startReceive() could clear
        // pending RX before the frame is read, or a full queue could swallow an IRQ.
        _drainRadioEvents();

        if (got) {
            if (evt.type == RFEvent::TX_REQ) {
                _routeTxReq(evt.txMsg);
            } else if (evt.type == RFEvent::SHUTDOWN) {
                break;
            }
            // HW_IRQ: already consumed by the unconditional drain above.
        }

        _processForwards();
        _processOutgoingAcks();
        _processDelayedTx();
        _checkTimeouts();
        _reasm.tick(_osal.tickMs(), _reasmTimeoutMs());
        _driveFragmentSessions();
    }
    _workerExited = true;
}

uint32_t Engine::_resolveAckTimeoutMs(uint16_t dataFrameLen, uint8_t hopCount) const {
    if (_cfg.ackTimeoutMs) return _cfg.ackTimeoutMs;

    const uint16_t ackLen = PACKET_HEADER_SIZE
                          + (_cfg.sec ? ACK_ENC_PLAINTEXT_SIZE + CRYPTO_TAG_SIZE : 0);
    const uint32_t toaAck = _radio.getAirtimeMs(ackLen);

    if (_cfg.mode != PacketMode::Mesh) {
        const uint32_t margin = _cfg.ackTimeoutMarginP2Pms
                              ? _cfg.ackTimeoutMarginP2Pms
                              : RF_ACK_TIMEOUT_MARGIN_P2P_MS;
        return 2 * toaAck + margin;
    }

    const uint8_t h = hopCount ? hopCount : 1;
    const uint32_t toaData = _radio.getAirtimeMs(dataFrameLen);

    // Per-hop budget: 1x ToA transmission + K x ToA jitter window + worst-case
    // CHANNEL_BUSY backoff over RF_FWD_RETRY_MAX retries. Without the retry term,
    // a busy intermediate hop would spuriously trip the ACK timeout.
    const uint32_t slotsPerHop = 1u + RF_JITTER_WINDOW_SLOTS
                               + 2u * (uint32_t)RF_FWD_RETRY_MAX;
    const uint32_t perHopData  = toaData * slotsPerHop;
    const uint32_t perHopAck   = toaAck  * slotsPerHop;

    const uint32_t out  = (h > 1) ? (uint32_t)(h - 1) * perHopData : 0;
    const uint32_t back = toaAck + ((h > 1) ? (uint32_t)(h - 1) * perHopAck : 0);
    const uint32_t meshMargin = _cfg.ackTimeoutMarginMeshMs
                              ? _cfg.ackTimeoutMarginMeshMs
                              : RF_ACK_TIMEOUT_MARGIN_MESH_MS;
    return out + back + meshMargin;
}

uint32_t Engine::_reasmTimeoutMs() const {
    const uint16_t tagLen   = _cfg.sec ? CRYPTO_TAG_SIZE : 0;
    const uint16_t frameLen = (uint16_t)(PACKET_HEADER_SIZE_FRAG
                            + RF_MAX_PAYLOAD + tagLen);
    const uint32_t perRound = _resolveAckTimeoutMs(frameLen, _cfg.hopCount);
    // ×2: margin for the sender-side CHANNEL_BUSY backoff between rounds.
    const uint32_t worst = (uint32_t)(_cfg.fragRetryMax + 1u) * perRound * 2u;
    return (worst > RF_REASM_TIMEOUT_MS) ? worst : RF_REASM_TIMEOUT_MS;
}

uint32_t Engine::_jitterDelayMs(uint16_t frameLen) {
    const uint32_t toa = _radio.getAirtimeMs(frameLen);
    const uint32_t window = toa * RF_JITTER_WINDOW_SLOTS;
    if (window == 0) return 0;
    return _osal.random() % (window + 1);
}

uint32_t Engine::_busyBackoffMs(uint16_t frameLen) {
    uint32_t toa = _radio.getAirtimeMs(frameLen);
    if (toa == 0) toa = RF_ACK_RETRY_FALLBACK_TOA_MS;
    return toa + (_osal.random() % (toa + 1));
}

uint8_t Engine::_resolveL2(uint8_t appDst) const {
    if (_cfg.mode != PacketMode::Mesh) return appDst;
    if (appDst == ADDR_BROADCAST)      return ADDR_BROADCAST;
    if (!_cfg.routing)                 return ADDR_BROADCAST;
    RoutingContext ctx{ _cfg.myAddr, _lastRssi };
    return _cfg.routing->nextHop(appDst, ctx);
}

bool Engine::_txAndAccount(const uint8_t* buf, uint16_t len, uint8_t dst,
                           uint32_t* outWaitMs,
                           TxResult* outRadioResult) {
    if (outWaitMs)      *outWaitMs      = 0;
    if (outRadioResult) *outRadioResult = TxResult::OK;
    uint32_t toaMs = _radio.getAirtimeMs(len);
    const uint16_t denom = _currentDutyDenom();
    // A radio reporting 0 airtime would pass the duty gate for free (canTransmit/
    // addTransmission are no-ops at 0). Charge a floor instead and warn once,
    // rather than silently disabling enforcement.
    if (denom != 0 && toaMs == 0) {
        if (!_dutyAirtimeWarned) {
            LOG_W("Engine", "duty enabled but radio reports 0 airtime — charging a %u ms "
                  "floor per frame; duty accounting is approximate (the radio backend should "
                  "implement getAirtimeMs)", (unsigned)RF_ACK_RETRY_FALLBACK_TOA_MS);
            _dutyAirtimeWarned = true;
        }
        toaMs = RF_ACK_RETRY_FALLBACK_TOA_MS;
    }
    if (!_duty.canTransmit(denom, toaMs)) {
        if (outWaitMs) *outWaitMs = _duty.getWaitMs(denom, toaMs);
        return false;
    }
    // LBT lives here, not in HAL.send, so Engine controls CCA policy.
    if (_radio.cca(RF_CCA_TIMEOUT_MS, (int8_t)RF_CCA_RSSI_THRESHOLD)
        == CcaResult::BUSY) {
        if (outRadioResult) *outRadioResult = TxResult::CHANNEL_BUSY;
        return false;
    }
    TxResult r = _radio.send(buf, len, dst);
    if (r != TxResult::OK) {
        if (outRadioResult) *outRadioResult = r;
        return false;
    }
    _duty.addTransmission(denom, toaMs);
    return true;
}

void Engine::_sendMsg(RFMessage* msg) {
    const uint32_t rawSeq = _nvSeq.get();
    // Wire seq is 24-bit, feeds the GCM nonce; a wrap reuses a nonce under the same
    // key (GHASH leak, tag forgery). Warn once per block.
    const uint8_t seqEpoch = (uint8_t)(rawSeq >> 24);
    if (seqEpoch != _seqWrapEpoch) {
        _seqWrapEpoch = seqEpoch;
        LOG_E("Engine",
              "24-bit seq wrapped (epoch %u) — GCM nonce reuse risk, rotate the network key",
              (unsigned)seqEpoch);
    }
    uint32_t seq = rawSeq & 0xFFFFFF;
    _nvSeq.increment();

    // Stamp up front: every failure path below then reports the real seq, not a
    // stale value from the recycled pool slot.
    msg->_seq = seq;

    uint8_t hopCount = 0;
    if (_cfg.mode == PacketMode::Mesh) {
        hopCount = msg->hopCount;
    }

    uint8_t  frameBuf[RF_FRAME_BUF_SIZE];

    FragInfo fi;
    const FragInfo* fragPtr = nullptr;
    if (msg->_isFragment) {
        fi      = { msg->_fragMsgId, msg->_fragIdx, msg->_fragTotal };
        fragPtr = &fi;
    }

    const bool waitAck = msg->waitAck && (msg->peer != ADDR_BROADCAST);

    uint16_t frameLen = PacketBuilder::build(
        frameBuf, sizeof(frameBuf),
        _cfg.mode, _cfg.myAddr, msg->peer, hopCount,
        seq, msg->payload, msg->payloadLen, _cfg.sec, fragPtr, waitAck);

    msg->_dutyWaitMs = 0;

    if (frameLen == 0) {
        LOG_E("Engine", "TX failed: frame build failed dst=0x%02X", msg->peer);
        if (msg->_isFragment) {
            int8_t si = msg->_fragSessionIdx;
            _poolFree(msg);
            _completeLargeSend(si, false, 0, TxFailReason::FRAME_BUILD_FAILED);
        } else {
            _fireSendFail(_cbFailed, _ctxFailed, msg, TxFailReason::FRAME_BUILD_FAILED);
        }
        return;
    }

    if (msg->waitAck && msg->peer != ADDR_BROADCAST && msg->ackTimeoutMs == 0) {
        msg->ackTimeoutMs = _resolveAckTimeoutMs(frameLen, hopCount);
    }

    uint32_t waitMs = 0;
    TxResult radioResult = TxResult::OK;
    uint8_t radioAddr = _resolveL2(msg->peer);
    if (!_txAndAccount(frameBuf, frameLen, radioAddr, &waitMs, &radioResult)) {
        TxFailReason reason;
        if (waitMs > 0) {
            LOG_W("Engine", "TX deferred: duty wait %u ms", (unsigned)waitMs);
            msg->_dutyWaitMs = waitMs;
            reason = TxFailReason::DUTY_CYCLE;
        } else if (radioResult == TxResult::CHANNEL_BUSY) {
            LOG_W("Engine", "TX failed: channel busy dst=0x%02X", msg->peer);
            reason = TxFailReason::CHANNEL_BUSY;
        } else if (radioResult == TxResult::BAD_FRAME) {
            LOG_E("Engine", "TX failed: bad frame dst=0x%02X len=%u",
                  msg->peer, (unsigned)frameLen);
            reason = TxFailReason::FRAME_BUILD_FAILED;
        } else {
            LOG_E("Engine", "TX failed: radio error dst=0x%02X", msg->peer);
            reason = TxFailReason::RADIO_ERROR;
        }

        if (msg->_isFragment) {
            int8_t   si      = msg->_fragSessionIdx;
            uint32_t lastSeq = msg->_seq;
            _poolFree(msg);
            // Not a failure: reschedule the same fragment instead of tearing down
            // the session, which would strand the receiver's slot.
            if (reason == TxFailReason::DUTY_CYCLE) {
                if (!_sendNextFragment(si, waitMs)) {
                    // Pool full: session stays waitingForPool;
                    // _driveFragmentSessions retries once a slot frees.
                    LOG_W("Engine", "frag duty-deferred but pool full (session %d) — will retry",
                          (int)si);
                }
                return;
            }
            bool done, success;
            done = _largeTx[si].onFragmentResult(false, &success);
            if (done) {
                _completeLargeSend(si, success, lastSeq, reason);
            } else {
                // CHANNEL_BUSY backs off; RADIO_ERROR (transient) retries immediately.
                const uint32_t backoff = (reason == TxFailReason::CHANNEL_BUSY)
                                       ? _busyBackoffMs(frameLen) : 0;
                LOG_W("Engine", "frag retry %u/%u after %s (session %d, backoff=%u ms)",
                      (unsigned)_largeTx[si].fragRetries(),
                      (unsigned)_largeTx[si].fragRetryMax(),
                      reason == TxFailReason::CHANNEL_BUSY  ? "CHANNEL_BUSY" :
                      reason == TxFailReason::RADIO_ERROR   ? "RADIO_ERROR"  :
                                                              "OTHER",
                      (int)si, (unsigned)backoff);
                _sendNextFragment(si, backoff);
            }
        } else {
            _fireSendFail(_cbFailed, _ctxFailed, msg, reason);
        }
        return;
    }

    LOG_D("Engine", "TX OK dst=0x%02X seq=%lu len=%u frag=%u/%u",
          (unsigned)msg->peer, (unsigned long)msg->_seq,
          (unsigned)frameLen,
          msg->_isFragment ? (unsigned)msg->_fragIdx + 1u : 1u,
          msg->_isFragment ? (unsigned)msg->_fragTotal    : 1u);

    if (!waitAck) {
        if (msg->_isFragment) {
            int8_t   si      = msg->_fragSessionIdx;
            uint32_t fragSeq = msg->_seq;
            bool done, success;
            done = _largeTx[si].onFragmentResult(true, &success);
            _poolFree(msg);
            if (done) {
                _completeLargeSend(si, success, fragSeq);
            } else {
                _sendNextFragment(si);
            }
            return;
        }
        _fireSend(_cbSuccess, _ctxSuccess, msg);
        return;
    }

    msg->_sentTick = _osal.tickMs();

    for (auto& p : _pending) {
        if (!p.used) { p.msg = msg; p.used = true; return; }
    }
    LOG_E("Engine", "pending list full");
    if (msg->_isFragment) {
        int8_t si = msg->_fragSessionIdx;
        _poolFree(msg);
        _completeLargeSend(si, false, 0, TxFailReason::PENDING_LIST_FULL);
    } else {
        _fireSendFail(_cbFailed, _ctxFailed, msg, TxFailReason::PENDING_LIST_FULL);
    }
}

void Engine::_handleRaw(uint8_t* raw, uint16_t len) {
    RoutingContext rctx{ _cfg.myAddr, _lastRssi };

    uint8_t forwardBuf[RF_FRAME_BUF_SIZE];
    bool    needFwdCopy = (_cfg.routing != nullptr &&
                           _cfg.sec    != nullptr &&
                           _cfg.mode   == PacketMode::Mesh);
    if (needFwdCopy && len <= sizeof(forwardBuf)) {
        memcpy(forwardBuf, raw, len);
    } else {
        needFwdCopy = false;
    }

    ParsedPacket parsed = PacketParser::parse(raw, len, _cfg.sec);
    if (!parsed.valid) {
        LOG_W("Engine", "recv: dropped (invalid/auth fail) len=%u", (unsigned)len);
        return;
    }

    if (_cfg.sec != nullptr && _cfg.requireEncrypted && !parsed.hdr.encrypted()) {
        LOG_W("Engine", "recv: dropped (plaintext rejected in strict mode)");
        return;
    }

    uint8_t  src   = parsed.hdr.src();
    uint32_t seq   = headerGetSeq(parsed.hdr);
    bool     isAck = parsed.hdr.isAck();

    if (src == _cfg.myAddr) return;

    if (isAck) {
        if (_seen.checkAndMark(src, parsed.hdr.dst(), seq, 1)) return;
    } else {
        // Covers real replay and routine mesh duplicates; LOG_D not W since
        // duplicates are normal in a flooding mesh.
        if (_replay.isReplay(src, seq)) {
            LOG_D("Engine", "recv: replay/dup drop src=0x%02X seq=%lu",
                  (unsigned)src, (unsigned long)seq);
            return;
        }
    }

    if (isAck) {
        if (PacketParser::isForMe(parsed.hdr, _cfg.myAddr)) {
            for (auto& p : _pending) {
                if (p.used && p.msg->peer == src && p.msg->_seq == seq) {
                    p.used = false;
                    RFMessage* msg = p.msg;
                    if (msg->_fragSessionIdx >= 0) {
                        int8_t   si      = msg->_fragSessionIdx;
                        uint32_t fragSeq = msg->_seq;
                        _poolFree(msg);
                        bool done, success;
                        done = _largeTx[si].onFragmentResult(true, &success);
                        if (done) {
                            _completeLargeSend(si, success, fragSeq);
                        } else {
                            _sendNextFragment(si);
                        }
                    } else {
                        _fireSend(_cbSuccess, _ctxSuccess, msg);
                    }
                    break;
                }
            }
        }
        if (_cfg.routing != nullptr &&
            PacketParser::shouldForward(parsed.hdr, rctx, *_cfg.routing)) {
            // Forward the pre-parse copy: parse() decrypted `raw` in place,
            // which would fail the next hop's tag check.
            _scheduleForward(needFwdCopy ? forwardBuf : raw, len);
        }
        return;
    }

    if (_cfg.routing != nullptr &&
        PacketParser::shouldForward(parsed.hdr, rctx, *_cfg.routing)) {
        _scheduleForward(needFwdCopy ? forwardBuf : raw, len);
    }

    if (!PacketParser::isForMe(parsed.hdr, _cfg.myAddr)) return;

    // No key but frame encrypted: parse() skipped decrypt/auth, so `payload` is
    // ciphertext. Delivering/ACKing it would report false success.
    if (_cfg.sec == nullptr && parsed.hdr.encrypted()) {
        LOG_W("Engine", "recv: encrypted frame from 0x%02X but no key configured, dropped", src);
        return;
    }

    // Ingest before ACK, and only ACK if accepted: an Aborted/Dropped fragment
    // must not be ACKed, or the sender sees false success for an undeliverable message.
    if (parsed.fragmented) {
        // Safe to narrow: a fragment carries at most RF_MAX_PAYLOAD bytes.
        uint8_t fragPayloadLen = static_cast<uint8_t>(parsed.payloadLen);
        Reassembler::Result r = _reasm.ingest(
            src, parsed.fragMsgId, parsed.fragIdx, parsed.fragTotal,
            parsed.payload, fragPayloadLen, _osal.tickMs());

        const bool accepted = (r.status == Reassembler::Status::Complete ||
                               r.status == Reassembler::Status::Incomplete);
        // ACK only when requested (ACK_REQ); a no-ack session advances on
        // TX success alone.
        if (accepted && parsed.hdr.ackReq() && parsed.hdr.dst() != ADDR_BROADCAST) {
            uint8_t  ackBuf[PACKET_HEADER_SIZE + ACK_ENC_PLAINTEXT_SIZE + CRYPTO_TAG_SIZE];
            uint8_t  ackHops = (_cfg.mode == PacketMode::Mesh) ? _cfg.hopCount : 0;
            uint16_t ackLen  = PacketBuilder::buildAck(
                ackBuf, sizeof(ackBuf), _cfg.mode, _cfg.myAddr, src, seq, ackHops, _cfg.sec);
            uint8_t ackAddr = _resolveL2(src);
            if (ackLen == 0) {
                LOG_W("Engine", "frag ACK build failed for 0x%02X", src);
            } else {
                uint32_t waitMs = 0;
                TxResult tr    = TxResult::OK;
                if (!_txAndAccount(ackBuf, ackLen, ackAddr, &waitMs, &tr)) {
                    // Park for retry: losing this ACK fails the sender's
                    // whole session via ACK timeout.
                    LOG_W("Engine", "frag ACK deferred for 0x%02X (busy/err), retrying", src);
                    _scheduleAckRetry(ackBuf, ackLen, ackAddr);
                }
            }
        } else if (r.status == Reassembler::Status::Aborted) {
            LOG_W("Engine", "frag from 0x%02X msgId=%u belongs to aborted message, not ACKed",
                  src, (unsigned)parsed.fragMsgId);
        }

        if (r.status == Reassembler::Status::Complete && _cbRecv) {
            LOG_D("Engine", "RX large src=0x%02X msgId=%u totalLen=%u rssi=%d dBm",
                  (unsigned)src, (unsigned)parsed.fragMsgId,
                  (unsigned)r.totalLen, (int)_lastRssi);
            RxInfo rxInfo{ src, (parsed.hdr.dst() == ADDR_BROADCAST), _lastRssi };
            RFMessage rxMsg = {};
            rxMsg.peer             = src;
            rxMsg._largePayload    = r.data;
            rxMsg._largePayloadLen = r.totalLen;
            rxMsg._fragSessionIdx  = -1;
            _cbRecv(rxInfo, &rxMsg, _ctxRecv);
        }
        return;
    }

    if (parsed.payloadLen > RF_MAX_PAYLOAD) {
        LOG_W("Engine", "recv: payload too large (%u), dropped", (unsigned)parsed.payloadLen);
        return;
    }

    // ACK before the user callback: a blocking callback here, against the sender's
    // tight auto-resolved timeout, would turn delivered messages into false ACK_TIMEOUTs.
    if (parsed.hdr.ackReq() && parsed.hdr.dst() != ADDR_BROADCAST) {
        uint8_t  ackBuf[PACKET_HEADER_SIZE + ACK_ENC_PLAINTEXT_SIZE + CRYPTO_TAG_SIZE];
        uint8_t  ackHops = (_cfg.mode == PacketMode::Mesh) ? _cfg.hopCount : 0;
        uint16_t ackLen  = PacketBuilder::buildAck(
            ackBuf, sizeof(ackBuf), _cfg.mode, _cfg.myAddr, src, seq, ackHops, _cfg.sec);
        uint8_t ackRadioAddr = _resolveL2(src);
        if (ackLen == 0) {
            LOG_W("Engine", "ACK build failed for 0x%02X", src);
        } else {
            uint32_t waitMs = 0;
            TxResult tr    = TxResult::OK;
            if (!_txAndAccount(ackBuf, ackLen, ackRadioAddr, &waitMs, &tr)) {
                LOG_W("Engine", "ACK deferred to 0x%02X (busy/err), retrying", src);
                _scheduleAckRetry(ackBuf, ackLen, ackRadioAddr);
            }
        }
        // HAL auto-rearms RX inside send().
    }

    RFMessage rxMsg;
    rxMsg.peer             = src;
    rxMsg.payloadLen       = static_cast<uint8_t>(parsed.payloadLen);
    rxMsg._seq             = seq;
    rxMsg.waitAck          = false;
    rxMsg.ackTimeoutMs     = 0;
    rxMsg.hopCount         = 0;
    rxMsg._isFragment      = false;
    rxMsg._fragSessionIdx  = -1;
    rxMsg._largePayload    = nullptr;
    rxMsg._largePayloadLen = 0;
    memcpy(rxMsg.payload, parsed.payload, parsed.payloadLen);

    if (_cbRecv) {
        RxInfo rxInfo;
        rxInfo.from      = src;
        rxInfo.broadcast = (parsed.hdr.dst() == ADDR_BROADCAST);
        rxInfo.rssi      = _lastRssi;
        LOG_D("Engine", "RX src=0x%02X seq=%lu len=%u rssi=%d dBm",
              (unsigned)src, (unsigned long)seq,
              (unsigned)parsed.payloadLen, (int)_lastRssi);
        _cbRecv(rxInfo, &rxMsg, _ctxRecv);
    }
}

void Engine::_scheduleForward(const uint8_t* frame, uint16_t len) {
    if (len < PACKET_HEADER_SIZE || len > RF_FRAME_BUF_SIZE) return;

    uint32_t delay = _cfg.forwardJitter ? _jitterDelayMs(len) : 0;
    uint32_t wake = _osal.tickMs() + delay;

    for (auto& e : _fwdQueue) {
        if (e.used) continue;
        memcpy(e.frame, frame, len);
        e.len       = len;
        e.wakeTick  = wake;
        e.radioAddr = _resolveL2(frame[1]);
        e.retries   = 0;
        e.used      = true;
        PacketBuilder::decrementHop(e.frame);
        return;
    }
    LOG_W("Engine",
          "fwd queue full, dropping src=0x%02X dst=0x%02X seq=%lu hop=%u",
          frame[2], frame[1],
          (unsigned long)headerGetSeq(frame),
          (unsigned)(frame[0] & 0x07));
}

void Engine::_drainRadioEvents() {
    for (;;) {
        RadioEvent re = _radio.pollEvent();
        if (re.type == RadioEvent::NONE) break;
        if (re.type == RadioEvent::RX_DONE) {
            RadioPacket pkt;
            if (_radio.readPacket(&pkt)) {
                _lastRssi = pkt.rssi;
                _handleRaw(pkt.data, pkt.length);
            }
        } else if (re.type == RadioEvent::CRC_ERROR) {
            LOG_W("Engine", "radio CRC error");
        }
    }
}

void Engine::_processForwards() {
    uint32_t now = _osal.tickMs();
    for (auto& e : _fwdQueue) {
        if (!e.used) continue;
        if ((int32_t)(now - e.wakeTick) < 0) continue;
        uint32_t waitMs = 0;
        TxResult radioResult = TxResult::OK;
        if (!_txAndAccount(e.frame, e.len, e.radioAddr, &waitMs, &radioResult)) {
            // Duty-cycle: off-time still active — reschedule after the exact wait.
            if (waitMs > 0) {
                e.wakeTick = now + waitMs;
                continue;
            }
            // Retried here, not in L2, so the worker stays free for other work.
            if (radioResult == TxResult::CHANNEL_BUSY &&
                e.retries < RF_FWD_RETRY_MAX) {
                e.wakeTick = now + _busyBackoffMs(e.len);
                e.retries++;
                continue;
            }
            LOG_W("Engine",
                  "mesh forward dropped after %u retr%s: %s",
                  (unsigned)e.retries,
                  e.retries == 1 ? "y" : "ies",
                  radioResult == TxResult::CHANNEL_BUSY ? "channel-busy" : "radio-error");
        }
        e.used = false;
        return;
    }
}

void Engine::_checkTimeouts() {
    uint32_t now = _osal.tickMs();
    for (auto& p : _pending) {
        if (!p.used) continue;
        if ((now - p.msg->_sentTick) >= p.msg->ackTimeoutMs) {
            p.used = false;
            RFMessage* msg = p.msg;
            LOG_W("Engine", "ACK timeout dst=0x%02X seq=%lu",
                  msg->peer, (unsigned long)msg->_seq);
            if (msg->_fragSessionIdx >= 0) {
                int8_t   si      = msg->_fragSessionIdx;
                uint32_t lastSeq = msg->_seq;
                _poolFree(msg);
                // Most frag ACK timeouts are the receiver's ACK lost to interference;
                // re-TX safe since reassembler dedupes on (msgId, idx).
                bool done, success;
                done = _largeTx[si].onFragmentResult(false, &success);
                if (done) {
                    _completeLargeSend(si, success, lastSeq, TxFailReason::ACK_TIMEOUT);
                } else {
                    LOG_W("Engine", "frag retry %u/%u after ACK_TIMEOUT (session %d)",
                          (unsigned)_largeTx[si].fragRetries(),
                          (unsigned)_largeTx[si].fragRetryMax(), (int)si);
                    _sendNextFragment(si);
                }
            } else {
                _fireSendFail(_cbFailed, _ctxFailed, msg, TxFailReason::ACK_TIMEOUT);
            }
        }
    }
}
