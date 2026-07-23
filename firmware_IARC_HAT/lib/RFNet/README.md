# RFNet

Transport-agnostic RF networking middleware for embedded radios. RFNet sits between your application and a physical radio and provides a compact wire frame, transparent fragmentation, AES-GCM security, EU duty-cycle enforcement, and P2P + mesh routing — all behind a single `RFNode` facade.

## Features

- **Compact wire frame** — 6-byte header (8 when fragmented), 24-bit sequence, 1-byte addressing with `0xFF` broadcast.
- **Transparent fragmentation** — messages up to `RF_MAX_FRAGMENTED_PAYLOAD` (default 3072 B, hard ceiling 16 fragments) are split and reassembled automatically.
- **Security** — AES-GCM (software / mbedTLS / STM32 CRYP backends) with the full header authenticated as AAD, an NVS-persisted monotonic nonce counter, and a per-peer sliding-window replay filter. Key from raw bytes, a PBKDF2 password, or a custom `ICypher`.
- **EU duty cycle** — ETSI EN 300 220 air-time accounting (on by default; engages when the radio reports a regulatory limit or a manual override is set, and is a no-op only when neither applies).
- **P2P and mesh** — managed flooding (with optional RSSI suppression) or static routing via the `IRoutingStrategy` port, with automatic duplicate suppression and per-message hop budgets.
- **Portable** — HAL / OSAL / crypto / NVS / clock port layer; the whole stack is driven either by a background task or a cooperative `poll()`.

## Quick start

```cpp
#include <RFNode.h>

SX1262LoRaRadio radio(SX1262LoRaRadio::Channel::EU868_CH0, /*cs*/41, /*irq*/39, /*rst*/42, /*busy*/40);

RFNodeConfig cfg;
cfg.addr     = 0x01;  // [0x01, 0xFE]; 0x00 reserved (begin() rejects it), 0xFF = broadcast
cfg.mode     = PacketMode::P2P;
cfg.security = RFSecurityConfig::FromPassword("correct horse battery staple");

RFNode node(radio, cfg);

void setup() {
    node.onReceive([](const RxInfo& info, const uint8_t* data, size_t len, void*) {
        // handle received message
    });
    if (node.begin() != BeginStatus::OK) { /* handle */ }
    node.startWorkerTask();              // or call node.poll() from loop()
}

void loop() {
    SendStatus st = node.sendAck(0x02, "hello");   // check it — send() is [[nodiscard]]
    delay(5000);
}
```

Every node on the same link must agree on the parts they share: address space, the security key/password, the radio channel, and the modulation profile (`setTransmitProfile()` — SF/BW/CR must match or peers can't demodulate each other at all).

## API at a glance

| Call | Purpose |
| --- | --- |
| `begin()` / `end()` | Bring the node up / tear it down (both idempotent-safe; `begin()` is `[[nodiscard]]`). |
| `send(dst, data, len)` | Fire-and-forget unicast — no ACK requested. |
| `sendAck(dst, data, len)` | Reliable unicast — requests an ACK with an auto-resolved timeout, reports via `onSendOk`/`onSendFail`. One attempt: on failure, retrying is your call (fragmented large sends are the exception — each fragment is retried internally). |
| `sendBroadcast(data, len)` | Send to `0xFF`; never ACKed by definition. |
| `send(dst, data, len, SendOptions)` | Full-featured overload — explicit ACK, hop count and timeout knobs (the other overloads above just pick defaults for these). |
| `onReceive` / `onSendOk` / `onSendFail` | Register callbacks (set before `begin()`). |
| `startWorkerTask()` / `poll()` | Pick one driving model (see below). |
| `getDutyCycleWaitMs(len)` | Advisory pre-flight: `0` = clear, `N` = wait ms, `UINT32_MAX` = never fits. |

Return/reason codes live in `BeginStatus`, `SendStatus` and `TxFailReason` (`RFTypes.h`). A single-frame send returns only resource statuses; the duty-cycle outcome arrives asynchronously via `onSendFail`.

## Driving model

Pick exactly one — never mix them:

- **`startWorkerTask()`** — a background task owns RX / TX / ACK / forwarding / timers. Recommended on an RTOS.
- **`poll()`** — call frequently from your main loop; RX and ACK latency are bounded by how often you call it. The only option on a target with no scheduler.

## Configuration

Runtime knobs live in `RFNodeConfig` (`security`, `dutyCycle`, `mesh`, `reliability`, `nv`). Compile-time limits — payload sizes, table depths, timeouts — live in `core/RFConfig.h` and are overridable with `-D` build flags.

> **Security note:** encryption requires a persistent NV backend. `begin()` refuses to start an encrypted node on volatile storage (the nonce counter would reset every reboot and reuse GCM nonces) unless `RF_ALLOW_VOLATILE_NV` is defined — do not define it in production.

## Ports & platform support

Each port auto-selects a default by platform; all are includable unconditionally and only the applicable ones compile.

| Port | Built-in implementations |
| --- | --- |
| Radio (`IRadio`) | `SX1262LoRaRadio`, `CC1101FSKRadio` |
| Crypto (`ICypher`) | `AesGcmSoft`, `AesGcmMbedTls`, `AesGcmStm32Hal` (auto: mbedTLS → STM32 CRYP → software) |
| OSAL (`IOsal`) | `FreeRtosOsal` (ESP32), `BaremetalOsal` (Arduino), `PosixOsal` (host) |
| NVS (`INVBackend`) | `PreferencesBackend` (ESP32), `EepromBackend` (Arduino), `ZephyrNvsBackend`, `NullNVBackend` |
| Clock (`IClock`) | `EspClock` (SNTP), `PosixClock`, `ZephyrClock`, `NullClock` |

> A Zephyr OSAL isn't shipped yet — building for Zephyr fails immediately with a clear `#error`, with no constructor-injection workaround like the other ports have, until a `ZephyrOsal` is added.

## Examples

See `examples/`:

- **`simple/`** — `send`, `receive`, `poll_driven`
- **`security/`** — `raw_key`, `password`, `nvs_persistence`, `custom_cipher`
- **`mesh/`** — `flooding`, `hop_count`, `rssi_suppression`, `static_routing`
- **`advanced/`** — `duty_cycle`, `custom_clock`, `custom_osal` (bare-metal / no-RTOS), `reliability_tuning`

## Status

Version 1.0.0.

