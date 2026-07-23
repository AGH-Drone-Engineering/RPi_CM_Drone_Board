#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ICypher.h"
#include "../packet/Packet.h"

// Packet-level crypto adapter: builds the GCM nonce/AAD from the packet header
// (see buildNonce/buildAad in Packet.h).
// - Cipher must be keyed before construction.
// - Caller must fill the header before encryptPacket: nonce binds src+seq+ack+dst
//   and must be unique per key — any repeat for the same key breaks GCM's
//   security guarantees.
class SecurityLayer {
    ICypher& _crypto;

public:
    explicit SecurityLayer(ICypher& backend) : _crypto(backend) {}

    bool encryptPacket(uint8_t* header,
                       uint8_t* payload, size_t payloadLen,
                       uint8_t  tag[CRYPTO_TAG_SIZE]);

    bool decryptPacket(const uint8_t* header,
                       uint8_t*       payload, size_t payloadLen,
                       const uint8_t  tag[CRYPTO_TAG_SIZE]);
};
