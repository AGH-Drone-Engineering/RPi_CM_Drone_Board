#pragma once
#include <Arduino.h>
#include <RFNode.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// The commit this firmware was built from, generated into $BUILD_DIR by
// scripts/git_version.py (wired in as a pre-build extra_script, which also puts
// $BUILD_DIR on the include path). Lives here rather than in the .cpp because
// the mains log it at boot too. Guarded so this still compiles if the generator
// didn't run - a hand-rolled build, a stripped source export - in which case
// the stamp reads "unknown", the same fallback firmware_CMDB/loracom/main.cpp
// uses.
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef CMDB_ESP_FIRMWARE_BUILD
#define CMDB_ESP_FIRMWARE_BUILD "unknown"
#endif

// Wire type bytes
enum class FrameType : uint8_t
{
    GETMSG = 'G',
    SENDMSG = 'S',
    GETCONF = 'C',
    ACK = 'A',
};

// Shared by both variants built from this directory (main_node.cpp, which speaks
// the protocol over the RPi UART, and main_phone.cpp, which speaks it over
// USB-CDC to a phone). The transport is the only difference between them, and
// Stream is enough to erase it: HardwareSerial and HWCDC - what `Serial` is with
// ARDUINO_USB_CDC_ON_BOOT=1 - share nothing else, and the parser needs nothing
// beyond available()/read()/write().
class UartRfBridge
{
public:
    UartRfBridge(RFNode &node, Stream &uart, uint8_t myAddr);

    void poll();

    // RFNode event trampolines
    static void onReceiveTrampoline(const RxInfo &info, const uint8_t *data, size_t len, void *ctx);
    static void onSendOkTrampoline(const SentInfo &info, void *ctx);
    static void onSendFailTrampoline(const SentInfo &info, TxFailReason reason, void *ctx);

private:
    struct QueuedMessage
    {
        uint8_t senderId = 0;
        std::vector<uint8_t> payload;
    };

    // An outgoing SENDMSG waiting to be handed to RFNode, or currently in flight.
    // Owns its payload: for len > RF_MAX_PAYLOAD, RFNet fragments lazily and
    // keeps only a pointer to the caller's buffer (see LargeTxSession's
    // contract), reading later fragments long after the UART frame that carried
    // them is gone. The bytes must therefore outlive the send, not the parse.
    struct PendingTx
    {
        uint8_t dst = 0;
        // RF send attempts made so far. Saturates at 255 rather than wrapping —
        // retries are unbounded, so a wrap would restart the backoff schedule.
        uint8_t attempts = 0;
        // millis() before which this must not be attempted again. Per-message
        // rather than per-head: a failed message rotates to the tail, so several
        // can be waiting out their own backoffs at once.
        uint32_t notBeforeMs = 0;
        std::vector<uint8_t> payload;
    };

    // Attempts after which a failure stops looking like a glitch and starts
    // looking like a downed link. Up to here retries carry no added delay; past it
    // they are spaced by the escalating backoff below. With rotation the backoff
    // only bites on a short queue — a long one takes longer to come round than the
    // backoff itself.
    //
    // Note what is NOT here: a cap that drops the message. The host was ACKed the
    // moment this was queued and the protocol cannot report a later failure, so a
    // transient failure must never consume the message — out of range EVERY send
    // fails, and dropping after N attempts would empty a full queue into the void
    // in about two minutes. Only a failure the frame itself causes is terminal
    // (see isPermanentFailure/isPermanentReject in the .cpp).
    static constexpr uint8_t  TX_FAST_ATTEMPTS   = 3;
    static constexpr uint32_t TX_BACKOFF_BASE_MS = 500;
    static constexpr uint32_t TX_BACKOFF_MAX_MS  = 5000;

    static constexpr size_t MAX_QUEUED_MESSAGES = 128;

