#pragma once
#include "IRoutingStrategy.h"
#include <stdint.h>

// Floods every unseen packet; optional RSSI-based suppression saves airtime
// on strongly-heard hops.
// - Default: plain flooding (suppressIfRssiAbove = INT16_MAX).
// - E.g. -60 dBm suppresses close-by repeats.
// Suppression is one-shot per frame: DATA deduped by ReplayWindow (src, seq);
// ACK deduped by SeenCache (src, dst, seq, ack-bit).
// Tune threshold against real topology.

struct ManagedFloodingConfig {
    int16_t suppressIfRssiAbove = INT16_MAX;
};

class ManagedFloodingStrategy final : public IRoutingStrategy {
public:
    ManagedFloodingStrategy() = default;
    explicit ManagedFloodingStrategy(const ManagedFloodingConfig& cfg) : _cfg(cfg) {}

    bool shouldForward(const PacketHeader&,
                        const RoutingContext& ctx) const override {
        // Unknown RSSI always forwards.
        if (ctx.rssi != RF_RSSI_UNKNOWN && ctx.rssi > _cfg.suppressIfRssiAbove)
            return false;
        return true;
    }

private:
    ManagedFloodingConfig _cfg{};
};
