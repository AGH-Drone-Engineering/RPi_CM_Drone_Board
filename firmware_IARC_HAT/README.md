# LoRa

Firmware for a two-node LoRa link (transmitter / receiver) built on top of **[RFNet](lib/RFNet/README.md)**, a transport-agnostic RF networking middleware living in this repo as a PlatformIO library.

- `src/transmitter/` — P2P node (`addr 0x01`) that periodically sends unacked, acked, broadcast and large (fragmented) messages to the receiver.
- `src/receiver/` — Mesh-mode node (`addr 0x02`) that listens and logs incoming traffic.
- `src/node/` — the IARC HAT node: bridges the RPi's UART protocol (see [firmware_CMDB/loracom/protocol.adoc](../firmware_CMDB/loracom/protocol.adoc)) to RFNet. Its address is **not** compiled in - it comes from solder jumpers JP1..JP5 (`ADDR1/2/4/8/16`), bridged high for a set bit: JP1 = 1, JP2 = 2, JP3 = 4, JP4 = 8, JP5 = 16, summed. Valid range is 1..31; with all five open the address would be 0, which RFNet rejects, so the node logs `boot_error reason=addr_jumpers_unset` and halts. The same address is what `GETCONF` reports as `CMDB_ID` — and the RPi names itself after it at boot, as `raspi-usa-<addr>` (JP1+JP2 bridged → address 3 → `raspi-usa-3`), so re-soldering the jumpers renames the Pi too.
- `lib/RFNet/` — the RFNet library itself: wire framing, fragmentation, AES-GCM security, EU duty-cycle accounting, P2P/mesh routing, and the HAL/OSAL port layer. See its [README](lib/RFNet/README.md) for the library API and usage.

On the other side of that UART link, the RPi needs UART3 enabled before it can talk to the HAT at all — that's done once per image by `system-setup.sh`, which also sets up the camera, VNC and the packages we work with. See [firmware_CMDB/README.adoc](../firmware_CMDB/README.adoc).

Both nodes target a Seeed XIAO ESP32S3 driving an SX1262 LoRa radio (EU868) and share the same security password — update `secret_password` in both `main.cpp` files before deploying, and confirm the duty-cycle/regulatory settings for your band (currently disabled in both examples via `c.dutyCycle.enabled = false`).

## Building

Requires [PlatformIO](https://platformio.org/).

```sh
pio run -e transmitter   # build+flash the transmitter node
pio run -e receiver       # build+flash the receiver node
pio run -e native         # host build (excludes Arduino/RadioLib-specific code)
```

Environments are defined in [platformio.ini](platformio.ini).
