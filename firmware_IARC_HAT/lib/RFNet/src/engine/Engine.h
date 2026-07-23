#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../port/hal/radio/IRadio.h"
#include "../core/RFConfig.h"
#include "../core/packet/Packet.h"
#include "../core/freshness/SeenCache.h"
#include "../core/freshness/ReplayWindow.h"
#include "../core/routing/IRoutingStrategy.h"
#include "../core/security/SecurityLayer.h"
#include "../core/duty/DutyCycleTracker.h"
#include "../core/nvs/MonotonicCounter.h"
#include "../core/fragment/Reassembler.h"
#include "../core/fragment/LargeTxSession.h"
#include "../port/osal/IOsal.h"
#include "../port/hal/clock/IClock.h"
#include "../port/hal/clock/DefaultClock.h"
#include "../port/hal/nvs/INVBackend.h"
#include "../port/hal/nvs/DefaultNVBackend.h"
#include "../RFTypes.h"

struct RFMessage {
    // On TX: recipient address. On RX: sender address.
    uint8_t  peer;
    uint8_t  payload[RF_MAX_PAYLOAD];
    // Normal sends: [1, RF_MAX_PAYLOAD]. Fragments: the per-fragment length.
    uint8_t  payloadLen;

    bool     waitAck;
    uint32_t ackTimeoutMs;
    uint8_t  hopCount;

    uint32_t _seq;
    uint32_t _sentTick;
    uint32_t _dutyWaitMs;

    // Non-null for large-send/receive callbacks: RFNode uses this instead of payload[].
    const uint8_t* _largePayload;
    size_t         _largePayloadLen;

    // Set for messages originating from a LargeTxSession.
    bool    _isFragment;
    uint8_t _fragMsgId;
    uint8_t _fragIdx;
    uint8_t _fragTotal;
    int8_t  _fragSessionIdx;  // index into Engine::_largeTx, -1 if not from session

    // Prevents re-jitter on a later pass through _routeTxReq.
    bool    _jitterApplied;
};

struct EngineConfig {
    // Node address in [0x01, 0xFE]; 0xFF is broadcast, 0x00 is reserved
    // (rejected by begin() — see there for why).
    uint8_t           myAddr          = 0x00;
    PacketMode        mode            = PacketMode::P2P;
    uint8_t           hopCount        = 0;
    uint32_t          ackTimeoutMs    = 0;
    // 0 = use the compile-time RF_ACK_TIMEOUT_MARGIN_* constants.
    uint16_t          ackTimeoutMarginP2Pms  = 0;
    uint16_t          ackTimeoutMarginMeshMs = 0;
    bool              forwardJitter   = true;
    // Pre-TX backoff on a message's first attempt, to decorrelate senders
    // on the same tick. Fragment continuations never jitter, regardless of
    // forwardJitter. Mesh forwards use forwardJitter instead.
    bool              originatedJitter = true;
    SecurityLayer*    sec              = nullptr;
    bool              requireEncrypted = true;
    IRoutingStrategy* routing          = nullptr;
    RFNvConfig        nv;
    IClock*           clock            = nullptr;

    // Duty-cycle gate; override must respect the regulatory floor from
    // IRadio::getRegulatoryDutyDenominator(). On by default; no-op if the
    // resolved denominator is 0.
    bool              dutyCycleEnabled              = true;
    uint16_t          dutyCycleDenominatorOverride  = 0;

    uint8_t           fragRetryMax = RF_FRAG_RETRY_MAX;
    uint8_t           ackRetryMax  = RF_ACK_RETRY_MAX;
};

using EngineSendCb     = void (*)(const RFMessage* msg, void* ctx);
using EngineSendFailCb = void (*)(const RFMessage* msg, TxFailReason reason, void* ctx);
using EngineRecvCb     = void (*)(const RxInfo& info, const RFMessage* msg, void* ctx);

enum class RFEvent : uint8_t {
    TX_REQ,
    HW_IRQ,
    SHUTDOWN
};

struct EngineEvent {
    RFEvent    type;
    RFMessage* txMsg;
};

class Engine {
public:
    Engine(IRadio& radio, IOsal& osal, const EngineConfig& cfg);
    ~Engine();

    BeginStatus begin();
    // Tears down the engine. If a worker task hasn't exited within
    // RF_WORKER_SHUTDOWN_TIMEOUT_MS, returns false without freeing anything —
    // caller must keep this Engine alive and call end() again later. Idempotent.
    bool end();

