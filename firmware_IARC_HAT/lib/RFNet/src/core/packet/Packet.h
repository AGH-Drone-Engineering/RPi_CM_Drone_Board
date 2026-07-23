#pragma once
#include <stdint.h>
#include <string.h>
#include "../security/ICypher.h"

constexpr uint8_t PACKET_HEADER_SIZE      = 6;
constexpr uint8_t PACKET_HEADER_SIZE_FRAG = 8;
constexpr uint8_t ADDR_BROADCAST          = 0xFF;

// Encrypted ACKs carry this single plaintext byte so the AEAD never sees a
// zero-length plaintext (some HW AES backends — STM32 CRYP — reject that).
// The value also serves as an in-band ACK format version for future use.
constexpr uint8_t ACK_ENC_PLAINTEXT_SIZE = 1;
constexpr uint8_t ACK_ENC_PLAINTEXT_BYTE = 0x01;

enum class PacketMode : uint8_t {
    P2P  = 0,
    Mesh = 1,
};

// ── WIRE VERSIONING ────────────────────────────────────────────────────────
// No explicit protocol-version field — all 8 bits of header byte 0 are used.
// Implicitly versioned via the security binding (buildNonce/buildAad): a
// peer on a different wire revision fails authentication rather than
// parsing garbage.
//
// Wire layout (byte 0 bit map, LSB→MSB):
//   bits 0-2 : hop_count
//   bit  3   : encrypted
//   bit  4   : mode (0=P2P, 1=Mesh)
//   bit  5   : ack (this frame IS an ACK)
//   bit  6   : FRAG (0=single frame, 1=fragmented — two extra header bytes follow)
//   bit  7   : ACK_REQ (DATA frames only: sender awaits an ACK — receiver
//              replies only when set, saving an ACK frame + duty budget on
//              every fire-and-forget unicast). Always 0 on ACK frames.
// byte 1: dst, byte 2: src, bytes 3-5: seq (big-endian 24-bit).
// When FRAG=1: byte 6: msgId, byte 7: (idx<<4)|(total-1).
//
// Manual bit accessors (not bitfields): keeps layout portable across
// compilers/endianness.
struct PacketHeader {
    uint8_t raw[PACKET_HEADER_SIZE];

    uint8_t hopCount()  const { return raw[0] & 0x07u; }
    bool    encrypted() const { return (raw[0] >> 3) & 0x01u; }
    uint8_t mode()      const { return (raw[0] >> 4) & 0x01u; }
    bool    isAck()     const { return (raw[0] >> 5) & 0x01u; }
    bool    frag()      const { return (raw[0] >> 6) & 0x01u; }
    bool    ackReq()    const { return (raw[0] >> 7) & 0x01u; }
    uint8_t dst()       const { return raw[1]; }
    uint8_t src()       const { return raw[2]; }

    void setHopCount(uint8_t v) { raw[0] = (raw[0] & ~0x07u) | (v & 0x07u); }
    void setEncrypted(bool v)   { raw[0] = (raw[0] & ~0x08u) | (v ? 0x08u : 0u); }
    void setMode(uint8_t v)     { raw[0] = (raw[0] & ~0x10u) | ((v & 0x01u) << 4); }
    void setIsAck(bool v)       { raw[0] = (raw[0] & ~0x20u) | (v ? 0x20u : 0u); }
    void setFrag(bool v)        { raw[0] = (raw[0] & ~0x40u) | (v ? 0x40u : 0u); }
    void setAckReq(bool v)      { raw[0] = (raw[0] & ~0x80u) | (v ? 0x80u : 0u); }
    void setDst(uint8_t v)      { raw[1] = v; }
    void setSrc(uint8_t v)      { raw[2] = v; }
};
static_assert(sizeof(PacketHeader) == PACKET_HEADER_SIZE,
              "PacketHeader must be exactly 6 bytes");

// Hard wire-format limit, not a tunable knob: idx/total-1 are 4-bit nibbles
// in byte[7] (see setFragInfo/fragTotal), so total tops out at 16. The
// static_assert below fails the build if this is ever raised past that.
constexpr uint8_t RF_MAX_FRAGMENTS = 16;
static_assert(RF_MAX_FRAGMENTS >= 1 && RF_MAX_FRAGMENTS <= 16,
              "frag idx/total are 4-bit fields in byte[7] — at most 16 fragments");

// Fragment metadata passed to PacketBuilder::build.
struct FragInfo {
    uint8_t msgId;
    uint8_t idx;    // 0..15
    uint8_t total;  // 1..16
};

inline uint32_t headerGetSeq(const uint8_t* h) {
    return ((uint32_t)h[3] << 16) | ((uint32_t)h[4] << 8) | h[5];
}
inline void headerSetSeq(uint8_t* h, uint32_t seq) {
    h[3] = (seq >> 16) & 0xFF;
    h[4] = (seq >>  8) & 0xFF;
    h[5] =  seq        & 0xFF;
}
inline void headerDecrementHop(uint8_t* h) {
    if ((h[0] & 0x7) > 0) h[0]--;
}

inline uint32_t headerGetSeq(const PacketHeader& h) { return headerGetSeq(h.raw); }
inline void     headerSetSeq(PacketHeader& h, uint32_t seq) { headerSetSeq(h.raw, seq); }

inline uint8_t packetHeaderSize(const uint8_t* h) {
    return (h[0] & 0x40u) ? PACKET_HEADER_SIZE_FRAG : PACKET_HEADER_SIZE;
}

// Frag-info accessors on raw buffer — only valid when FRAG bit is set.
inline uint8_t fragMsgId(const uint8_t* h) { return h[6]; }
inline uint8_t fragIdx  (const uint8_t* h) { return h[7] >> 4; }
inline uint8_t fragTotal(const uint8_t* h) { return (h[7] & 0x0Fu) + 1u; }
inline void    setFragInfo(uint8_t* h, uint8_t msgId, uint8_t idx, uint8_t total) {
    h[6] = msgId;
    h[7] = (uint8_t)((idx << 4) | ((total - 1u) & 0x0Fu));
}

inline void buildNonce(uint8_t nonce[CRYPTO_NONCE_SIZE], const uint8_t* header) {
    nonce[0] = header[2];                       // src
    nonce[1] = header[3];                       // seq[0]
    nonce[2] = header[4];                       // seq[1]
    nonce[3] = header[5];                       // seq[2]
    nonce[4] = (header[0] >> 5) & 0x01;        // ack-bit domain separator
    // dst required: an ACK's seq echoes the *originator's* counter, so
    // without it two ACKs to different originators could reuse (key, nonce)
    // — a GCM violation leaking the key and enabling forgery. No-op for
    // DATA frames (src+seq already unique).
    nonce[5] = header[1];                       // dst
    memset(nonce + 6, 0, CRYPTO_NONCE_SIZE - 6);
}

// AAD covers the full header (6 or 8 bytes) so frag-info bytes are
// authenticated. Caller must pass packetHeaderSize(header) as hdrLen.
// hop_count bits are zeroed because they change during Mesh forwarding.
inline void buildAad(uint8_t* aad, const uint8_t* header, uint8_t hdrLen) {
    memcpy(aad, header, hdrLen);
    aad[0] &= ~0x07u;
}
