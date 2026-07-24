#include "LoRaCom.h"

// TYPE[1] | SENDER_ID[1] | LENGTH[4, little-endian] | PAYLOAD[LENGTH] | CHECKSUM[2, little-endian]
std::vector<uint8_t> LoRaCom::buildFrame(TransmissionType type, uint8_t id, const std::string& payload)
{
    std::vector<uint8_t> crcInput;
    crcInput.reserve(2 + payload.size());
    crcInput.push_back(static_cast<uint8_t>(type));
    crcInput.push_back(id);
    crcInput.insert(crcInput.end(), payload.begin(), payload.end());
    uint16_t checksum = getCRC(crcInput);

    std::vector<uint8_t> frame;
    frame.reserve(6 + payload.size() + 2);
    frame.push_back(static_cast<uint8_t>(type));
    frame.push_back(id);
    frame.push_back(payload.size() & 0xFF);
    frame.push_back((payload.size() >> 8) & 0xFF);
    frame.push_back((payload.size() >> 16) & 0xFF);
    frame.push_back((payload.size() >> 24) & 0xFF);
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(checksum & 0xFF);
    frame.push_back((checksum >> 8) & 0xFF);
    return frame;
}

std::optional<LoRaCom::ParsedFrame> LoRaCom::readFrame()
{
    // TODO: BasicUart::read() has no timeout parameter yet, so ACK_TIMEOUT_MS
    // is not actually enforced here - this blocks until something arrives.
    std::vector<uint8_t> raw = this->read();

    if (raw.size() < 8) {
        return std::nullopt;
    }

    uint32_t length = raw[2] | (raw[3] << 8) | (raw[4] << 16) | (raw[5] << 24);
    if (raw.size() != 6 + length + 2) {
        return std::nullopt;
    }

    uint16_t receivedChecksum = raw[6 + length] | (raw[6 + length + 1] << 8);

    ParsedFrame frame;
    frame.type = static_cast<TransmissionType>(raw[0]);
    frame.senderId = raw[1];
    frame.payload = std::string(raw.begin() + 6, raw.begin() + 6 + length);
    frame.checksumValid = verifyChecksum(frame, receivedChecksum);
    return frame;
}

bool LoRaCom::sendAck()
{
    return this->write(buildFrame(TransmissionType::ACK, 0, ""));
}

uint16_t LoRaCom::getCRC(const std::vector<uint8_t>& data)
{
    // TODO: implement CRC16 over `data`.
    return 0;
}

bool LoRaCom::verifyChecksum(const ParsedFrame& frame, uint16_t receivedChecksum)
{
    // TODO: verify receivedChecksum against a CRC16 over TYPE, SENDER_ID and PAYLOAD.
    return true;
}

bool LoRaCom::sendTransmission(TransmissionType type, uint8_t destId, const std::string& payload)
{
    std::vector<uint8_t> frame = buildFrame(type, destId, payload);

    for (uint32_t retry = 0; retry < MAX_RETRIES; ++retry) {
        if (!this->write(frame)) {
            return false;
        }

        std::optional<ParsedFrame> reply = readFrame();
        if (reply && reply->checksumValid && reply->type == TransmissionType::ACK) {
            return true;
        }
        // timeout, corrupted frame, or unexpected reply -> retransmit
    }
    return false;
}

std::optional<Transmission> LoRaCom::getTransmission(TransmissionType type)
{
    std::vector<uint8_t> request = buildFrame(type, 0, "");

    for (uint32_t retry = 0; retry < MAX_RETRIES; ++retry) {
        if (!this->write(request)) {
            return std::nullopt;
        }

        std::optional<ParsedFrame> reply = readFrame();
        if (!reply || !reply->checksumValid) {
            continue; // timeout or corrupted frame -> retransmit request
        }
        if (reply->type == TransmissionType::ACK) {
            return std::nullopt; // nothing available
        }
        if (reply->type != type) {
            continue; // unexpected reply -> retransmit request
        }

        sendAck();
        return Transmission{reply->senderId, reply->payload};
    }
    return std::nullopt;
}
