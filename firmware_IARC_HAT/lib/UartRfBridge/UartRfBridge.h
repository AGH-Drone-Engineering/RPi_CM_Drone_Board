#pragma once
#include <Arduino.h>
#include <RFNode.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Wire type bytes
enum class FrameType : uint8_t
{
    GETMSG = 'G',
    SENDMSG = 'S',
    GETCONF = 'C',
    ACK = 'A',
};

class UartRfBridge
{
public:
    UartRfBridge(RFNode &node, Stream &host, uint8_t myAddr);

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
        std::vector<uint8_t> payload;
    };

    static constexpr size_t MAX_QUEUED_MESSAGES = 128;

    // Only one send is in flight at a time, so this bounds how many host
    // SENDMSGs may be buffered behind it. Each entry can hold up to
    // RF_MAX_FRAGMENTED_PAYLOAD bytes, so keep it small.
    static constexpr size_t MAX_QUEUED_TX = 4;

    RFNode &_node;
    Stream &_uart;
    uint8_t _myAddr;

    QueueHandle_t _rxQueue;
    QueuedMessage *_pendingGetmsg = nullptr;

    // TX side. _txQueue is written by poll()'s task (handleSendmsg) and read by
    // the same task (pumpTx), so it exists for bounding, not for cross-thread
    // handoff. _txDone is the one genuinely shared bit: set by the RF worker
    // task from onSendOk/onSendFail, consumed by pumpTx.
    QueueHandle_t _txQueue;
    PendingTx *_inFlightTx = nullptr;
    bool _txDone = false;

    // Frees a completed send and submits the next queued one. Called from
    // poll(), i.e. never while a fragment source buffer is still in use.
    void pumpTx();

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
