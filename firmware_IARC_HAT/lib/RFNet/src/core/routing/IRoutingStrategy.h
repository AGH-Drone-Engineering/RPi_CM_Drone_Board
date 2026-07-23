#pragma once
#include <stdint.h>
#include "../packet/Packet.h"

// Routing interface: decides whether a mesh node should forward a given packet.
// Called only after PacketParser::shouldForward verifies frame preconditions
// (mode==Mesh, hop_count>0, dst!=myAddr) — implementations need not repeat these checks.

// Sentinel for "RSSI not provided by backend". Mirrors the definition in
// port/hal/radio/IRadio.h (both headers must stay includable on their own).
#ifndef RF_RSSI_UNKNOWN
#define RF_RSSI_UNKNOWN INT16_MIN
#endif

// Per-RX context handed to the routing strategy.
// Extend with more metadata (SNR, timestamp, hop history) as new strategies
// need it — keeps the interface stable.
struct RoutingContext {
    uint8_t myAddr;
    int16_t rssi;  // dBm, RF_RSSI_UNKNOWN when the backend didn't report it

    RoutingContext() : myAddr(0), rssi(RF_RSSI_UNKNOWN) {}
    RoutingContext(uint8_t addr, int16_t r) : myAddr(addr), rssi(r) {}
};

class IRoutingStrategy {
public:
    virtual ~IRoutingStrategy() = default;
    virtual bool shouldForward(const PacketHeader& hdr,
                                const RoutingContext& ctx) const = 0;

    // L2 next-hop address for an application-layer destination `appDst`.
    // Called for originated TX, ACK reply, and mesh forwards — must not depend on
    // per-frame state beyond `ctx`.
    // `ctx.rssi` = RSSI of the most recently RECEIVED frame:
    // - ACK reply / mesh forward: the inbound frame that triggered this call.
    // - Originated TX: stale value from whatever frame was heard last
    //   (RF_RSSI_UNKNOWN until the first RX). Do not base next-hop decisions
    //   for originated sends on it.
    virtual uint8_t nextHop(uint8_t appDst, const RoutingContext& ctx) const {
        (void)appDst; (void)ctx;
        return ADDR_BROADCAST;
    }
};
