#include "UartRfBridge.h"
#include "port/Logger.h"

static constexpr char LOG_MODULE[] = "bridge";

UartRfBridge::UartRfBridge(RFNode &node, HardwareSerial &uart, uint8_t myAddr)
    : _node(node), _uart(uart), _myAddr(myAddr)
{
    _rxQueue = xQueueCreate(MAX_QUEUED_MESSAGES, sizeof(QueuedMessage *));
}

// ── RFNode event trampolines ────────────────────────────────────────────────

void UartRfBridge::onReceiveTrampoline(const RxInfo &info, const uint8_t *data, size_t len, void *ctx)
{
    static_cast<UartRfBridge *>(ctx)->onReceive(info, data, len);
}

void UartRfBridge::onSendOkTrampoline(const SentInfo &info, void *ctx)
{
    static_cast<UartRfBridge *>(ctx)->onSendOk(info);
}

void UartRfBridge::onSendFailTrampoline(const SentInfo &info, TxFailReason reason, void *ctx)
{
    static_cast<UartRfBridge *>(ctx)->onSendFail(info, reason);
}

void UartRfBridge::onReceive(const RxInfo &info, const uint8_t *data, size_t len)
{
    QueuedMessage *m = new QueuedMessage();
    m->senderId = info.from;
    m->payload.assign(data, data + len);

    if (xQueueSend(_rxQueue, &m, 0) != pdTRUE)
    {
        LOG_W(LOG_MODULE, "rx_queue_full, dropping message from=0x%02X", info.from);
        delete m;
    }
}

void UartRfBridge::onSendOk(const SentInfo &info)
{
    LOG_I(LOG_MODULE, "tx_ok to=0x%02X seq=%lu len=%u", info.to, (unsigned long)info.seq, (unsigned)info.payloadLen);
}

void UartRfBridge::onSendFail(const SentInfo &info, TxFailReason reason)
{
    LOG_W(LOG_MODULE, "tx_fail to=0x%02X seq=%lu reason=%d", info.to, (unsigned long)info.seq, (int)reason);
}

// ── CRC16 (CRC-16/XMODEM: poly 0x1021, init 0x0000, no reflect, no xorout) ──
// Must match firmware_CMDB/loracom/LoRaCom.cpp::getCRC byte-for-byte.
uint16_t UartRfBridge::crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// ── UART frame parser ───────────────────────────────────────────────────────

void UartRfBridge::poll()
{
    // Drop a partial frame if the stream went quiet mid-frame (see
    // FRAME_IDLE_TIMEOUT_MS). Only meaningful once we're past a clean HEADER
    // boundary — a fully-idle line at rest must never trigger a reset.
    bool midFrame = (_state != ParseState::HEADER) || (_headerFill != 0);
    if (midFrame && (millis() - _lastByteMs) > FRAME_IDLE_TIMEOUT_MS)
        resetParser();

    while (_uart.available() > 0)
    {
        _lastByteMs = millis();
        feed(static_cast<uint8_t>(_uart.read()));
    }
}

void UartRfBridge::resetParser()
{
    _state = ParseState::HEADER;
    _headerFill = 0;
    _curPayload.clear();
    _checksumFill = 0;
}

void UartRfBridge::feed(uint8_t byte)
{
    switch (_state)
    {
    case ParseState::HEADER:
    {
        _headerBuf[_headerFill++] = byte;
        if (_headerFill < sizeof(_headerBuf))
            break;

        _curType = static_cast<FrameType>(_headerBuf[0]);
        _curId = _headerBuf[1];
        _curLen = static_cast<uint32_t>(_headerBuf[2]) |
                  (static_cast<uint32_t>(_headerBuf[3]) << 8) |
                  (static_cast<uint32_t>(_headerBuf[4]) << 16) |
                  (static_cast<uint32_t>(_headerBuf[5]) << 24);

        if (_curLen > RF_MAX_FRAGMENTED_PAYLOAD)
        {
            // Garbage/desync — a real frame can never declare a payload this
            // large. Drop and resync on the next byte rather than blocking
            // forever waiting for bytes that will never arrive.
            resetParser();
            break;
        }

        if (_curLen == 0)
        {
            _state = ParseState::CHECKSUM;
        }
        else
        {
            _curPayload.reserve(_curLen);
            _state = ParseState::PAYLOAD;
        }
        break;
    }

    case ParseState::PAYLOAD:
        _curPayload.push_back(byte);
        if (_curPayload.size() >= _curLen)
            _state = ParseState::CHECKSUM;
        break;

    case ParseState::CHECKSUM:
    {
        _checksumBuf[_checksumFill++] = byte;
        if (_checksumFill < sizeof(_checksumBuf))
            break;

        uint16_t receivedCrc = static_cast<uint16_t>(_checksumBuf[0]) |
                                (static_cast<uint16_t>(_checksumBuf[1]) << 8);

        std::vector<uint8_t> crcInput;
        crcInput.reserve(2 + _curPayload.size());
        crcInput.push_back(static_cast<uint8_t>(_curType));
        crcInput.push_back(_curId);
        crcInput.insert(crcInput.end(), _curPayload.begin(), _curPayload.end());
        uint16_t crc = crc16(crcInput.data(), crcInput.size());

        bool valid = (crc == receivedCrc);
        FrameType type = _curType;
        uint8_t id = _curId;
        uint32_t len = _curLen;
        // Copy payload out before resetting parser state (which clears it).
        std::vector<uint8_t> payload = std::move(_curPayload);
        resetParser();

        onFrame(type, id, payload.data(), len, valid);
        break;
    }
    }
}