    // Only one send is in flight at a time, so this bounds how many host
    // SENDMSGs may be buffered behind it — and the host outruns the radio by
    // orders of magnitude: it gets its ACK as soon as a frame is queued (a few
    // hundred µs at 115200), while draining one costs airtime plus an ACK round
    // trip, tens to hundreds of ms. A 100-frame burst therefore has to sit
    // somewhere, and it has to be here: the engine's own pool can't absorb it
    // without breaking the ACK protocol (see RF_POOL_SIZE in RFConfig.h).
    //
    // Depth is the backlog to absorb. Sized for an out-of-range spell rather than
    // a burst: retries are unbounded, so nothing leaves while the link is down and
    // everything the host hands over has to fit here until range returns.
    //
    // The FreeRTOS queue is created one slot DEEPER than this and handleSendmsg
    // caps intake here explicitly. That spare slot belongs to the message
    // currently off the queue: a failed send rotates to the tail (requeueTx), and
    // since the queue can fill with new host traffic while that message is on the
    // air, without the reservation every retry under load would become a drop.
    //
    // MAX_QUEUED_TX_BYTES is the co-limit, since an entry costs only its actual
    // payload (heap) and not RF_MAX_FRAGMENTED_PAYLOAD. It exists to stop a peer
    // sending large messages from turning this depth into 512 × 3 kB of heap; keep
    // it above MAX_QUEUED_TX × your typical payload or IT becomes the binding
    // limit and the depth is decorative. 64 kB over 512 slots covers payloads up
    // to a 128 B average — this link carries 13-70 B. Worst-case heap is the cap
    // plus ~45 B of per-entry overhead (PendingTx + vector + allocator headers),
    // so ~87 kB.
    //
    // Whichever limit runs out first, the frame goes unACKed (handleSendmsg)
    // instead of being dropped silently — the host's 250 ms/4-attempt retry then
    // doubles as flow control, and past that it raises EREMOTEIO rather than
    // believing a message was sent.
    static constexpr size_t MAX_QUEUED_TX       = 512;
    static constexpr size_t MAX_QUEUED_TX_BYTES = 64 * 1024;

    RFNode &_node;
    Stream &_uart;
    uint8_t _myAddr;

    QueueHandle_t _rxQueue;
    QueuedMessage *_pendingGetmsg = nullptr;

    // TX side. _txQueue is written by poll()'s task (handleSendmsg) and read by
    // the same task (pumpTx), so it exists for bounding, not for cross-thread
    // handoff. _txDone/_txOk/_txRetryable are the genuinely shared bits: set by
    // the RF worker task from onSendOk/onSendFail, consumed by pumpTx. Only valid
    // once _txDone reads true (release/acquire pair), and _txRetryable only when
    // _txOk is false.
    //
    // _inFlightTx is the one message RFNet currently owns — off the queue, its
    // payload possibly still feeding the fragmenter. On completion it is freed,
    // dropped, or rotated back to the tail; it is never held here across attempts.
    QueueHandle_t _txQueue;
    PendingTx *_inFlightTx = nullptr;
    bool _txDone = false;
    bool _txOk = true;
    bool _txRetryable = false;

    // Payload bytes sitting in _txQueue, against MAX_QUEUED_TX_BYTES. Touched
    // only by handleSendmsg/pumpTx, both on the poll() task — no atomics needed.
    size_t _queuedTxBytes = 0;

    // Settles the completed send (free / drop / rotate) and then offers the next
    // due message to RFNet. Called from poll(), i.e. never while a fragment source
    // buffer is still in use.
    void pumpTx();

    // Hands tx to RFNet and takes ownership of its fate: in flight on OK, rotated
    // to the tail on a transient rejection, deleted on a permanent one. tx must not
    // be touched afterwards. False means RFNet refused it transiently, so offering
    // another message this round is pointless.
    bool submitTx(PendingTx *tx);

    // Returns tx to the tail for a later attempt, stamping its backoff. Takes
    // ownership; deletes tx only if the queue somehow has no room (unreachable
    // given the reserved slot).
    void requeueTx(PendingTx *tx);

    // Delay before the next attempt for a message that has failed `attempts`
    // times: 0 while within TX_FAST_ATTEMPTS, then doubling to TX_BACKOFF_MAX_MS.
    static uint32_t retryBackoffMs(uint8_t attempts);

    // -- UART frame parser state machine --
    enum class ParseState
    {
        HEADER,
        PAYLOAD,
        CHECKSUM
    };
    ParseState _state = ParseState::HEADER;
    uint8_t _headerBuf[6];
    size_t _headerFill = 0;
    FrameType _curType = FrameType::ACK;
    uint8_t _curId = 0;
    uint32_t _curLen = 0;
    std::vector<uint8_t> _curPayload;
    uint8_t _checksumBuf[2];
    size_t _checksumFill = 0;

    static constexpr uint32_t FRAME_IDLE_TIMEOUT_MS = 100;
    uint32_t _lastByteMs = 0;

    void feed(uint8_t byte);
    void resetParser();
    void onFrame(FrameType type, uint8_t id, const uint8_t *payload, uint32_t len, bool checksumValid);

    void handleGetmsg();
    void handleSendmsg(uint8_t destId, const uint8_t *payload, uint32_t len);
    void handleGetconf();
    void handleAck();

    void sendFrame(FrameType type, uint8_t id, const uint8_t *payload, uint32_t len);
    void sendAckFrame();

    void onReceive(const RxInfo &info, const uint8_t *data, size_t len);
    void onSendOk(const SentInfo &info);
    void onSendFail(const SentInfo &info, TxFailReason reason);

    static uint16_t crc16(const uint8_t *data, size_t len);
};
