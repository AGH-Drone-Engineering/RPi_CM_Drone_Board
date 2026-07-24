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
    UartRfBridge(RFNode &node, HardwareSerial &uart, uint8_t myAddr);

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

    static constexpr size_t MAX_QUEUED_MESSAGES = 128;

    RFNode &_node;
    HardwareSerial &_uart;
    uint8_t _myAddr;

    QueueHandle_t _rxQueue;
    QueuedMessage *_pendingGetmsg = nullptr;

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
