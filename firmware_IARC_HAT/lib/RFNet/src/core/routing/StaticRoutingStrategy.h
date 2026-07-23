#pragma once
#include "IRoutingStrategy.h"
#include <stdint.h>

// Routing table for known-topology mesh networks. Each entry maps an
// end-to-end destination to its L2 next-hop neighbour; an entry's presence
// is the forward decision (no separate boolean).
// Lookup miss: `fallback` picks FloodBroadcast (degrade to flooding) or
// Drop (refuse to forward/originate via that destination).

class StaticRoutingStrategy final : public IRoutingStrategy {
public:
    struct Entry {
        uint8_t dst;       // L3 end-to-end destination
        uint8_t nextHop;   // L2 next-hop neighbour to reach `dst`
    };

    enum class Fallback : uint8_t { FloodBroadcast, Drop };

    StaticRoutingStrategy(const Entry* routes, uint8_t count,
                          Fallback fallback = Fallback::FloodBroadcast)
        : _routes(routes), _count(count), _fallback(fallback) {}

    bool shouldForward(const PacketHeader& hdr,
                        const RoutingContext& ctx) const override {
        if (hdr.dst() == ADDR_BROADCAST) return true;
        // Defensive: PacketParser::shouldForward already guarantees dst != myAddr.
        if (hdr.dst() == ctx.myAddr) return false;
        return _lookup(hdr.dst()) != nullptr
            || _fallback == Fallback::FloodBroadcast;
    }

    // ctx is unused: a table lookup doesn't need RSSI.
    uint8_t nextHop(uint8_t appDst, const RoutingContext&) const override {
        if (appDst == ADDR_BROADCAST) return ADDR_BROADCAST;
        if (const Entry* e = _lookup(appDst)) return e->nextHop;
        // No route: FloodBroadcast returns ADDR_BROADCAST; Drop returns appDst instead of guessing.
        return _fallback == Fallback::FloodBroadcast ? ADDR_BROADCAST : appDst;
    }

private:
    const Entry* _lookup(uint8_t dst) const {
        for (uint8_t i = 0; i < _count; ++i) {
            if (_routes[i].dst == dst) return &_routes[i];
        }
        return nullptr;
    }

    const Entry* _routes;
    uint8_t      _count;
    Fallback     _fallback;
};
