#pragma once
#include <stdint.h>
#include <stddef.h>
#include "packet/Packet.h"
#include "security/ICypher.h"
#include "../port/hal/radio/IRadio.h"

// Single source of truth for compile-time, user-tunable knobs.
// All overridable via -D at build time; invariants enforced by static_assert below.

// Per-frame payload limit (bytes); last fragment of a message may carry fewer.
// NETWORK INVARIANT: every node MUST use the same value — Reassembler validates
// non-last fragments against exactly this size; mismatch drops all fragments at receiver.
#ifndef RF_MAX_PAYLOAD
// Parenthesized: textually substituted into exprs like `idx * RF_MAX_PAYLOAD` —
// unparenthesized subtraction would escape the * / operators, producing garbage offsets.
// 255 B wire max (CC1101/SX126x) - 17 B overhead (1 L2 + 8 frag header + 8 AEAD tag).
#  define RF_MAX_PAYLOAD (255 - 17)
#endif

// Default for RFMeshConfig::hopCount — max intermediate nodes that may forward a message.
// Range 0-7 (3-bit wire field); 0 disables mesh forwarding.
// Default 3 bounds flood radius/air-time for typical deployments; 7 floods widest
// radius on every frame — raise only when needed.
#ifndef RF_DEFAULT_HOP_COUNT
#  define RF_DEFAULT_HOP_COUNT 3
#endif

// Max total message size for fragmented sends/reassembly.
// Protocol limit: RF_MAX_FRAGMENTS × RF_MAX_PAYLOAD. RF_MAX_FRAGMENTS is a hard,
// non-tunable ceiling (constexpr 16 in packet/Packet.h, bounded by the fragment-index
// wire field) — with default RF_MAX_PAYLOAD this knob caps at ~3808 B; exceeding it
// fails the static_assert below, not at runtime.
// RAM: each reassembly slot holds ceil(this / RF_MAX_PAYLOAD) × RF_MAX_PAYLOAD bytes
// (see Reassembler::REASM_MAX_TOTAL) × RF_REASM_SLOTS slots in Engine.
// Default: 13 × 238 × 4 ≈ 12.1 KB — lower both knobs on RAM-constrained targets
// (e.g. STM32 20 KB SRAM: 1024 / 2 slots ≈ 2.3 KB).
// NETWORK INVARIANT: receiver drops fragments of a message exceeding its own cap
// (sender's session then fails by ACK timeout) — senders' value must not exceed
// any receiver they target.
#ifndef RF_MAX_FRAGMENTED_PAYLOAD
#  define RF_MAX_FRAGMENTED_PAYLOAD 3072
#endif

// RFMessage pool size — also caps in-flight TX + pending-ACK entries.
#ifndef RF_POOL_SIZE
#  define RF_POOL_SIZE   8
#endif

// Engine event queue depth. DERIVED — tracks RF_POOL_SIZE (queue carries at most
// one TX_REQ per pool slot). HW_IRQ/SHUTDOWN control events share it; a lost
// HW_IRQ push is harmless (worker re-polls radio every wake), so headroom is
// rarely needed. Override independently only if a bursty ISR outpaces the worker.
#ifndef RF_EVENT_QUEUE_DEPTH
#  define RF_EVENT_QUEUE_DEPTH RF_POOL_SIZE
#endif

// Deferred forward queue (mesh).
#ifndef RF_FWD_QUEUE_SIZE
#  define RF_FWD_QUEUE_SIZE 8
#endif

// Mesh forward retry budget: a forward whose LBT finds the channel busy is
// rescheduled with random backoff up to this many times before being dropped.
// Counted only on CHANNEL_BUSY — duty-cycle waits reschedule separately.
// Raising it trades longer ACK timeouts (per-hop budget grows linearly with
// this, see Engine::_resolveAckTimeoutMs) for better loss tolerance under contention.
#ifndef RF_FWD_RETRY_MAX
#  define RF_FWD_RETRY_MAX 2
#endif

// Engine-side wire frame buffer (header + payload + tag). DERIVED from the HAL's
// per-frame cap (MAX_PAYLOAD_SIZE, IRadio.h) via static_assert below — not
// tunable: lowering fails to compile, raising only wastes RAM.
#ifndef RF_FRAME_BUF_SIZE
#  define RF_FRAME_BUF_SIZE MAX_PAYLOAD_SIZE
#endif

