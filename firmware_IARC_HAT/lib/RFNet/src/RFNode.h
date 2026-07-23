#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "port/hal/radio/IRadio.h"
#include "port/osal/IOsal.h"
#include "port/osal/DefaultOsal.h"
#include "port/hal/clock/IClock.h"
#include "core/packet/Packet.h"
#include "core/routing/IRoutingStrategy.h"
#include "core/routing/ManagedFloodingStrategy.h"
#include "core/routing/StaticRoutingStrategy.h"
#include "core/security/ICypher.h"
#include "core/RFConfig.h"
#include "RFTypes.h"

class Engine;
class SecurityLayer;

struct RFSecurityConfig {
    enum class Source : uint8_t { None, Key, Password, Cipher };

    static RFSecurityConfig None() { return {}; }

    static RFSecurityConfig FromKey(const uint8_t (&k)[CRYPTO_KEY_SIZE]) {
        RFSecurityConfig c;
        c.source = Source::Key;
        c.key    = &k;
        return c;
    }

    static RFSecurityConfig FromPassword(const char* pw, uint32_t iters = RF_KDF_DEFAULT_ITERATIONS) {
        RFSecurityConfig c;
        c.source        = Source::Password;
        c.password      = pw;
        c.kdfIterations = iters;
        return c;
    }

    static RFSecurityConfig FromCipher(ICypher& cipher) {
        RFSecurityConfig c;
        c.source = Source::Cipher;
        c.cipher = &cipher;
        return c;
    }

    bool requireEncrypted = true;

    Source                            source        = Source::None;
    const uint8_t (*key)[CRYPTO_KEY_SIZE] = nullptr;
    const char*                       password      = nullptr;
    uint32_t                          kdfIterations = RF_KDF_DEFAULT_ITERATIONS;
    ICypher*                          cipher        = nullptr;
};

// Duty-cycle enforcement.
// - Exhausted budget: single-frame fails with TxFailReason::DUTY_CYCLE (wait
//   in SentInfo::dutyWaitMs); large sends return DUTY_CYCLE_INSUFFICIENT or
//   MESSAGE_TOO_LARGE_FOR_DUTY. Query wait via getDutyCycleWaitMs().
// - denominatorOverride: 1/N air-time fraction (100→1%, 1000→0.1%), clamped
//   up to the HAL's regulatory floor if looser. 0 = use HAL value.
// - Pair with an RTC-backed NV store: Toff deadline then survives reboot.
//   Without RTC it's tick-based and lost on restart — repeated reboots can
//   unknowingly exceed the regulatory limit.
// - enabled=true by default; engages when the radio's regulatory denominator
//   (getRegulatoryDutyDenominator) or denominatorOverride is non-zero — no-op
//   only when both are 0. Set false only if you manage air-time yourself.
struct RFDutyCycleConfig {
    bool     enabled             = true;
    uint16_t denominatorOverride = 0;
};

struct RFMeshConfig {
    // 0 = no mesh forwarding; 1–7 = max hops per outgoing message.
    uint8_t           hopCount      = RF_DEFAULT_HOP_COUNT;
    // Routing strategy. nullptr = built-in ManagedFloodingStrategy (plain
    // flooding: every node forwards unseen frames). Inject your own for RSSI
    // suppression or static routing. Ignored in P2P mode.
    IRoutingStrategy* routing       = nullptr;
    // Stagger forwards by a random fraction of K×ToA to avoid synchronized
    // re-broadcasts. Disable only in tightly-controlled test rigs.
    bool              forwardJitter = true;
};

struct RFReliabilityConfig {
    // Per-message ACK timeout. 0 = auto-resolve from frame size + mode.
    uint32_t ackTimeoutMs = 0;

    // Margin added on top of 2×ToA(ack) in P2P or the per-hop budget in Mesh.
    // 0 = use RF_ACK_TIMEOUT_MARGIN_P2P_MS / RF_ACK_TIMEOUT_MARGIN_MESH_MS.
    uint16_t ackTimeoutMarginP2Pms  = 0;
    uint16_t ackTimeoutMarginMeshMs = 0;

    // Per-fragment TX retry budget within a large-send session.
    // 0 = one failed fragment kills the session.
    uint8_t fragRetryMax = RF_FRAG_RETRY_MAX;

