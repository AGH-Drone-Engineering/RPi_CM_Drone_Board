#pragma once
#include <stdint.h>
#include <stddef.h>
#include "core/RFConfig.h"
#include "port/hal/nvs/INVBackend.h"

enum class BeginStatus : uint8_t {
    OK,
    ALREADY_STARTED,
    INVALID_CONFIG,
    SECURITY_INIT_FAILED,
    RADIO_INIT_FAILED,
    OUT_OF_MEMORY,
    // Encryption configured but NV backend is volatile (no persistence across reboot):
    // monotonic seq counter would restart every power cycle, reusing GCM (key, nonce) pairs
    // under the fixed key — refused. Provide a persistent INVBackend, or define
    // RF_ALLOW_VOLATILE_NV to override on non-production builds.
    NV_NOT_PERSISTENT,
    // Encryption configured and NV backend returned a readable but unrecognised seq counter
    // record (foreign data at that id, or bit-rot that flipped bits without erasing the whole
    // record). Trusting it — or silently resetting to 0 — risks reusing a GCM nonce already
    // used under the current key. Wipe the slot deliberately to reprovision, or point
    // nv.idSeq at an unused id.
    NV_SEQ_CORRUPTED,
};

enum class SendStatus : uint8_t {
    OK = 0,
    BAD_LENGTH,                 // len == 0 or > RF_MAX_FRAGMENTED_PAYLOAD
    POOL_EXHAUSTED,             // RFMessage pool full
    QUEUE_FULL,                 // event queue push failed
    NOT_INITIALIZED,            // begin() not called
    LARGE_TX_BUSY,              // all LargeTxSession slots occupied
    // Pre-flight duty-cycle gate (large sends only): required off-time not yet available.
    // Call getDutyCycleWaitMs(len) to learn how long to wait before retrying.
    DUTY_CYCLE_INSUFFICIENT,
    // Pre-flight duty-cycle gate (large sends only): worst-case total ToA exceeds the max
    // air-time one message may occupy under the resolved denominator (windowMs/denom) —
    // un-sendable regardless of waiting. Shorten the message, lower SF, or loosen the
    // override (within regulatory bounds). Returned as UINT32_MAX from getDutyCycleWaitMs(len).
    MESSAGE_TOO_LARGE_FOR_DUTY,
};

enum class TxFailReason : uint8_t {
    RADIO_ERROR,
    CHANNEL_BUSY,
    DUTY_CYCLE,
    PENDING_LIST_FULL,
    FRAME_BUILD_FAILED,
    ACK_TIMEOUT,
};

struct RxInfo {
    uint8_t from;
    bool    broadcast;
    // dBm; RF_RSSI_UNKNOWN (INT16_MIN) when the radio backend didn't report
    // a reading for this frame.
    int16_t rssi;
};

// Sentinel for SendOptions::hops: use the node's configured RFMeshConfig::hopCount instead of
// a per-message override. 0xFF is safe as a sentinel — real hop counts are a 3-bit wire field
// (0-7).
constexpr uint8_t RF_HOPS_USE_NODE_DEFAULT = 0xFF;

struct SendOptions {
    bool     requireAck   = false;
    uint32_t ackTimeoutMs = 0;
    // 0–7 per-message hop count override; 0 = no mesh forwarding for this message. Default
    // uses the node's configured RFMeshConfig::hopCount instead of a fixed compile-time value.
    uint8_t  hops         = RF_HOPS_USE_NODE_DEFAULT;
};

struct SentInfo {
    uint8_t        to;
    uint32_t       seq;
    const uint8_t* payload;       // valid only during the callback
    size_t         payloadLen;    // widened from uint8_t for large sends
    uint32_t       ackTimeoutMs;
    uint32_t       dutyWaitMs;
};

struct RFNvConfig {
    INVBackend* backend = nullptr;
    uint16_t idSeq  = 1;
    uint16_t idDuty = 2;
    uint32_t nonceCommitStep = 100;
    uint32_t dutyMinCommitMs = 2000;
};