// CSMA-CA contention slots for pre-send/pre-forward jitter (window = K × ToA;
// 0 disables jitter). Also feeds the auto-resolved mesh ACK timeout — raising it
// lengthens mesh ACK timeouts (Engine::_resolveAckTimeoutMs) and, transitively,
// the reassembly timeout (Engine::_reasmTimeoutMs).
#ifndef RF_JITTER_WINDOW_SLOTS
#  define RF_JITTER_WINDOW_SLOTS 4
#endif

// SeenCache (ACK dedupe) FIFO depth.
#ifndef RF_SEEN_CACHE_SIZE
#  define RF_SEEN_CACHE_SIZE 32
#endif

// ReplayWindow per-peer history depth (bits). Bitmap is uint32_t → max 32.
#ifndef RF_REPLAY_WINDOW_BITS
#  define RF_REPLAY_WINDOW_BITS 32
#endif

// ReplayWindow peer table size (LRU) — backs anti-replay and mesh forward
// de-duplication. Evicting a peer under load reopens its replay window (attacker
// can replay its last captured frame) and forward suppression. Size should be >=
// the number of originators whose traffic transits any node (~16 B/peer); raise
// for larger meshes, lower only on RAM-constrained single-hop links.
#ifndef RF_REPLAY_MAX_PEERS
#  define RF_REPLAY_MAX_PEERS 32
#endif

// Concurrent reassembly slots for incoming fragmented messages.
#ifndef RF_REASM_SLOTS
#  define RF_REASM_SLOTS 4
#endif

// Incomplete reassembly slot lifetime FLOOR (ms) — effective timeout is
// max(this, worst-case sender retry schedule): Engine::_reasmTimeoutMs scales it
// up with RF_FRAG_RETRY_MAX, hop count, and jitter/forward-retry slots so a
// slow-but-alive sender's slot isn't evicted mid-session (at the cost of holding
// incomplete slots, and their RAM, longer).
#ifndef RF_REASM_TIMEOUT_MS
#  define RF_REASM_TIMEOUT_MS 30000
#endif

// Concurrent outgoing large-message send sessions.
#ifndef RF_LARGE_TX_SESSIONS
#  define RF_LARGE_TX_SESSIONS 1
#endif

// Per-fragment retransmission budget within a large-send session: a fragment
// that fails to TX or times out its ACK is re-sent up to this many times before
// failing the whole session (Reassembler dedupes retransmits by msgId+idx).
// 0 = one failed fragment kills the session; too high inflates session duration
// unboundedly under persistent interference.
// Also bounds receiver-side reassembly eviction timeout — each node uses its OWN
// value to estimate the sender's worst case (Engine::_reasmTimeoutMs), so a
// receiver set much lower than its senders may evict in-progress slots early.
#ifndef RF_FRAG_RETRY_MAX
#  define RF_FRAG_RETRY_MAX 2
#endif

// Outgoing ACK retry queue. When the receiver's ACK attempt hits busy CCA (or
// radio.send fails), the ACK is parked here and retried with random backoff up
// to RF_ACK_RETRY_MAX times. Without this, one lost ACK kills the originator's
// pending message — especially painful for fragmented sends, where a missing
// ACK times out the whole session.
#ifndef RF_ACK_QUEUE_SIZE
#  define RF_ACK_QUEUE_SIZE 4
#endif

// Max retries per outgoing ACK when CCA is busy or radio.send fails. Each retry
// waits a uniform random backoff in [ToA, 2×ToA] (fixed, doesn't grow between
// attempts — see Engine::_busyBackoffMs); RF_ACK_RETRY_FALLBACK_TOA_MS substitutes
// for ToA when the HAL can't report airtime.
#ifndef RF_ACK_RETRY_MAX
#  define RF_ACK_RETRY_MAX 3
#endif

// CSMA-CA RSSI threshold (dBm, integer).
#ifndef RF_CCA_RSSI_THRESHOLD
#  define RF_CCA_RSSI_THRESHOLD  -85
#endif

// Auto-resolved ACK timeout margin (ms) added on top of 2×ToA(ack). Covers
// scheduler jitter, peer-side CCA cost, and serial-print blocking in user
// callbacks. Override at compile time or per-RFNode via
// RFReliabilityConfig::ackTimeoutMarginP2Pms.
#ifndef RF_ACK_TIMEOUT_MARGIN_P2P_MS
#  define RF_ACK_TIMEOUT_MARGIN_P2P_MS  100
#endif