    // Outgoing ACK retry budget when CCA is busy or radio.send fails.
    uint8_t ackRetryMax = RF_ACK_RETRY_MAX;
};

struct RFNodeConfig {
    // Node address in [0x01, 0xFE]; 0xFF is broadcast, 0x00 is reserved.
    uint8_t           addr  = 0x00;
    PacketMode        mode  = PacketMode::P2P;
    IClock*           clock = nullptr;

    RFSecurityConfig    security    = {};
    RFDutyCycleConfig   dutyCycle   = {};
    RFMeshConfig        mesh        = {};
    RFReliabilityConfig reliability = {};
    RFNvConfig          nv          = {};
};

// RFNode — single user-facing facade. Construct with radio + config, wire
// callbacks, call begin(), drive via poll() or startWorkerTask().
// DI convention: radio (required) is a constructor reference, never null.
// Optional collaborators (clock, NV backend, routing, cipher) are nullable
// pointers in RFNodeConfig — null = built-in default. osal has no cfg field;
// inject only via the second constructor overload (mandatory reference).
class RFNode {
public:
    // Callback signatures. `ctx`: opaque pointer passed back to the handler.
    // RecvCb `data` is valid only during the call (points into an internal
    // buffer — copy what you need). `len` is size_t: large reassembled
    // messages deliver whole, never truncated.
    using RecvCb       = void(*)(const RxInfo& info, const uint8_t* data, size_t len, void* ctx);
    using OnSendOkCb   = void(*)(const SentInfo& info, void* ctx);
    using OnSendFailCb = void(*)(const SentInfo& info, TxFailReason reason, void* ctx);

    // Uses built-in DefaultOsal — for single-RTOS targets needing no custom OS abstraction.
    RFNode(IRadio& radio, const RFNodeConfig& cfg);
    // Inject your own OSAL (FreeRTOS / POSIX / bare-metal port); differs from the default
    // only in which IOsal the node drives.
    RFNode(IRadio& radio, IOsal& osal, const RFNodeConfig& cfg);

    ~RFNode();

    // Brings the node up: derives the key (if password/key configured), starts the radio, arms
    // RX. Call exactly once before any send. Returns OK or the first failure reason —
    // [[nodiscard]], check it. Safe to end() then begin() again to retry.
    [[nodiscard]] BeginStatus begin();
    // Tears down: stops the worker (if any), powers the radio down, frees owned resources.
    // Idempotent — no-op if begin() never succeeded. Called automatically by the destructor.
    void end();

    // Register callbacks; each fires from whichever context drives the node
    // (worker task or poll() thread). Keep handlers short/non-blocking — a
    // slow one delays ACKs and can turn delivered messages into spurious ACK
    // timeouts. ctx=nullptr if unused. Set before begin() to not miss early events.
    void onReceive (RecvCb       cb, void* ctx = nullptr);
    void onSendOk  (OnSendOkCb  cb, void* ctx = nullptr);
    void onSendFail(OnSendFailCb cb, void* ctx = nullptr);

    // ── TX API ──────────────────────────────────────────────────────────
    // send()/sendAck()/sendBroadcast() (+ char* overloads) return SendStatus
    // for queued-or-not only; real outcome (sent, ACK_TIMEOUT, CHANNEL_BUSY,
    // etc.) arrives via onSendOk/onSendFail. len > RF_MAX_PAYLOAD
    // auto-fragments (data pointer must stay valid until the send callback).

    // Custom options: ACK, hop count, ACK timeout.
    [[nodiscard]] SendStatus send(uint8_t dst, const void* data, size_t len, const SendOptions& opts);
    [[nodiscard]] SendStatus send(uint8_t dst, const char* str, const SendOptions& opts) {
        return send(dst, static_cast<const void*>(str), strlen(str), opts);
    }

    // Fire-and-forget (no ACK).
    [[nodiscard]] SendStatus send(uint8_t dst, const void* data, size_t len);
    [[nodiscard]] SendStatus send(uint8_t dst, const char* str) {
        return send(dst, static_cast<const void*>(str), strlen(str));
    }

    // Reliable: ACK with auto-resolved timeout, one attempt only — retry
    // yourself via onSendFail if needed.
    [[nodiscard]] SendStatus sendAck(uint8_t dst, const void* data, size_t len);
    [[nodiscard]] SendStatus sendAck(uint8_t dst, const char* str) {
        return sendAck(dst, static_cast<const void*>(str), strlen(str));
    }

