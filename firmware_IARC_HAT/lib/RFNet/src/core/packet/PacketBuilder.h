#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Packet.h"
#include "../security/SecurityLayer.h"

class PacketBuilder {
public:
    // Returns frame length, or 0 on failure. frag != nullptr sets the FRAG
    // bit and writes two extra header bytes; payload then starts at offset 8.
    // ackRequested sets the ACK_REQ bit (authenticated, so it can't be
    // stripped in flight); the receiver ACKs only when it's set.
    static uint16_t build(
        uint8_t*        outBuf,
        uint16_t        outBufSize,
        PacketMode      mode,
        uint8_t         src,
        uint8_t         dst,
        uint8_t         hopCount,
        uint32_t        seq,
        const uint8_t*  payload,
        uint8_t         payloadLen,
        SecurityLayer*  sec          = nullptr,
        const FragInfo* frag         = nullptr,
        bool            ackRequested = false
    );

    // Returns frame length:
    //   plaintext ACK (sec == nullptr): PACKET_HEADER_SIZE
    //   encrypted ACK (sec != nullptr): PACKET_HEADER_SIZE
    //                                   + ACK_ENC_PLAINTEXT_SIZE
    //                                   + CRYPTO_TAG_SIZE
    // ACKs are never fragmented (FRAG bit always 0).
    // Returns 0 on failure.
    static uint16_t buildAck(
        uint8_t*       outBuf,
        uint16_t       outBufSize,
        PacketMode     mode,
        uint8_t        src,
        uint8_t        dst,
        uint32_t       echoSeq,
        uint8_t        hopCount = 0,
        SecurityLayer* sec      = nullptr
    );

    static void decrementHop(uint8_t* frame);
};
