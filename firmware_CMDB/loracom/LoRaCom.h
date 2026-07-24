
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "BasicUart.h"

constexpr uint32_t DEFAULT_ACK_TIMEOUT_MS = 250;
constexpr uint32_t DEFAULT_MAX_RETRIES = 3;

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

    bool sendTransmission(TransmissionType type, uint8_t destId, const std::string& payload);
    std::optional<Transmission> getTransmission(TransmissionType type);

    // True if the last sendTransmission()/getTransmission() call gave up after
    // exhausting retries (as opposed to e.g. a legitimately empty message queue).
    bool lastCallFailed() const { return lastCallFailed_; }

private:
    struct ParsedFrame
    {
        TransmissionType type;
        uint8_t senderId;
        std::string payload;
        bool checksumValid;
    };

    // CRC16 over `data`. TODO: not implemented yet, always returns 0.
    uint16_t getCRC(const std::vector<uint8_t>& data);

    // Verify frame's checksum against receivedChecksum. TODO: not implemented yet, always returns true.
    bool verifyChecksum(const ParsedFrame& frame, uint16_t receivedChecksum);

    std::vector<uint8_t> buildFrame(TransmissionType type, uint8_t id, const std::string& payload);
    std::optional<ParsedFrame> readFrame(uint32_t timeoutMs);

    // Waits up to timeoutMs_ for a reply. If a frame arrives but its checksum is
    // bad, waits an additional 2*timeoutMs_ for the far end to notice we never
    // ACKed and retransmit on its own, instead of immediately resending our
    // request. Returns nullopt if nothing usable arrives either way.
    std::optional<ParsedFrame> awaitReply();

    bool sendAck();

    uint32_t timeoutMs_;
    uint32_t maxRetries_;
    bool lastCallFailed_ = false;
};
