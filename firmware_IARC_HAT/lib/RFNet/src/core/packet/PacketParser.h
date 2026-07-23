#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Packet.h"
#include "../routing/IRoutingStrategy.h"
#include "../security/SecurityLayer.h"

struct ParsedPacket {
    PacketHeader hdr;
    uint8_t*     payload;
    uint16_t     payloadLen;   // header + max payload + tag can exceed 255 bytes
    bool         valid;
    bool         fragmented;   // true when FRAG bit set
    uint8_t      fragMsgId;    // valid when fragmented
    uint8_t      fragIdx;
    uint8_t      fragTotal;
};

class PacketParser {
public:
    static ParsedPacket parse(
        uint8_t*       buf,
        uint16_t       len,
        SecurityLayer* sec = nullptr
    );

    static bool isForMe(const PacketHeader& hdr, uint8_t myAddr);

    // Unified forward decision for both data frames and ACKs.
    static bool shouldForward(const PacketHeader& hdr,
                               const RoutingContext& ctx,
                               const IRoutingStrategy& strategy);
};