    void onSendSuccess(EngineSendCb     cb, void* ctx = nullptr);
    void onSendFailed (EngineSendFailCb cb, void* ctx = nullptr);
    void onReceive    (EngineRecvCb     cb, void* ctx = nullptr);

    RFMessage*  acquire();
    SendStatus  send(RFMessage* msg);
    SendStatus  send(uint8_t dst, const uint8_t* data, size_t len);

    // Fragmented send for len > RF_MAX_PAYLOAD; data must stay valid until the
    // session completes. Not safe to call concurrently on the same Engine:
    // session-slot selection and msgId assignment are unmutexed.
    SendStatus startLargeSend(uint8_t dst, const uint8_t* data, size_t len,
                              bool requireAck, uint8_t hopCount,
                              uint32_t ackTimeoutMs);

    bool startWorkerTask(uint32_t stackSize = 4096, uint8_t priority = 5);

    void poll();

    // Wait time under the duty-cycle gate for a message of totalLen bytes
    // (single-frame or fragmented). Returns UINT32_MAX if it can never fit.
    uint32_t getDutyCycleWaitMs(size_t totalLen) const;

private:
    struct PoolSlot { RFMessage msg; bool used; };
    PoolSlot  _pool[RF_POOL_SIZE];
    IMutex*   _poolMux = nullptr;
    IMutex*   _dutyMux = nullptr;

    // forFragment=true may use the last RF_LARGE_TX_SESSIONS slots, reserved
    // for fragment continuations so they can't starve behind deferred user
    // sends (which would otherwise deadlock the session).
    RFMessage* _poolAlloc(bool forFragment = false);
    void       _poolFree(RFMessage* msg);

    struct PendingEntry { RFMessage* msg; bool used; };
    PendingEntry _pending[RF_POOL_SIZE];

    struct ScheduledFwd {
        uint8_t  frame[RF_FRAME_BUF_SIZE];
        uint16_t len;
        uint32_t wakeTick;
        uint8_t  radioAddr;
        // CHANNEL_BUSY retry count (random backoff, up to RF_FWD_RETRY_MAX);
        // duty-cycle waits don't count against this.
        uint8_t  retries;
        bool     used;
    };
    ScheduledFwd _fwdQueue[RF_FWD_QUEUE_SIZE] = {};

    // Non-fragment sends parked here while a fragment session is in flight;
    // drained on completion. Sized to the pool so pool exhaustion is the
    // natural back-pressure boundary.
    struct DeferredSlot { RFMessage* msg; bool used; };
    DeferredSlot _deferred[RF_POOL_SIZE] = {};

    // ACKs that failed to go out (CCA busy / radio error), retried with
    // backoff so one collision doesn't fail the originator's pending message.
    struct AckPending {
        uint8_t  frame[PACKET_HEADER_SIZE + ACK_ENC_PLAINTEXT_SIZE + CRYPTO_TAG_SIZE];
        uint16_t len;
        uint8_t  radioAddr;
        uint32_t wakeTick;
        uint8_t  retries;
        bool     used;
    };
    AckPending _ackQueue[RF_ACK_QUEUE_SIZE] = {};

    // Outgoing TX parked with a wake time: originated-jitter delay, or
    // fragment retry backoff. Sized to RF_POOL_SIZE (one pool msg per entry).
    struct DelayedTxEntry { RFMessage* msg; uint32_t wakeTick; bool used; };
    DelayedTxEntry _delayedTx[RF_POOL_SIZE] = {};

    IRadio&      _radio;
    IOsal&       _osal;
    EngineConfig _cfg;
    DefaultNVBackend _nvOwned;
    DefaultClock     _clockOwned;
    INVBackend*      _nv;
    IClock*          _clock;
    DutyCycleTracker _duty;
    MonotonicCounter _nvSeq;
    SeenCache    _seen;
    ReplayWindow _replay;
    Reassembler  _reasm;
    LargeTxSession _largeTx[RF_LARGE_TX_SESSIONS];
    uint8_t      _nextOutgoingMsgId = 0;
    // High byte of the last seq seen in _sendMsg; a change means the 24-bit
    // wire seq wrapped (GCM nonce reuse risk under the current key).
    uint8_t      _seqWrapEpoch = 0;

