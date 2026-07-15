#include "UartManager.h"

bool LoRaCom::sendTransmission(TransmissionType type, uint8_t senderId, const std::string& payload)
{
    uint8_t ackBuffer[2];

    // TYPE[1] | SENDER_ID[1] | LENGTH[4] | PAYLOAD[LEN] | CHECKSUM[2]
    for (int retryCount = 0; retryCount < MAX_RETRIES; ++retryCount) {
        std::vector<uint8_t> data;
        uint16_t checksum = 0; // TODO
        data.reserve(sizeof(TransmissionHeader) + payload.size() + sizeof(uint16_t));

        data.push_back(TransmissionType::SENDMSG);
        data.push_back(transmission.senderId);
        data.push_back((payload.size() >> 24) & 0xFF);
        data.push_back((payload.size() >> 16) & 0xFF);
        data.push_back((payload.size() >> 8) & 0xFF);
        data.push_back(payload.size() & 0xFF);
        data.insert(data.end(), payload.begin(), payload.end());
        data.push_back((checksum >> 8) & 0xFF);
        data.push_back(checksum & 0xFF);

        if (!this->write(data)) {
            return false;
        }

        // Wait for ACK
        this->read(ackBuffer, 2);
    }
    return true;
}

std::pair<uint8_t, std::vector<uint8_t>> LoRaCom::getTransmission(TransmissionType type)
{
    this->write({static_cast<uint8_t>(type)});

    uint8_t header[6];
    this->read(header, 6);
    TransmissionType receivedType = static_cast<TransmissionType>(header[0]);
    uint8_t senderId = header[1];
    uint32_t length = (header[2] << 24) | (header[3] << 16) | (header[4] << 8) | header[5];

    if (receivedType != type) {
        throw std::runtime_error("Received unexpected transmission type");
    }

    std::vector<uint8_t> payload(length);
    this->read(payload.data(), length);

    this->send({static_cast<uint8_t>(TransmissionType::ACK)});

    return {senderId, payload};
}