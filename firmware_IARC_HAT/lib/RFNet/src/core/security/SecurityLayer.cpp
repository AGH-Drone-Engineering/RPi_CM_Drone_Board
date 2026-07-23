#include "SecurityLayer.h"
#include "../../port/Logger.h"
#include <string.h>

bool SecurityLayer::encryptPacket(uint8_t* header,
                                   uint8_t* payload, size_t payloadLen,
                                   uint8_t  tag[CRYPTO_TAG_SIZE]) {
    uint8_t nonce[CRYPTO_NONCE_SIZE];
    buildNonce(nonce, header);

    uint8_t hdrLen = packetHeaderSize(header);
    uint8_t aad[PACKET_HEADER_SIZE_FRAG];
    buildAad(aad, header, hdrLen);

    bool ok = _crypto.encrypt(aad, hdrLen, payload, payloadLen, nonce, tag);
    if (!ok) LOG_E("Security", "encrypt failed len=%u", (unsigned)payloadLen);
    return ok;
}

bool SecurityLayer::decryptPacket(const uint8_t* header,
                                   uint8_t*       payload, size_t payloadLen,
                                   const uint8_t  tag[CRYPTO_TAG_SIZE]) {
    uint8_t nonce[CRYPTO_NONCE_SIZE];
    buildNonce(nonce, header);

    uint8_t hdrLen = packetHeaderSize(header);
    uint8_t aad[PACKET_HEADER_SIZE_FRAG];
    buildAad(aad, header, hdrLen);

    bool ok = _crypto.decrypt(aad, hdrLen, payload, payloadLen, nonce, tag);
    if (!ok) LOG_W("Security", "decrypt auth fail len=%u", (unsigned)payloadLen);
    return ok;
}