// Same margin, for multi-hop mesh ACKs — longer round-trip, more delay sources
// (per-hop CCA, forward jitter, etc).
#ifndef RF_ACK_TIMEOUT_MARGIN_MESH_MS
#  define RF_ACK_TIMEOUT_MARGIN_MESH_MS 200
#endif

// Fallback per-frame ToA estimate (ms), used wherever non-zero airtime is
// required but HAL reports 0 (getAirtimeMs): random busy backoff for ACK
// retries, fragment re-sends and mesh forwards (Engine::_busyBackoffMs), and
// the conservative duty-cycle charge when enforcement is on but airtime is
// unknown (Engine::_txAndAccount). Must be >= 1 — 0 zeroes those backoffs and
// reopens the duty-cycle free-pass hole the floor charge closes.
#ifndef RF_ACK_RETRY_FALLBACK_TOA_MS
#  define RF_ACK_RETRY_FALLBACK_TOA_MS 30
#endif

// Max time (ms) end() waits for the background worker task to exit gracefully.
// On expiry end() does NOT free anything (freeing while the worker may still
// touch Engine state would be a use-after-free) — it detaches the radio IRQ,
// returns false, and defers teardown until end() is called again.
#ifndef RF_WORKER_SHUTDOWN_TIMEOUT_MS
#  define RF_WORKER_SHUTDOWN_TIMEOUT_MS 500
#endif

// Worker poll interval (ms) while a large-send session stalls waiting for a
// pool slot (no pending/delayed entry of its own to wake the worker).
// Lower = resumes sooner after a slot frees; higher = fewer idle wakeups.
// Rarely engages — pool's fragment reserve normally prevents this state.
#ifndef RF_POOL_STALL_POLL_MS
#  define RF_POOL_STALL_POLL_MS 10
#endif

// PBKDF2-SHA256 iteration count for FromPassword(). Tuned for embedded MCUs
// (~150 ms on ESP32-S3; longer on Cortex-M0 — raise/lower per target). Higher =
// better key-stretching against offline attacks, but only matters for weak
// passwords; high-entropy passwords barely benefit.
#ifndef RF_KDF_DEFAULT_ITERATIONS
#  define RF_KDF_DEFAULT_ITERATIONS 10000
#endif

// PBKDF2 salt for FromPassword() (string literal). Shared by every RFNet
// deployment by default, so identical passwords derive identical keys across
// unrelated networks — override per deployment to defeat precomputed dictionary
// attacks.
// NETWORK INVARIANT: every node MUST use the same salt (it's mixed into the key)
// — changing it is a network-wide rekey; a mismatched node derives a different
// key and fails authentication, indistinguishable from a wrong key.
#ifndef RF_KDF_SALT
#  define RF_KDF_SALT "RFNet-PBKDF2-SHA256-v1"
#endif

// CSMA-CA RSSI sweep timeout (ms). LBT total cost ≈ CAD time (chip-internal,
// SF-dependent) + this sweep. Lower = faster TX path but more false-free reads
// on intermittent interferers; higher = slower TX but more reliable busy detection.
#ifndef RF_CCA_TIMEOUT_MS
#  define RF_CCA_TIMEOUT_MS 10
#endif

// RSSI sampling interval (ms) within the CCA sweep. Lower = finer busy detection
// (catches shorter bursts) at cost of more SPI reads per sweep; must be >= 1
// (0 spins the sweep loop forever).
#ifndef RF_CCA_SWEEP_STEP_MS
#  define RF_CCA_SWEEP_STEP_MS 2
#endif

// Settle time (ms) after arming RX before the first RSSI sample of a CCA sweep
// — lets AGC converge so the read reflects the channel, not the amplifier ramp.
// Chip-family dependent; 3 ms is safe for SX126x/CC1101.
#ifndef RF_CCA_SETTLE_MS
#  define RF_CCA_SETTLE_MS 3
#endif

// EEPROM backend only: bytes reserved per slot id — must hold the largest record
// persisted into it plus the 1-byte "written" marker the backend prepends. Each
// consumer static_asserts its own record against this at its definition site
// (MonotonicCounter.h, DutyCycleTracker.h): shrinking it below a consumer's need
// fails the build there instead of silently disabling persistence at runtime.
#ifndef RF_NV_SLOT_BYTES
#  define RF_NV_SLOT_BYTES        16
#endif

