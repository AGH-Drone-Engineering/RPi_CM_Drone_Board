#include "../RFNode.h"
#include "Engine.h"
#include "../core/security/SecurityLayer.h"
#include "../core/security/ICypher.h"
#include "../core/security/pbkdf2/Pbkdf2.h"
#include "../port/crypto/DefaultCipher.h"
#include <string.h>
#include "../port/Logger.h"

namespace
{
    inline void secure_zero(void *p, size_t n)
    {
        volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
        while (n--)
            *v++ = 0;
    }
}

RFNode::RFNode(IRadio &radio, const RFNodeConfig &cfg)
    : _osal(_defaultOsal), _radio(radio), _cfg(cfg) {}

RFNode::RFNode(IRadio &radio, IOsal &osal, const RFNodeConfig &cfg)
    : _osal(osal), _radio(radio), _cfg(cfg) {}

RFNode::~RFNode() { end(); }

BeginStatus RFNode::begin()
{
    if (_engine)
        return BeginStatus::ALREADY_STARTED;

    const auto &sec = _cfg.security;

    switch (sec.source)
    {
    case RFSecurityConfig::Source::None:
        break;

    case RFSecurityConfig::Source::Cipher:
        if (!sec.cipher) {
            LOG_E("RFNode", "begin: Source::Cipher but cipher pointer is null");
            return BeginStatus::INVALID_CONFIG;
        }
        _cipher = sec.cipher;
        _ownsCipher = false;
        _sec = new SecurityLayer(*_cipher);
        break;

    case RFSecurityConfig::Source::Key:
    case RFSecurityConfig::Source::Password:
    {
        if (sec.source == RFSecurityConfig::Source::Key && !sec.key) {
            LOG_E("RFNode", "begin: Source::Key but key pointer is null");
            return BeginStatus::INVALID_CONFIG;
        }
        if (sec.source == RFSecurityConfig::Source::Password &&
            (!sec.password || sec.kdfIterations == 0)) {
            LOG_E("RFNode", "begin: Source::Password but password is null or kdfIterations=0");
            return BeginStatus::INVALID_CONFIG;
        }

        _cipher = new DefaultCipher();
        _ownsCipher = true;

        if (sec.source == RFSecurityConfig::Source::Key)
        {
            memcpy(_key, *sec.key, CRYPTO_KEY_SIZE);
        }
        else
        {
            static const uint8_t kSalt[] = RF_KDF_SALT;
            static_assert(sizeof(kSalt) > 1, "RF_KDF_SALT must be a non-empty string literal");
            pbkdf2_sha256(sec.password, strlen(sec.password),
                          kSalt, sizeof(kSalt) - 1,
                          sec.kdfIterations, _key, CRYPTO_KEY_SIZE);
        }

        const bool ok = _cipher->setKey(_key);
        secure_zero(_key, CRYPTO_KEY_SIZE);
        if (!ok)
        {
            delete _cipher;
            _cipher = nullptr;
            _ownsCipher = false;
            LOG_E("RFNode", "begin: cipher setKey() failed");
            return BeginStatus::SECURITY_INIT_FAILED;
        }
        _sec = new SecurityLayer(*_cipher);
        break;
    }
    }

    EngineConfig rcfg;
    rcfg.myAddr                    = _cfg.addr;
    rcfg.mode                      = _cfg.mode;
    rcfg.clock                     = _cfg.clock;
    rcfg.requireEncrypted          = sec.requireEncrypted;
    rcfg.sec                       = _sec;
    rcfg.nv                        = _cfg.nv;
    rcfg.hopCount                  = _cfg.mesh.hopCount;
    // Mesh with no explicit strategy defaults to managed flooding; P2P leaves
    // nullptr since Engine never consults routing there.
    rcfg.routing                   = _cfg.mesh.routing;
    if (_cfg.mode == PacketMode::Mesh && !rcfg.routing)
        rcfg.routing = &_defaultFlooding;
    rcfg.forwardJitter             = _cfg.mesh.forwardJitter;
    rcfg.dutyCycleEnabled          = _cfg.dutyCycle.enabled;
    rcfg.dutyCycleDenominatorOverride = _cfg.dutyCycle.denominatorOverride;
    rcfg.ackTimeoutMs              = _cfg.reliability.ackTimeoutMs;
    rcfg.ackTimeoutMarginP2Pms     = _cfg.reliability.ackTimeoutMarginP2Pms;
    rcfg.ackTimeoutMarginMeshMs    = _cfg.reliability.ackTimeoutMarginMeshMs;
    rcfg.fragRetryMax              = _cfg.reliability.fragRetryMax;
    rcfg.ackRetryMax               = _cfg.reliability.ackRetryMax;

    _engine = new Engine(_radio, _osal, rcfg);
    _setupCallbacks();
    BeginStatus st = _engine->begin();
    if (st != BeginStatus::OK)
        LOG_E("RFNode", "begin failed status=%u", (unsigned)st);
    return st;
}

void RFNode::end()
{
    if (_engine)
    {
        if (!_engine->end())
        {
            // May still touch _sec/_cipher, so they must stay alive; leaked
            // until a later end()/destructor call completes teardown.
            LOG_E("RFNode", "end: engine worker still running; keeping engine/security alive to avoid UAF");
            return;
        }
        delete _engine;
        _engine = nullptr;
    }
    if (_sec)
    {
        delete _sec;
        _sec = nullptr;
    }
    if (_ownsCipher && _cipher)
    {
        delete _cipher;
        _cipher = nullptr;
    }
    _ownsCipher = false;
}