// ── Frame dispatch ───────────────────────────────────────────────────────────
void UartRfBridge::onFrame(FrameType type, uint8_t id, const uint8_t *payload, uint32_t len, bool checksumValid)
{
    if (!checksumValid)
        return; 

    switch (type)
    {
    case FrameType::GETMSG:
        if (len != 0)
            return; // malformed request, drop
        handleGetmsg();
        break;
    case FrameType::SENDMSG:
        handleSendmsg(id, payload, len);
        break;
    case FrameType::GETCONF:
        if (len != 0)
            return; // malformed request, drop
        handleGetconf();
        break;
    case FrameType::ACK:
        handleAck();
        break;
    default:
        break; // unknown type, drop
    }
}

void UartRfBridge::handleGetmsg()
{
    if (!_pendingGetmsg && xQueuePeek(_rxQueue, &_pendingGetmsg, 0) != pdTRUE)
    {
        sendAckFrame(); // queue empty
        return;
    }

    // Either freshly peeked, or a resend because the host's ACK for our
    // previous response never arrived — either way, same content.
    sendFrame(FrameType::GETMSG, _pendingGetmsg->senderId,
              _pendingGetmsg->payload.data(), _pendingGetmsg->payload.size());
}

void UartRfBridge::handleSendmsg(uint8_t destId, const uint8_t *payload, uint32_t len)
{
    sendAckFrame();
    SendStatus st = _node.sendAck(destId, payload, len);
    if (st != SendStatus::OK)
        LOG_W(LOG_MODULE, "sendmsg_queue_failed to=0x%02X status=%d", destId, (int)st);
}

void UartRfBridge::handleGetconf()
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "CMDB_ID=4");
    sendFrame(FrameType::GETCONF, 0, reinterpret_cast<const uint8_t *>(buf), static_cast<uint32_t>(n));
}

void UartRfBridge::handleAck()
{
    if (!_pendingGetmsg)
        return; // stray ACK (e.g. for a GETCONF response) — nothing to do

    QueuedMessage *popped = nullptr;
    xQueueReceive(_rxQueue, &popped, 0); // pops the same message we peeked (single consumer)
    delete popped;
    _pendingGetmsg = nullptr;
}

// ── Frame building ───────────────────────────────────────────────────────────

void UartRfBridge::sendFrame(FrameType type, uint8_t id, const uint8_t *payload, uint32_t len)
{
    uint8_t header[6];
    header[0] = static_cast<uint8_t>(type);
    header[1] = id;
    header[2] = static_cast<uint8_t>(len & 0xFF);
    header[3] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[4] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[5] = static_cast<uint8_t>((len >> 24) & 0xFF);

    std::vector<uint8_t> crcInput;
    crcInput.reserve(2 + len);
    crcInput.push_back(header[0]);
    crcInput.push_back(header[1]);
    if (len > 0 && payload != nullptr)
        crcInput.insert(crcInput.end(), payload, payload + len);
    uint16_t crc = crc16(crcInput.data(), crcInput.size());

    _uart.write(header, sizeof(header));
    if (len > 0 && payload != nullptr)
        _uart.write(payload, len);
    uint8_t crcBytes[2] = {static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>((crc >> 8) & 0xFF)};
    _uart.write(crcBytes, sizeof(crcBytes));
}

void UartRfBridge::sendAckFrame()
{
    sendFrame(FrameType::ACK, 0, nullptr, 0);
}
