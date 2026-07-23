#include "PacketBuilder.h"
#include <string.h>

uint16_t PacketBuilder::build(
    uint8_t*        outBuf,
    uint16_t        outBufSize,
    PacketMode      mode,
    uint8_t         src,
    uint8_t         dst,
    uint8_t         hopCount,
    uint32_t        seq,
    const uint8_t*  payload,
    uint8_t         payloadLen,
    SecurityLayer*  sec,
    const FragInfo* frag,
    bool            ackRequested)
{
    bool     doEncrypt = (sec != nullptr);
    bool     doFrag    = (frag != nullptr);
    uint8_t  hdrLen    = doFrag ? PACKET_HEADER_SIZE_FRAG : PACKET_HEADER_SIZE;
    uint16_t needed    = hdrLen + payloadLen + (doEncrypt ? CRYPTO_TAG_SIZE : 0);
    if (outBufSize < needed) return 0;

    PacketHeader hdr = {};
    hdr.setMode(static_cast<uint8_t>(mode));
    hdr.setHopCount(hopCount);
    hdr.setEncrypted(doEncrypt);
    hdr.setIsAck(false);
    hdr.setFrag(doFrag);
    hdr.setAckReq(ackRequested);
    hdr.setDst(dst);
    hdr.setSrc(src);

    memcpy(outBuf, hdr.raw, PACKET_HEADER_SIZE);
    headerSetSeq(outBuf, seq);

    if (doFrag) {
        setFragInfo(outBuf, frag->msgId, frag->idx, frag->total);
    }

    if (payloadLen > 0) {
        memcpy(outBuf + hdrLen, payload, payloadLen);
    }

    if (doEncrypt) {
        uint8_t tag[CRYPTO_TAG_SIZE];
        if (!sec->encryptPacket(outBuf, outBuf + hdrLen, payloadLen, tag))
            return 0;
        memcpy(outBuf + hdrLen + payloadLen, tag, CRYPTO_TAG_SIZE);
    }

    return needed;
}

uint16_t PacketBuilder::buildAck(
    uint8_t*       outBuf,
    uint16_t       outBufSize,
    PacketMode     mode,
    uint8_t        src,
    uint8_t        dst,
    uint32_t       echoSeq,
    uint8_t        hopCount,
    SecurityLayer* sec)
{
    const bool     doEncrypt = (sec != nullptr);
    const uint8_t  encPlain  = doEncrypt ? ACK_ENC_PLAINTEXT_SIZE : 0;
    const uint16_t needed    = PACKET_HEADER_SIZE + encPlain
                             + (doEncrypt ? CRYPTO_TAG_SIZE : 0);
    if (outBufSize < needed) return 0;

    PacketHeader hdr = {};
    hdr.setMode(static_cast<uint8_t>(mode));
    hdr.setIsAck(true);
    hdr.setHopCount(hopCount);
    hdr.setEncrypted(doEncrypt);
    hdr.setFrag(false);
    hdr.setDst(dst);
    hdr.setSrc(src);
    headerSetSeq(hdr, echoSeq);
    memcpy(outBuf, hdr.raw, PACKET_HEADER_SIZE);

    if (doEncrypt) {
        outBuf[PACKET_HEADER_SIZE] = ACK_ENC_PLAINTEXT_BYTE;

        uint8_t tag[CRYPTO_TAG_SIZE];
        if (!sec->encryptPacket(outBuf,
                                outBuf + PACKET_HEADER_SIZE,
                                ACK_ENC_PLAINTEXT_SIZE,
                                tag)) return 0;
        memcpy(outBuf + PACKET_HEADER_SIZE + ACK_ENC_PLAINTEXT_SIZE,
               tag, CRYPTO_TAG_SIZE);
    }

    return needed;
}

void PacketBuilder::decrementHop(uint8_t* frame) {
    headerDecrementHop(frame);
}