// EEPROM backend only: bytes passed to EEPROM.begin() on cores with emulated
// EEPROM (ESP8266, RP2040, STM32 Arduino — AVR is directly addressable, ignores
// this). Bounds usable slot-id space: every id used via RFNvConfig must satisfy
// baseAddr + (id + 1) × RF_NV_SLOT_BYTES <= this. Default 512 → ids 0..31 with
// default slot size and baseAddr 0.
#ifndef RF_EEPROM_SIZE_BYTES
#  define RF_EEPROM_SIZE_BYTES    512
#endif

// Preferences (ESP32 NVS) backend only: namespace string for RFNet's keys.
// Override if it collides with a namespace your application already uses.
// PERSISTENCE INVARIANT: changing this orphans previously stored records — after
// an update with a new namespace the node behaves like first boot (seq counter
// restarts, pending duty-cycle off-time forgotten).
#ifndef RF_NVS_NAMESPACE
#  define RF_NVS_NAMESPACE        "lora"
#endif

// ─── Cross-cutting invariants ────────────────────────────────────────────────

// Frame buffer must fit the largest possible L3 frame: fragmented header +
// max payload + AEAD tag. (HAL adds the 1-byte L2 dst byte separately.)
static_assert(RF_MAX_PAYLOAD + PACKET_HEADER_SIZE_FRAG + CRYPTO_TAG_SIZE
              <= RF_FRAME_BUF_SIZE,
              "RF_FRAME_BUF_SIZE too small for max encrypted fragmented frame");

static_assert(MAX_PAYLOAD_SIZE <= RF_FRAME_BUF_SIZE,
              "HAL MAX_PAYLOAD_SIZE exceeds engine RF_FRAME_BUF_SIZE — "
              "decrypt-in-place would forward plaintext on oversized frames");

// On-air frame = L3 frame + 1 L2 byte. Must fit in the HAL's per-frame cap.
static_assert(RF_MAX_PAYLOAD + PACKET_HEADER_SIZE_FRAG + CRYPTO_TAG_SIZE + 1
              <= MAX_PAYLOAD_SIZE,
              "Max L3 frame + L2 byte exceeds HAL MAX_PAYLOAD_SIZE — "
              "shrink RF_MAX_PAYLOAD or raise MAX_PAYLOAD_SIZE");

// Protocol cap: RF_MAX_FRAGMENTS fragments of RF_MAX_PAYLOAD each.
static_assert(RF_MAX_FRAGMENTED_PAYLOAD <= RF_MAX_FRAGMENTS * RF_MAX_PAYLOAD,
              "RF_MAX_FRAGMENTED_PAYLOAD exceeds the fragment protocol limit");

static_assert(RF_MAX_PAYLOAD >= 1, "RF_MAX_PAYLOAD must be >= 1");
// RFNode::send validates len against RF_MAX_FRAGMENTED_PAYLOAD BEFORE the
// single-frame/fragmented split, so a smaller value would BAD_LENGTH
// payloads that fit a single frame — contradicting the documented API
// ("len <= RF_MAX_PAYLOAD sends a single frame").
static_assert(RF_MAX_FRAGMENTED_PAYLOAD >= RF_MAX_PAYLOAD,
              "RF_MAX_FRAGMENTED_PAYLOAD must be >= RF_MAX_PAYLOAD — smaller "
              "values reject single-frame sends the API documents as valid");

static_assert(RF_REPLAY_WINDOW_BITS >= 1 && RF_REPLAY_WINDOW_BITS <= 32,
              "RF_REPLAY_WINDOW_BITS must be in [1, 32] (uint32_t bitmap; 0 "
              "silently degrades to strictly-increasing-seq-only acceptance)");

static_assert(RF_POOL_SIZE          >= 1, "RF_POOL_SIZE must be >= 1");
// _poolAlloc reserves RF_LARGE_TX_SESSIONS slots for fragment continuations;
// at least one slot must remain available to ordinary sends.
static_assert(RF_POOL_SIZE > RF_LARGE_TX_SESSIONS,
              "RF_POOL_SIZE must exceed RF_LARGE_TX_SESSIONS (fragment reserve)");
static_assert(RF_FWD_QUEUE_SIZE     >= 1, "RF_FWD_QUEUE_SIZE must be >= 1");
static_assert(RF_EVENT_QUEUE_DEPTH  >= 1, "RF_EVENT_QUEUE_DEPTH must be >= 1");
static_assert(RF_CCA_SWEEP_STEP_MS  >= 1,
              "RF_CCA_SWEEP_STEP_MS must be >= 1 — 0 spins the CCA sweep loop forever");
static_assert(RF_POOL_STALL_POLL_MS >= 1,
              "RF_POOL_STALL_POLL_MS must be >= 1 — 0 busy-spins the worker");
