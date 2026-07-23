#include "PacketParser.h"
#include "../RFConfig.h"
#include <string.h>

ParsedPacket PacketParser::parse(uint8_t* buf, uint16_t len, SecurityLayer* sec) {
    ParsedPacket result = {};
    result.valid = false;

    if (len < PACKET_HEADER_SIZE) return result;
    // Upper bound: fragmented frame header is 8 bytes, so use the larger cap.
    if (len > PACKET_HEADER_SIZE_FRAG + RF_MAX_PAYLOAD + CRYPTO_TAG_SIZE) return result;

    // Determine header length before copying — FRAG bit is in buf[0].
    uint8_t hdrLen = packetHeaderSize(buf);
    if (len < hdrLen) return result;

    memcpy(result.hdr.raw, buf, PACKET_HEADER_SIZE);

    if (result.hdr.frag()) {
        result.fragmented = true;
        result.fragMsgId  = fragMsgId(buf);
        result.fragIdx    = fragIdx(buf);
        result.fragTotal  = fragTotal(buf);
    }

    uint8_t* payloadPtr = buf + hdrLen;
    uint16_t payloadLen = static_cast<uint16_t>(len - hdrLen);

    if (result.hdr.encrypted()) {
        if (payloadLen < CRYPTO_TAG_SIZE) return result;
        payloadLen -= CRYPTO_TAG_SIZE;

        if (sec != nullptr) {
            const uint8_t* tag = payloadPtr + payloadLen;
            // SecurityLayer reads packetHeaderSize(buf) internally to build
            // the correct AAD — covering bytes 6-7 when FRAG=1.
            if (!sec->decryptPacket(buf, payloadPtr, payloadLen, tag))
                return result;
        }
    }

    result.payload    = payloadPtr;
    result.payloadLen = payloadLen;
    result.valid      = true;
    return result;
}

bool PacketParser::isForMe(const PacketHeader& hdr, uint8_t myAddr) {
    return hdr.dst() == myAddr || hdr.dst() == ADDR_BROADCAST;
}

bool PacketParser::shouldForward(const PacketHeader& hdr,
                                   const RoutingContext& ctx,
                                   const IRoutingStrategy& strategy) {
    if (static_cast<PacketMode>(hdr.mode()) != PacketMode::Mesh) return false;
    if (hdr.hopCount() == 0) return false;
    if (hdr.dst() != ADDR_BROADCAST && hdr.dst() == ctx.myAddr) return false;
    return strategy.shouldForward(hdr, ctx);
}
