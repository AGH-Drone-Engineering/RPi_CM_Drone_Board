# RFNet — pre-release deep review

Perform an exhaustive pre-release revision of the **RFNet** library at `lib/RFNet/`. This is a transport-agnostic wireless middleware (FSK/LoRa via RadioLib, and pluggable radios) providing: a custom wire frame, fragmentation/reassembly, EU duty-cycle compliance, AES-GCM encryption with NVS-persisted monotonic counters, replay protection, P2P + mesh with pluggable routing strategies, and a port layer (HAL/OSAL/crypto/NVS/clock) for Arduino/ESP32/STM32/Zephyr/POSIX/baremetal.

The library is considered feature-complete; this is the final revision before release. Find real defects, not style nits. Report language: **Polish** (keep identifiers/code in English).

## Ground rules

- Read the ENTIRE library before judging: `src/core/`, `src/engine/` (the heart — `Engine.cpp` is ~1300 lines), `src/port/`, `src/RFNode.h`, `src/RFTypes.h`, plus `examples/`, `library.json`, `library.properties`, `keywords.txt`.
- Be adversarial: for every claimed guarantee (comments claim GCM nonce uniqueness, replay safety, ETSI compliance, ISR safety), try to construct a concrete counterexample (inputs, packet sequence, timing, reboot) that breaks it.
- Every finding must cite `file:line`, state the failure scenario concretely (who sends what, what state, what goes wrong on air), and propose a fix. No hand-waving.
- Distinguish severity: **[BLOCKER]** (wire/security/compliance bug — must fix before release), **[MAJOR]** (correctness/robustness bug in realistic use), **[MINOR]** (edge case, portability, docs), **[NIT]**.
- If something is correct but subtle, don't report it as a bug — but DO flag it if the comment/doc contradicts the code.

## Review dimensions

### 1. Wire format & frame sanity (`core/packet/`)
- `Packet.h` bit layout: byte0 flags (hop_count 3b, encrypted, mode, ack, FRAG, ACK_REQ), dst/src, 24-bit seq, frag bytes (msgId, idx/total nibbles). Is the layout self-consistent everywhere (builder, parser, engine, nonce)? Any field read before validating FRAG/size?
- Sanity of the format itself: is 3-bit hop count enough for the mesh strategies shipped? 8-bit addresses + 0xFF broadcast — any collision with real usage? 24-bit seq rollover handling? Is there a protocol **version** field or at least a documented wire-compat story (comments mention wire-incompatible changes — how is mixed-version deployment detected/rejected)?
- `PacketParser`: fuzz it mentally — truncated frames, FRAG bit set with 6-byte frame, total=0 encodings, idx>total, oversized payload vs radio MTU. Every out-of-bounds read path.
- Header integrity: header bytes are outside the ciphertext — are they authenticated as AAD? If not, what can an attacker flip (dst, hop, ACK_REQ, FRAG bits) on an encrypted frame without detection, and what does the receiver then do?

### 2. Security (`core/security/`, `core/freshness/`, `core/nvs/`, `port/crypto/`)
- **Nonce construction** (`buildNonce` in Packet.h): prove or refute uniqueness under: same key on many nodes, ACK vs DATA domain separation (bit 5), broadcast frames, fragmented frames (do all fragments share seq or increment?), retransmissions, and **reboot** (does `MonotonicCounter` guarantee seq never repeats after power loss — reserve-ahead/flush semantics? What if NVS backend is Null?).
- Replay: `ReplayWindow` — window size vs 24-bit seq, out-of-order tolerance, wraparound, behavior on first packet from a node, interaction with mesh re-forwarding and `SeenCache`.
- AES-GCM backends (`AesGcmSoft`, `AesGcmMbedTls`, `AesGcmStm32Hal`): tag length, constant-time tag comparison, behavior parity (empty plaintext, empty AAD), key size handling. Is `AesGcmSoft` a real GCM (GHASH correctness) — spot-check against a known test vector.
- PBKDF2/SHA256 soft implementations: correct per RFC test vectors? Iteration count sane? Salt source?
- Unencrypted mode: what does the receiver do with `encrypted=0` frames when it has a key configured — downgrade attack possible?
- Key/nonce material zeroization, keys in logs.

### 3. Fragmentation (`core/fragment/`)
- `LargeTxSession` + `Reassembler` end-to-end: msgId is 8-bit — collision after 256 messages or across reboot; two senders with same src? (src is in the key?), interleaved messages from multiple peers, lost-fragment timeout and buffer reclamation, duplicate fragments, fragments with mismatching `total`, encrypted fragments (per-fragment auth? can an attacker mix fragments from two messages?).
- Memory: worst-case reassembly buffer usage, static vs heap, DoS by opening many sessions with garbage first-fragments.
- Interaction with ACK/retry: are fragments individually ACKed or the whole message? Retransmit path duty-counted?