    IQueue*      _eventQueue  = nullptr;
    int16_t      _lastRssi = RF_RSSI_UNKNOWN;
    volatile bool _workerStarted = false;
    volatile bool _workerExited  = false;
    // Makes repeated end() calls re-entrant while a worker shutdown is pending.
    bool _shuttingDownWorker = false;
    // Guards against double-freeing OS objects across repeated end()/destructor calls.
    bool _osObjectsFreed = false;
    // Logs the zero-airtime warning in _txAndAccount once, not every frame.
    bool _dutyAirtimeWarned = false;

    EngineSendCb     _cbSuccess = nullptr;  void* _ctxSuccess = nullptr;
    EngineSendFailCb _cbFailed  = nullptr;  void* _ctxFailed  = nullptr;
    EngineRecvCb     _cbRecv    = nullptr;  void* _ctxRecv    = nullptr;

    void _fireSend    (EngineSendCb     cb, void* ctx, RFMessage* msg);
    void _fireSendFail(EngineSendFailCb cb, void* ctx, RFMessage* msg, TxFailReason reason);

    bool _txAndAccount(const uint8_t* buf, uint16_t len, uint8_t dst,
                       uint32_t* outWaitMs,
                       TxResult* outRadioResult = nullptr);

    static void _workerEntry(void* pv);
    void _workerLoop();
    void _sendMsg(RFMessage* msg);
    void _checkTimeouts();
    void _handleRaw(uint8_t* buf, uint16_t len);
    void _scheduleForward(const uint8_t* frame, uint16_t len);
    void _processForwards();
    void _drainRadioEvents();
    OsMs _nextWakeMs() const;

    // True while a fragmented TX session holds the radio; gates non-fragment
    // TX_REQ and outgoing ACKs.
    bool _sessionInFlight() const;

    // Sends msg now, or parks it in _deferred until the active session ends;
    // drops with PENDING_LIST_FULL on _deferred overflow.
    void _routeTxReq(RFMessage* msg);

    // Drains _deferred through _routeTxReq (not _sendMsg — a callback mid-flush
    // may start a new session). Safe to call when no session is active.
    void _flushDeferred();

    // Parks an ACK that failed to send for retry; drops with a log on
    // _ackQueue overflow.
    void _scheduleAckRetry(const uint8_t* frame, uint16_t len, uint8_t radioAddr);

    // Retries queued ACKs with backoff on CHANNEL_BUSY, drops after
    // RF_ACK_RETRY_MAX. Skipped while a fragment session is in flight.
    void _processOutgoingAcks();

    uint32_t _resolveAckTimeoutMs(uint16_t dataFrameLen, uint8_t hopCount) const;

    // Reassembly inactivity timeout: at least RF_REASM_TIMEOUT_MS, scaled up
    // for slow profiles so it doesn't expire a slot between legitimate
    // fragment retransmissions.
    uint32_t _reasmTimeoutMs() const;
    uint8_t  _resolveL2(uint8_t appDst) const;

    // Pre-send CSMA-CA jitter in [0, RF_JITTER_WINDOW_SLOTS × ToA]; 0 if ToA
    // is unknown.
    uint32_t _jitterDelayMs(uint16_t frameLen);

    // Post-CHANNEL_BUSY retry backoff in [ToA, 2×ToA]; falls back to
    // RF_ACK_RETRY_FALLBACK_TOA_MS if airtime is unknown.
    uint32_t _busyBackoffMs(uint16_t frameLen);

    // Effective duty-cycle denominator: user override clamped to never loosen
    // below the radio's regulatory floor. 0 if duty tracking is disabled.
    uint16_t _currentDutyDenom() const;

    void _fillFragmentMsg(RFMessage* msg, int8_t sessionIdx);

    // Pushes the session's next fragment as TX_REQ (or delayed delayMs for
    // backoff). False if the pool is exhausted (session marked waitingForPool).
    bool _sendNextFragment(int8_t sessionIdx, uint32_t delayMs = 0);

    // False if no _delayedTx slot is free.
    bool _scheduleDelayedTx(RFMessage* msg, uint32_t delayMs);

    // Dispatches one due _delayedTx entry per tick back through _routeTxReq.
    void _processDelayedTx();

    // reason is only meaningful when success is false.
    void _completeLargeSend(int8_t sessionIdx, bool success, uint32_t lastSeq,
                            TxFailReason reason = TxFailReason::ACK_TIMEOUT);

    // Retries sessions that were waiting for a pool slot.
    void _driveFragmentSessions();

    // Worst-case total ToA for a userLen-byte message, single-frame or
    // fragmented; feeds getDutyCycleWaitMs and startLargeSend's pre-flight gate.
    uint32_t _computeWorstCaseToa(size_t userLen) const;
};
