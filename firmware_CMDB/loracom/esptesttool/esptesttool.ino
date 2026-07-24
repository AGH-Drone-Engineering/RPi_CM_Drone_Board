// Mock ESP-side implementation of the loracom wire protocol (see
// ../protocol.adoc), for testing loracom/bootstrap against real UART
// hardware without real LoRa firmware.
//
// Behavior:
//   - SENDMSG: the (sender id, payload) is queued and ACKed. No actual LoRa
//     TX happens - it's purely a loopback so a later GETMSG echoes it back.
//   - GETMSG: replies with the oldest queued message, or ACK if empty.
//   - GETCONF: always replies with the hardcoded "CMDB_ID=4".
//   - CHECKSUM is written as 0x0000 on every reply, matching loracom's
//     current CRC stub (LoRaCom::getCRC() always returns 0) - the host
//     doesn't verify it yet either. Update both together once real CRC16
//     lands on both sides.
//
// This is a simple, synchronous mock (no device-side retransmission on a
// lost ACK) - good enough for exercising loracom's send/get/config paths,
// not a stand-in for real link robustness.
//
// Board: ESP32S3 Dev Module (Arduino IDE/arduino-cli, esp32 board package).
//
// Wiring (per this board's UART, not the ESP's USB serial):
//   RPi GPIO8 -> ESP32-S3 IO18 (UART_RX_PIN below)
//   RPi GPIO9 <- ESP32-S3 IO17 (UART_TX_PIN below)
// 8N1 @ 115200, matching BasicUart's defaults in loracom.

#include <Arduino.h>

constexpr int UART_RX_PIN = 18;
constexpr int UART_TX_PIN = 17;
constexpr uint32_t UART_BAUD = 115200;
constexpr uint32_t UART_READ_TIMEOUT_MS = 250;

constexpr uint8_t TYPE_CONFREQ = 'C';
constexpr uint8_t TYPE_SENDMSG = 'S';
constexpr uint8_t TYPE_GETMSG = 'G';
constexpr uint8_t TYPE_ACK = 'A';

constexpr size_t MAX_PAYLOAD = 256;
constexpr size_t QUEUE_CAPACITY = 8;

const char* const HARDCODED_CONFIG = "CMDB_ID=4";

struct QueuedMessage
{
    uint8_t senderId;
    String payload;
};

QueuedMessage queue[QUEUE_CAPACITY];
size_t queueHead = 0;
size_t queueCount = 0;

void enqueue(uint8_t senderId, const String& payload)
{
    size_t tail = (queueHead + queueCount) % QUEUE_CAPACITY;
    if (queueCount == QUEUE_CAPACITY) {
        // Full: drop the oldest to make room, matching protocol.adoc's note
        // that SENDMSG's ACK confirms receipt only, not eventual delivery.
        queueHead = (queueHead + 1) % QUEUE_CAPACITY;
        --queueCount;
        tail = (queueHead + queueCount) % QUEUE_CAPACITY;
    }
    queue[tail].senderId = senderId;
    queue[tail].payload = payload;
    ++queueCount;
}

bool dequeue(QueuedMessage& out)
{
    if (queueCount == 0) {
        return false;
    }
    out = queue[queueHead];
    queueHead = (queueHead + 1) % QUEUE_CAPACITY;
    --queueCount;
    return true;
}

void sendFrame(uint8_t type, uint8_t senderId, const String& payload)
{
    uint32_t length = payload.length();
    uint8_t header[6] = {
        type, senderId,
        static_cast<uint8_t>(length & 0xFF),
        static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>((length >> 16) & 0xFF),
        static_cast<uint8_t>((length >> 24) & 0xFF),
    };
    Serial1.write(header, sizeof(header));
    if (length > 0) {
        Serial1.write(reinterpret_cast<const uint8_t*>(payload.c_str()), length);
    }
    uint8_t checksum[2] = {0, 0};
    Serial1.write(checksum, sizeof(checksum));
}

void handleGetMsg()
{
    QueuedMessage msg;
    if (dequeue(msg)) {
        sendFrame(TYPE_GETMSG, msg.senderId, msg.payload);
    } else {
        sendFrame(TYPE_ACK, 0, "");
    }
}

void handleSendMsg(uint8_t senderId, const String& payload)
{
    enqueue(senderId, payload);
    sendFrame(TYPE_ACK, 0, "");
}

void handleGetConf()
{
    sendFrame(TYPE_CONFREQ, 0, HARDCODED_CONFIG);
}

void handleFrame()
{
    uint8_t header[6];
    if (Serial1.readBytes(header, sizeof(header)) != sizeof(header)) {
        return; // incomplete header within the timeout - drop
    }

    uint8_t type = header[0];
    uint8_t senderId = header[1];
    uint32_t length = header[2] | (static_cast<uint32_t>(header[3]) << 8) |
                       (static_cast<uint32_t>(header[4]) << 16) | (static_cast<uint32_t>(header[5]) << 24);

    String payload;
    if (length > 0) {
        if (length > MAX_PAYLOAD) {
            return; // implausible length - drop rather than allocate wildly
        }
        uint8_t buf[MAX_PAYLOAD];
        if (Serial1.readBytes(buf, length) != length) {
            return; // incomplete payload within the timeout - drop
        }
        payload = String(reinterpret_cast<const char*>(buf), length);
    }

    uint8_t checksum[2];
    Serial1.readBytes(checksum, sizeof(checksum)); // not verified - see file header

    switch (type) {
        case TYPE_GETMSG:
            handleGetMsg();
            break;
        case TYPE_SENDMSG:
            handleSendMsg(senderId, payload);
            break;
        case TYPE_CONFREQ:
            handleGetConf();
            break;
        case TYPE_ACK:
        default:
            break; // nothing to do
    }
}

void setup()
{
    Serial.begin(115200); // USB debug console
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial1.setTimeout(UART_READ_TIMEOUT_MS);
}

void loop()
{
    if (Serial1.available() > 0) {
        handleFrame();
    }
}
