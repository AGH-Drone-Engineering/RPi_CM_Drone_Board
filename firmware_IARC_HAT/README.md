# LoRa

Firmware for a two-node LoRa link (transmitter / receiver) built on top of **[RFNet](lib/RFNet/README.md)**, a transport-agnostic RF networking middleware living in this repo as a PlatformIO library.

- `src/transmitter/` — P2P node (`addr 0x01`) that periodically sends unacked, acked, broadcast and large (fragmented) messages to the receiver.
- `src/receiver/` — Mesh-mode node (`addr 0x02`) that listens and logs incoming traffic.
- `lib/RFNet/` — the RFNet library itself: wire framing, fragmentation, AES-GCM security, EU duty-cycle accounting, P2P/mesh routing, and the HAL/OSAL port layer. See its [README](lib/RFNet/README.md) for the library API and usage.

Both nodes target a Seeed XIAO ESP32S3 driving an SX1262 LoRa radio (EU868) and share the same security password — update `secret_password` in both `main.cpp` files before deploying, and confirm the duty-cycle/regulatory settings for your band (currently disabled in both examples via `c.dutyCycle.enabled = false`).

## Building

Requires [PlatformIO](https://platformio.org/).

```sh
pio run -e transmitter   # build+flash the transmitter node
pio run -e receiver       # build+flash the receiver node
pio run -e native         # host build (excludes Arduino/RadioLib-specific code)
```

Environments are defined in [platformio.ini](platformio.ini).
