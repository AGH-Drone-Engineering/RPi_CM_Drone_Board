
#pragma once

#include <string>
#include <optional>

constexpr uint32_t ACK_TIMEOUT_MS = 250;
constexpr uint32_t MAX_RETRIES = 3;

class enum TransmissionType : uint8_t
{
    CONFREQ = 'C',
    SENDMSG = 'S',
    GETMSG = 'G',
    ACK = 'A',
};

// Actual implementation of LoRa communication using the BasicUart

class LoRaCom : public BasicUart
{
public:
    LoRaCom(const std::string& device, uint32_t baudrate)  
        : BasicUart(device, baudrate) {};

    std::optional<Transmission> getTransmission();
    void LoRaCom::sendTransmission(TransmissionType type, uint8_t senderId, const std::string& payload);
};