    // Broadcast: never waits for an ACK.
    [[nodiscard]] SendStatus sendBroadcast(const void* data, size_t len);
    [[nodiscard]] SendStatus sendBroadcast(const char* str) {
        return sendBroadcast(static_cast<const void*>(str), strlen(str));
    }

    // Spawns a background task owning the node (RX/TX/ACKs/forwards/timers) —
    // no poll() needed. Recommended on an RTOS. Returns false if task creation
    // failed. Mutually exclusive with poll() — never use both.
    // stackBytes/priority pass straight to the OSAL.
    bool startWorkerTask(uint32_t stackBytes = 4096, uint8_t priority = 5);

    // Cooperative driver for setups without a worker task: call often (e.g.
    // every loop() iteration). Services one batch of radio events, TX
    // requests, ACK/forward retries and timeouts per call. No-op once
    // startWorkerTask() has started. RX/ACK latency bounded by call frequency.
    void poll();

    // Advisory pre-flight query — sends nothing. Pass raw application bytes (same value
    // you'd give send()); overhead added internally.
    //   0          → no pending off-time, safe to send now
    //   N ms       → approximate wait until this message may be sent
    //   UINT32_MAX → message can never fit the resolved denominator (its air-time alone
    //                exceeds windowMs/denom); shorten it, lower the SF, or loosen
    //                denominatorOverride. Not returned when duty is off.
    // ADVISORY only — the real gate is inside send(). Value can drift as the
    // off-time deadline elapses and ACKs/mesh forwards arm new off-times.
    // Always 0 when duty cycle is disabled.
    uint32_t getDutyCycleWaitMs(size_t totalLen) const;

private:
    DefaultOsal    _defaultOsal;
    IOsal&         _osal;
    IRadio&        _radio;
    RFNodeConfig   _cfg;

    uint8_t        _key[CRYPTO_KEY_SIZE] = {};
    // Installed automatically in Mesh mode when cfg.mesh.routing == nullptr.
    ManagedFloodingStrategy _defaultFlooding;
    ICypher*       _cipher     = nullptr;
    bool           _ownsCipher = false;
    SecurityLayer* _sec        = nullptr;
    Engine*        _engine     = nullptr;

    RecvCb       _recvCb    = nullptr;  void* _recvCtx    = nullptr;
    OnSendOkCb   _cbSendOk  = nullptr;  void* _ctxSendOk  = nullptr;
    OnSendFailCb _cbSendFail = nullptr;  void* _ctxSendFail = nullptr;

    SendStatus _sendImpl(uint8_t dst, const void* data, size_t len, const SendOptions& opts);
    void _setupCallbacks();
};

// ── Platform implementations ─────────────────────────────────────────────────
// All guarded by their own platform macros — safe to include unconditionally.

// Radios
#include "port/hal/radio/radiolib/lora/SX1262/SX1262LoRaRadio.h"
#include "port/hal/radio/radiolib/fsk/CC1101/CC1101FSKRadio.h"

// OSALs
#include "port/osal/osal_freertos/FreeRtosOsal.h"
#include "port/osal/osal_baremetal/BaremetalOsal.h"
#include "port/osal/osal_posix/PosixOsal.h"

// Clocks
#include "port/hal/clock/esp/EspClock.h"
#include "port/hal/clock/null/NullClock.h"
#include "port/hal/clock/posix/PosixClock.h"
#include "port/hal/clock/zephyr/ZephyrClock.h"

// NVS backends
#include "port/hal/nvs/esp/PreferencesBackend.h"
#include "port/hal/nvs/arduino/EepromBackend.h"
#include "port/hal/nvs/null/NullNVBackend.h"
#include "port/hal/nvs/zephyr/ZephyrNvsBackend.h"

// Cipher backends — each self-guards on platform/library availability, so
// including all unconditionally is safe; only the applicable ones define
// their class. DefaultCipher.h additionally picks the auto-selected default.
#include "port/crypto/DefaultCipher.h"
#include "port/crypto/soft/AesGcmSoft.h"
#include "port/crypto/mbedtls/AesGcmMbedTls.h"
#include "port/crypto/stm32hal/AesGcmStm32Hal.h"
