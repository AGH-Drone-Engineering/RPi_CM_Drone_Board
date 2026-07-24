
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "BasicUart.h"

constexpr uint32_t ACK_TIMEOUT_MS = 250;
constexpr uint32_t MAX_RETRIES = 3;

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
    LoRaCom(const std::string& device, uint32_t baudrate)
        : BasicUart(device, baudrate) {};

    bool sendTransmission(TransmissionType type, uint8_t destId, const std::string& payload);
    std::optional<Transmission> getTransmission(TransmissionType type);

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
    std::optional<ParsedFrame> readFrame();
    bool sendAck();
};
