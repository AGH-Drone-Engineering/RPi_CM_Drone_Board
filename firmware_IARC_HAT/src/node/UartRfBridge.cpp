#include "UartRfBridge.h"
#include "port/Logger.h"

static constexpr char LOG_MODULE[] = "bridge";

UartRfBridge::UartRfBridge(RFNode &node, Stream &uart, uint8_t myAddr)
    : _node(node), _uart(uart), _myAddr(myAddr)
{
    _rxQueue = xQueueCreate(MAX_QUEUED_MESSAGES, sizeof(QueuedMessage *));
    // +1 is the retry reservation, not extra capacity for the host — see
    // MAX_QUEUED_TX, and handleSendmsg which caps intake at MAX_QUEUED_TX.
    _txQueue = xQueueCreate(MAX_QUEUED_TX + 1, sizeof(PendingTx *));
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

// Both of these run on the RF worker task. They only publish "the payload is no
// longer needed" - pumpTx() does the freeing on the poll() task.
void UartRfBridge::onSendOk(const SentInfo &info)
{
    LOG_I(LOG_MODULE, "tx_ok to=0x%02X seq=%lu len=%u", info.to, (unsigned long)info.seq, (unsigned)info.payloadLen);
    __atomic_store_n(&_txDone, true, __ATOMIC_RELEASE);
}

// A failure the frame itself causes: every retry reproduces it exactly, so the
// message is terminal. Everything else — ACK_TIMEOUT above all, which is what an
// out-of-range peer looks like — is transient and must be retried without limit.
// DUTY_CYCLE and PENDING_LIST_FULL are local congestion, also transient.
static bool isPermanentFailure(TxFailReason reason)
{
    return reason == TxFailReason::FRAME_BUILD_FAILED;
}

// Same split for a send RFNet refuses outright, where the verdict arrives as a
// SendStatus instead of a callback. POOL_EXHAUSTED / QUEUE_FULL / LARGE_TX_BUSY /
// DUTY_CYCLE_INSUFFICIENT all clear on their own.
static bool isPermanentReject(SendStatus st)
{
    return st == SendStatus::BAD_LENGTH ||
           st == SendStatus::NOT_INITIALIZED ||
           st == SendStatus::MESSAGE_TOO_LARGE_FOR_DUTY;
}

void UartRfBridge::onSendFail(const SentInfo &info, TxFailReason reason)
{
    LOG_W(LOG_MODULE, "tx_fail to=0x%02X seq=%lu reason=%d", info.to, (unsigned long)info.seq, (int)reason);
    // Both ordered before the _txDone release below, which pumpTx acquires — so a
    // reader that sees the completion also sees the verdict.
    __atomic_store_n(&_txRetryable, !isPermanentFailure(reason), __ATOMIC_RELAXED);
    __atomic_store_n(&_txOk, false, __ATOMIC_RELAXED);
    __atomic_store_n(&_txDone, true, __ATOMIC_RELEASE);
}

uint32_t UartRfBridge::retryBackoffMs(uint8_t attempts)
{
    if (attempts < TX_FAST_ATTEMPTS)
        return 0; // the ACK timeout already spaced these

    // Clamped before the shift, not after: TX_BACKOFF_BASE_MS << 32 is undefined,
    // and attempts saturates at 255.
    uint32_t shift = (uint32_t)(attempts - TX_FAST_ATTEMPTS);
    if (shift > 4)
        shift = 4;
    const uint32_t ms = TX_BACKOFF_BASE_MS << shift;
    return ms > TX_BACKOFF_MAX_MS ? TX_BACKOFF_MAX_MS : ms;
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
    while (_uart.available() > 0)
    {
        _lastByteMs = millis();
        feed(static_cast<uint8_t>(_uart.read()));
    }

    // Drop a partial frame if the stream went quiet mid-frame (see
    // FRAME_IDLE_TIMEOUT_MS). Only meaningful once we're past a clean HEADER
    // boundary — a fully-idle line at rest must never trigger a reset.
    //
    // Checked AFTER the drain, never before: _lastByteMs tracks when we last
    // *read* a byte, not when the line last carried one. Anything that stalls
    // this task past the timeout — a blocking log write, a multi-kB GETMSG reply
    // going out over UART — would otherwise come back to a mid-frame parser and
    // discard a frame whose remaining bytes are already sitting in the RX ring.
    // Draining first means a reset can only fire when the ring really is empty.
    bool midFrame = (_state != ParseState::HEADER) || (_headerFill != 0);
    if (midFrame && (millis() - _lastByteMs) > FRAME_IDLE_TIMEOUT_MS)
        resetParser();

    pumpTx();
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

// The ACK is deliberately withheld until the message is queued for transmission
// (see sendAckFrame() at the bottom). Anything that makes this message
// undeliverable — empty payload, either queue bound — returns without ACKing, so
// the host's retry/timeout path reports it instead of the drop passing for a send.
// The window it can't cover is a permanently undeliverable frame, discovered only
// once RFNet or the radio rejects it: by then the ACK is out. That's the price of
// queueing more than one message deep, and it fails loudly as tx_drop. Transient
// failures cost nothing here — those rotate and retry (pumpTx).
void UartRfBridge::handleSendmsg(uint8_t destId, const uint8_t *payload, uint32_t len)
{
    if (len == 0)
    {
        // RFNet rejects empty sends, so this one can never go out.
        LOG_W(LOG_MODULE, "sendmsg_empty to=0x%02X, dropping", destId);
        return;
    }

    // Checked explicitly instead of letting xQueueSend fail: the queue is one slot
    // deeper than MAX_QUEUED_TX so a rotating retry always has room, and new host
    // traffic must not be able to eat that slot.
    if (uxQueueMessagesWaiting(_txQueue) >= MAX_QUEUED_TX)
    {
        LOG_W(LOG_MODULE, "tx_queue_full, dropping message to=0x%02X len=%u",
              destId, (unsigned)len);
        return;
    }

    if (_queuedTxBytes + len > MAX_QUEUED_TX_BYTES)
    {
        LOG_W(LOG_MODULE, "tx_queue_bytes_full (%u queued), dropping message to=0x%02X len=%u",
              (unsigned)_queuedTxBytes, destId, (unsigned)len);
        return;
    }

    // Copy out of the parser's frame buffer: `payload` dies with the feed()
    // call, while a fragmented send reads from it for as long as the session
    // runs. pumpTx() hands it over and keeps it alive until then.
    PendingTx *tx = new PendingTx();
    tx->dst = destId;
    tx->payload.assign(payload, payload + len);

    if (xQueueSend(_txQueue, &tx, 0) != pdTRUE)
    {
        // Unreachable given the depth check above; kept so a future change to
        // either bound can't turn into a leak.
        LOG_E(LOG_MODULE, "tx_queue_send_failed to=0x%02X len=%u", destId, (unsigned)len);
        delete tx;
        return;
    }

    _queuedTxBytes += len;
    sendAckFrame();
}

void UartRfBridge::requeueTx(PendingTx *tx)
{
    // Read before handing the pointer over: once it is in the queue it is no
    // longer ours to dereference.
    const size_t   len = tx->payload.size();
    const uint8_t  dst = tx->dst;

    // Spacing for the NEXT attempt. On a long queue the lap time exceeds this
    // anyway, so it only actually paces a short one.
    tx->notBeforeMs = millis() + retryBackoffMs(tx->attempts);

    if (xQueueSend(_txQueue, &tx, 0) != pdTRUE)
    {
        // Unreachable: the reserved slot is exactly this message's own.
        LOG_E(LOG_MODULE, "tx_requeue_failed to=0x%02X len=%u — dropping",
              dst, (unsigned)len);
        delete tx;
        return;
    }
    _queuedTxBytes += len;
}

bool UartRfBridge::submitTx(PendingTx *tx)
{
    if (tx->attempts < 255)
        ++tx->attempts;

    // Armed before the send so a callback landing immediately after can't be missed.
    __atomic_store_n(&_txOk, true, __ATOMIC_RELAXED);
    __atomic_store_n(&_txDone, false, __ATOMIC_RELEASE);

    const SendStatus st = _node.sendAck(tx->dst, tx->payload.data(), tx->payload.size());
    if (st == SendStatus::OK)
    {
        _inFlightTx = tx;
        return true;
    }

    // A refused send never reaches a callback, so settle it here.
    __atomic_store_n(&_txDone, true, __ATOMIC_RELEASE);

    if (isPermanentReject(st))
    {
        LOG_W(LOG_MODULE, "tx_drop to=0x%02X len=%u status=%d (permanent)",
              tx->dst, (unsigned)tx->payload.size(), (int)st);
        delete tx;
        return true; // the next message may still get through
    }

    LOG_W(LOG_MODULE, "tx_requeue to=0x%02X status=%d attempts=%u",
          tx->dst, (int)st, (unsigned)tx->attempts);
    requeueTx(tx);
    return false; // RFNet is congested — offering another one now is pointless
}

void UartRfBridge::pumpTx()
{
    // Settle the completed send first: it holds the single in-flight slot.
    if (_inFlightTx)
    {
        // The payload is still feeding the fragmenter until the send completes.
        if (!__atomic_load_n(&_txDone, __ATOMIC_ACQUIRE))
            return;

        PendingTx *done = _inFlightTx;
        _inFlightTx = nullptr;

        if (__atomic_load_n(&_txOk, __ATOMIC_ACQUIRE))
        {
            delete done;
        }
        else if (!__atomic_load_n(&_txRetryable, __ATOMIC_RELAXED))
        {
            LOG_W(LOG_MODULE, "tx_drop to=0x%02X len=%u attempts=%u (permanent)",
                  done->dst, (unsigned)done->payload.size(), (unsigned)done->attempts);
            delete done;
        }
        else
        {
            // Transient — rotate to the TAIL rather than retrying in place. An
            // unreachable peer must not wedge everything behind it: it comes round
            // again on the next lap and keeps being retried, unbounded, while every
            // other message still gets its turn. The cost is delivery order.
            LOG_W(LOG_MODULE, "tx_requeue to=0x%02X len=%u attempts=%u",
                  done->dst, (unsigned)done->payload.size(), (unsigned)done->attempts);
            requeueTx(done);
        }
    }

    // Peek before taking: the head gates on its own backoff, and dequeuing it just
    // to put it back would spin. It rotates on every failure, so a stall here lasts
    // one backoff at most, never indefinitely.
    PendingTx *next = nullptr;
    while (!_inFlightTx && xQueuePeek(_txQueue, &next, 0) == pdTRUE)
    {
        if ((int32_t)(millis() - next->notBeforeMs) < 0)
            return; // not due yet

        xQueueReceive(_txQueue, &next, 0);
        // Off the queue, so it no longer counts against MAX_QUEUED_TX_BYTES —
        // the one in-flight payload isn't budgeted.
        _queuedTxBytes -= next->payload.size();

        if (!submitTx(next))
            return;
    }
}

void UartRfBridge::handleGetconf()
{
    // Sized for the longest payload this can produce:
    //   "CMDB_ID=" 8 + "255" 3 + separator 1
    //   + "CMDB_ESP_FIRMWARE_BUILD=" 24 + "<8-char hash>-dirty" 14 + NUL 1 = 51.
    // No value may contain whitespace: the host splits the payload on it and
    // word-splits the result into environment variables (see
    // firmware_CMDB/bootstrap/bootstrap.sh).
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "CMDB_ID=%u CMDB_ESP_FIRMWARE_BUILD=%s",
                     static_cast<unsigned>(_myAddr), CMDB_ESP_FIRMWARE_BUILD);

    // snprintf returns the length it *would* have written, not what it did:
    // negative on an output error, >= sizeof(buf) once it truncates. Either
    // passed straight to sendFrame would read past the end of buf. What's
    // actually in buf is a NUL-terminated string of at most sizeof(buf) - 1
    // characters, so clamp to that. Neither case is reachable with the format
    // above; they're guarded so a later addition can't turn into an overread.
    if (n < 0)
        return; // nothing sane to report - the host retries on its timeout
    size_t len = static_cast<size_t>(n);
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;

    sendFrame(FrameType::GETCONF, 0, reinterpret_cast<const uint8_t *>(buf),
              static_cast<uint32_t>(len));
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