// Room for at least the two default slot ids (RFNvConfig idSeq=1, idDuty=2)
// at baseAddr 0. Runtime ids/baseAddr can still exceed the EEPROM — the
// backend then fails the read/write loudly at runtime.
static_assert(3 * RF_NV_SLOT_BYTES <= RF_EEPROM_SIZE_BYTES,
              "RF_EEPROM_SIZE_BYTES too small for the default NV slot ids");
static_assert(RF_REPLAY_MAX_PEERS   >= 1, "RF_REPLAY_MAX_PEERS must be >= 1");
static_assert(RF_SEEN_CACHE_SIZE    >= 1, "RF_SEEN_CACHE_SIZE must be >= 1");
static_assert(RF_REASM_SLOTS        >= 1, "RF_REASM_SLOTS must be >= 1");
static_assert(RF_LARGE_TX_SESSIONS  >= 1, "RF_LARGE_TX_SESSIONS must be >= 1");
static_assert(RF_ACK_QUEUE_SIZE     >= 1, "RF_ACK_QUEUE_SIZE must be >= 1");

// Several knobs land in uint8_t storage or counters (SeenCache::SIZE,
// ReplayWindow::MAX_PEERS, retry counters/config defaults, the pool batch
// counter in _flushDeferred) — an oversized value would truncate silently
// (e.g. 300 → 44) instead of failing the build. RF_LARGE_TX_SESSIONS is
// additionally capped by int8_t loop indices in Engine.cpp.
static_assert(RF_POOL_SIZE         <= 255, "RF_POOL_SIZE must fit uint8_t counters");
static_assert(RF_LARGE_TX_SESSIONS <= 127, "RF_LARGE_TX_SESSIONS must fit int8_t loop indices");
static_assert(RF_SEEN_CACHE_SIZE   <= 255, "RF_SEEN_CACHE_SIZE is stored in a uint8_t");
static_assert(RF_REPLAY_MAX_PEERS  <= 255, "RF_REPLAY_MAX_PEERS is stored in a uint8_t");
static_assert(RF_FWD_RETRY_MAX     <= 255, "RF_FWD_RETRY_MAX is compared against a uint8_t retry counter");
static_assert(RF_FRAG_RETRY_MAX    <= 255, "RF_FRAG_RETRY_MAX is stored in a uint8_t");
static_assert(RF_ACK_RETRY_MAX     <= 255, "RF_ACK_RETRY_MAX is stored in a uint8_t");

static_assert(RF_CCA_TIMEOUT_MS >= 1,
              "RF_CCA_TIMEOUT_MS must be >= 1 — 0 takes zero RSSI samples and "
              "silently disables LBT (while still paying the settle delay)");
static_assert(RF_CCA_RSSI_THRESHOLD >= -128 && RF_CCA_RSSI_THRESHOLD <= 127,
              "RF_CCA_RSSI_THRESHOLD must fit int8_t (IRadio::cca signature)");

static_assert(RF_KDF_DEFAULT_ITERATIONS >= 1,
              "RF_KDF_DEFAULT_ITERATIONS must be >= 1 — PBKDF2 with 0 "
              "iterations is undefined across crypto backends");

static_assert(RF_ACK_RETRY_FALLBACK_TOA_MS >= 1,
              "RF_ACK_RETRY_FALLBACK_TOA_MS must be >= 1 — 0 zeroes busy "
              "backoffs and voids the duty-cycle floor charge");
static_assert(RF_WORKER_SHUTDOWN_TIMEOUT_MS >= 1,
              "RF_WORKER_SHUTDOWN_TIMEOUT_MS must be >= 1 — 0 makes end() "
              "report failure without waiting for the worker at all");

// Some invariants live at their consumer's definition site instead:
//   - RF_NV_SLOT_BYTES vs the persisted structs (MonotonicCounter.h,
//     DutyCycleTracker.h) — sizeof() is visible there;
//   - RF_REASM_SLOTS <= 127 (Reassembler.h, ABORTED_DEPTH indexing);
//   - RF_KDF_SALT non-empty (RFNode.cpp);
//   - RF_NVS_NAMESPACE length 1..15 (PreferencesBackend.h, ESP-IDF limit).

static_assert(RF_DEFAULT_HOP_COUNT >= 0 && RF_DEFAULT_HOP_COUNT <= 7,
              "RF_DEFAULT_HOP_COUNT must be in [0, 7] (3-bit protocol field)");
