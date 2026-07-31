
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "BasicUart.h"

constexpr uint32_t DEFAULT_ACK_TIMEOUT_MS = 250;
constexpr uint32_t DEFAULT_MAX_RETRIES = 3;

// Max SENDMSG payload size. Frames must fit in the ESP's default 256-byte
// Arduino UART RX buffer: 256 - 6 (header) - 2 (checksum) = 248.
constexpr size_t MAX_MESSAGE_SIZE = 248;

// Largest payload length a reply frame may declare. Mirrors the HAT's
// RF_MAX_FRAGMENTED_PAYLOAD (firmware_IARC_HAT/lib/RFNet/src/core/RFConfig.h) -
// anything above it means we're desynced on the byte stream and not looking at
// a real frame header.
constexpr size_t MAX_FRAME_PAYLOAD = 3072;

enum class TransmissionType : uint8_t
{
    CONFREQ = 'C',
    SENDMSG = 'S',
    GETMSG = 'G',
    ACK = 'A',
};

struct Transmission
{
    uint8_t senderId;
    std::string payload;
};

// Actual implementation of LoRa communication using the BasicUart

class LoRaCom : public BasicUart
{
public:
    LoRaCom(const std::string& device, uint32_t baudrate,
            uint32_t timeoutMs = DEFAULT_ACK_TIMEOUT_MS, uint32_t maxRetries = DEFAULT_MAX_RETRIES)
        : BasicUart(device, baudrate), timeoutMs_(timeoutMs), maxRetries_(maxRetries) {};

    // Both throw std::system_error (errno-coded, std::generic_category()) on
    // failure - e.g. EREMOTEIO (no response after retries), or whatever errno
    // the underlying write()/read() call reported. sendTransmission() returns
    // true on success (it never returns false - failure is always an
    // exception, kept as bool for interface stability). getTransmission()
    // does NOT throw for a legitimately empty message/config queue - that's
    // std::nullopt, not an error.
    //
    // Does NOT check payload size against MAX_MESSAGE_SIZE - that's the
    // caller's job (see main.cpp's send()), so callers can decide whether to
    // enforce it (e.g. --force).
    bool sendTransmission(TransmissionType type, uint8_t destId, const std::string& payload);

    // force=true accepts a reply even if its checksum doesn't match, instead
    // of treating it as corrupted (silently dropping it and waiting for a
    // retransmit, per protocol). Use to recover a message despite a known-bad
    // link.
    std::optional<Transmission> getTransmission(TransmissionType type, bool force = false);

private:
    struct ParsedFrame
    {
        TransmissionType type;
        uint8_t senderId;
        std::string payload;
        bool checksumValid;
    };

    // CRC-16/XMODEM (poly 0x1021, init 0x0000, no reflect, no xorout) over
    // `data`. Must match the HAT's implementation byte-for-byte, see
    // firmware_IARC_HAT/src/node/UartRfBridge.cpp::crc16.
    uint16_t getCRC(const std::vector<uint8_t>& data);

    // Recomputes the frame's checksum (over type, senderId and payload - the
    // length field is not covered) and compares it against the one the frame
    // carried.
    bool verifyChecksum(const ParsedFrame& frame, uint16_t receivedChecksum);

    std::vector<uint8_t> buildFrame(TransmissionType type, uint8_t id, const std::string& payload);

    // Reads until one complete frame has been accumulated, or until timeoutMs
    // has elapsed in total (the timeout bounds the whole frame, not each
    // individual read). A single read() only returns whatever happens to sit in
    // the kernel buffer at that instant - the HAT emits a frame as three
    // separate writes (header/payload/checksum), so anything but the shortest
    // frames routinely arrives in pieces. Bytes past the end of the frame stay
    // in rxBuf_ for the next call, so back-to-back frames don't get lost.
    std::optional<ParsedFrame> readFrame(uint32_t timeoutMs);

    // Waits up to timeoutMs_ for a reply. If a frame arrives but its checksum is
    // bad, waits an additional 2*timeoutMs_ for the far end to notice we never
    // ACKed and retransmit on its own, instead of immediately resending our
    // request. Returns nullopt if nothing usable arrives either way.
    // force=true skips all of that and returns the first frame as received,
    // checksum failure or not.
    std::optional<ParsedFrame> awaitReply(bool force = false);

    void sendAck();

    uint32_t timeoutMs_;
    uint32_t maxRetries_;

    // Bytes read from UART but not yet consumed by a parsed frame.
    std::vector<uint8_t> rxBuf_;
};