void RFNode::onReceive (RecvCb       cb, void* ctx) { _recvCb    = cb; _recvCtx    = ctx; }
void RFNode::onSendOk  (OnSendOkCb  cb, void* ctx) { _cbSendOk  = cb; _ctxSendOk  = ctx; }
void RFNode::onSendFail(OnSendFailCb cb, void* ctx) { _cbSendFail = cb; _ctxSendFail = ctx; }

void RFNode::_setupCallbacks()
{
    _engine->onReceive([](const RxInfo& info, const RFMessage* m, void* ctx) {
        auto* self = static_cast<RFNode*>(ctx);
        if (!self->_recvCb) return;
        const uint8_t* p    = m->_largePayload ? m->_largePayload : m->payload;
        size_t         plen = m->_largePayload ? m->_largePayloadLen : (size_t)m->payloadLen;
        self->_recvCb(info, p, plen, self->_recvCtx);
    }, this);

    _engine->onSendSuccess([](const RFMessage* m, void* ctx) {
        auto* self = static_cast<RFNode*>(ctx);
        if (!self->_cbSendOk) return;
        uint32_t resolved = m->waitAck ? m->ackTimeoutMs : 0;
        const uint8_t* p    = m->_largePayload ? m->_largePayload : m->payload;
        size_t         plen = m->_largePayload ? m->_largePayloadLen : (size_t)m->payloadLen;
        SentInfo si{ m->peer, m->_seq, p, plen, resolved, 0 };
        self->_cbSendOk(si, self->_ctxSendOk);
    }, this);

    _engine->onSendFailed([](const RFMessage* m, TxFailReason reason, void* ctx) {
        auto* self = static_cast<RFNode*>(ctx);
        if (!self->_cbSendFail) return;
        uint32_t resolved = m->waitAck ? m->ackTimeoutMs : 0;
        const uint8_t* p    = m->_largePayload ? m->_largePayload : m->payload;
        size_t         plen = m->_largePayload ? m->_largePayloadLen : (size_t)m->payloadLen;
        SentInfo si{ m->peer, m->_seq, p, plen, resolved, m->_dutyWaitMs };
        self->_cbSendFail(si, reason, self->_ctxSendFail);
    }, this);
}

SendStatus RFNode::_sendImpl(uint8_t dst, const void* data, size_t len, const SendOptions& opts)
{
    if (!_engine) return SendStatus::NOT_INITIALIZED;
    if (len == 0 || len > RF_MAX_FRAGMENTED_PAYLOAD) return SendStatus::BAD_LENGTH;

    if (len > RF_MAX_PAYLOAD) {
        uint8_t hops = (opts.hops == RF_HOPS_USE_NODE_DEFAULT) ? _cfg.mesh.hopCount : opts.hops;
        if (hops > 7) {
            LOG_W("RFNode", "send: opts.hops=%u exceeds 3-bit max, clamping to 7",
                  (unsigned)hops);
            hops = 7;
        }
        bool requireAck = opts.requireAck && (dst != ADDR_BROADCAST);
        return _engine->startLargeSend(dst, static_cast<const uint8_t*>(data), len,
                                       requireAck, hops, opts.ackTimeoutMs);
    }

    RFMessage* msg = _engine->acquire();
    if (!msg) return SendStatus::POOL_EXHAUSTED;

    uint8_t hops = (opts.hops == RF_HOPS_USE_NODE_DEFAULT) ? _cfg.mesh.hopCount : opts.hops;
    if (hops > 7) {
        LOG_W("RFNode", "send: opts.hops=%u exceeds 3-bit max, clamping to 7", (unsigned)hops);
        hops = 7;
    }
    msg->peer         = dst;
    msg->payloadLen   = static_cast<uint8_t>(len);
    msg->waitAck      = opts.requireAck && (dst != ADDR_BROADCAST);
    msg->ackTimeoutMs = opts.ackTimeoutMs;
    msg->hopCount     = hops;
    memcpy(msg->payload, data, len);
    return _engine->send(msg);
}

SendStatus RFNode::send(uint8_t dst, const void* data, size_t len, const SendOptions& opts)
{
    // Just one of several public overloads — each calls _sendImpl() directly,
    // not through each other. Cross-cutting logic belongs in _sendImpl().
    return _sendImpl(dst, data, len, opts);
}

SendStatus RFNode::send(uint8_t dst, const void* data, size_t len)
{
    SendOptions opts;
    return _sendImpl(dst, data, len, opts);
}

SendStatus RFNode::sendAck(uint8_t dst, const void* data, size_t len)
{
    SendOptions opts;
    opts.requireAck = true;           // engine downgrades to no-ACK for broadcast dst
    return _sendImpl(dst, data, len, opts);
}

SendStatus RFNode::sendBroadcast(const void* data, size_t len)
{
    SendOptions opts;                 // ACK to 0xFF is meaningless — leave requireAck=false
    return _sendImpl(ADDR_BROADCAST, data, len, opts);
}

bool RFNode::startWorkerTask(uint32_t stackBytes, uint8_t priority)
{
    if (!_engine)
        return false;
    return _engine->startWorkerTask(stackBytes, priority);
}

void RFNode::poll()
{
    if (_engine)
        _engine->poll();
}

uint32_t RFNode::getDutyCycleWaitMs(size_t totalLen) const
{
    if (_engine)
        return _engine->getDutyCycleWaitMs(totalLen);
    return 0;
}