### 4. Duty cycle / EU compliance (`core/duty/DutyCycleTracker.h`)
- Model vs ETSI EN 300 220-2 reality: per-sub-band budgets (0.1%/1%/10%), evaluation window semantics (sliding vs fixed hour), Tx time measured or estimated (time-on-air calc per modem — correct for LoRa SF/BW/CR and FSK?).
- Are ALL transmissions counted: ACKs, retransmissions, **mesh forwards**, fragments? Any path in `Engine.cpp` that calls radio TX bypassing the tracker?
- Clock: rollover of the millisecond clock, `NullClock` semantics, behavior when blocked (does send fail, queue, or silently drop — and does the API tell the caller?).

### 5. Mesh & routing (`core/routing/`, engine forwarding path)
- `ManagedFloodingStrategy`, `StaticRoutingStrategy`, `IRoutingStrategy` contract: dedup via `SeenCache` (size, eviction, seq-wrap), hop decrement ordering, broadcast storm bounds, forwarding of encrypted frames the forwarder can't decrypt, ACK routing back through the mesh (does an ACK find its way to the originator?), self-origin echo suppression.
- RSSI suppression example strategy: race between hearing a stronger relay and the suppression timer.
- P2P vs Mesh mode mixing on one channel.

### 6. Engine (`engine/Engine.cpp`, `Engine.h`, `RFNode.cpp`, `src/RFNode.h`)
- Full state-machine walk: send path (queue → duty check → build → encrypt → TX → await ACK → retry/backoff), receive path (parse → dedup/replay → decrypt → reassemble → deliver / forward / ACK).
- Concurrency: which methods are called from ISR vs task vs user context? Every shared structure (queues, sessions, duty tracker, replay window, seq counter) — locked consistently? Lock ordering, callbacks invoked while holding locks, poll-driven vs worker-task modes both correct?
- Timeout/retry math: ACK timeout vs time-on-air vs duty blocking; retry storms; what happens when TX fails mid-fragment-train.
- Buffer ownership: payload lifetimes across queueing/callback boundaries, MTU handling for both radio families.
- Error propagation: does every failure surface a distinct, documented error code to the API user, or are there silent drops?

### 7. Port layer & portability (`src/port/`)
- Interface contracts (`IRadio`, `IOsal`, `IClock`, `INVBackend`, `ICypher`): are the documented contracts sufficient for a third party to write a new port without reading the engine? Any engine assumption not captured in the interface (e.g. reentrancy, blocking, ISR context of radio callbacks)?
- Parity across implementations: FreeRTOS vs POSIX vs baremetal OSAL (queue semantics, timeout units), EEPROM vs Preferences vs Zephyr NVS (write endurance — how often does the monotonic counter write flash? wear estimate), Default* selection logic (compile-time guards correct per platform?).
- RadioLib adapters (SX1262 LoRa, CC1101 FSK, generic): IRQ handling, RX-after-TX re-arm, error code mapping, MTU constants vs actual modem limits.
- C++ standard/embedded hygiene: no exceptions/RTTI assumptions, heap allocations in hot paths, `-Wall`-cleanliness, static init order.

### 8. API, docs & release hygiene
- `RFNode` public API: naming consistency, default config sanity (`RFConfig.h` — are defaults legal in EU?), everything in `keywords.txt`?
- Examples (`examples/`): do all 16 compile against the current API? Do they demonstrate correct usage (especially duty_cycle, nvs_persistence, mesh ones)? An example teaching a wrong pattern is a bug.
- `library.json` / `library.properties`: version, deps (RadioLib version pin?), export filters.
- Comments: the codebase is heavily commented with guarantees — verify each strong claim ("wire-incompatible", "GCM violation", "no-op for safety") is actually true in code. Flag stale or contradictory comments.
- Missing-for-release checklist: README/quickstart, CHANGELOG, license header, any leftover TODO/FIXME/debug logging (`grep` for them).

## Process

1. First produce a short architecture map (data flow TX and RX, threading model) to prove understanding — if the code contradicts itself here, that's finding #1.
2. Then review dimension by dimension. Prefer depth over breadth: the engine, nonce/counter/replay chain, and duty accounting are the highest-risk areas.
3. Verify, don't pattern-match: before reporting, re-read the code path and try to refute your own finding. Only report findings that survive.
4. Final output:
   - **Verdict**: release-ready / release after blockers / needs another pass.
   - Findings table grouped by severity with `file:line`.
   - Detailed write-up per BLOCKER/MAJOR with failure scenario + concrete fix.
   - A short "release checklist" of mechanical items (version bump, docs, stale comments).

Do not modify any files — report only.
