# LoRa

Firmware for a two-node LoRa link (transmitter / receiver) built on top of **[RFNet](lib/RFNet/README.md)**, a transport-agnostic RF networking middleware living in this repo as a PlatformIO library.

- `src/transmitter/` — P2P node (`addr 0x01`) that periodically sends unacked, acked, broadcast and large (fragmented) messages to the receiver.
- `src/receiver/` — Mesh-mode node (`addr 0x02`) that listens and logs incoming traffic.
- `src/node/` — the IARC HAT node: bridges the RPi's UART protocol (see [firmware_CMDB/loracom/protocol.adoc](../firmware_CMDB/loracom/protocol.adoc)) to RFNet. Its address is **not** compiled in - it comes from solder jumpers JP1..JP5 (`ADDR1/2/4/8/16`), bridged high for a set bit: JP1 = 1, JP2 = 2, JP3 = 4, JP4 = 8, JP5 = 16, summed. Valid range is 1..31; with all five open the address would be 0, which RFNet rejects, so the node logs `boot_error reason=addr_jumpers_unset` and halts. The same address is what `GETCONF` reports as `CMDB_ID`, alongside `CMDB_ESP_FIRMWARE_BUILD` — see [Build stamp](#build-stamp) below.
- `lib/RFNet/` — the RFNet library itself: wire framing, fragmentation, AES-GCM security, EU duty-cycle accounting, P2P/mesh routing, and the HAL/OSAL port layer. See its [README](lib/RFNet/README.md) for the library API and usage.

Both nodes target a Seeed XIAO ESP32S3 driving an SX1262 LoRa radio (EU868) and share the same security password — update `secret_password` in both `main.cpp` files before deploying, and confirm the duty-cycle/regulatory settings for your band (currently disabled in both examples via `c.dutyCycle.enabled = false`).

## Building

Requires [PlatformIO](https://platformio.org/).

```sh
pio run -e transmitter   # build the transmitter node
pio run -e receiver      # build the receiver node
pio run -e node          # build the IARC HAT node
pio run -e node -t upload --upload-port COM3   # flash it
```

Environments are defined in [platformio.ini](platformio.ini).

## Build stamp

[`scripts/git_version.py`](scripts/git_version.py) runs before every build (wired in as `extra_scripts` in [platformio.ini](platformio.ini)) and writes `$BUILD_DIR/version.h`:

```c
#define CMDB_ESP_FIRMWARE_BUILD "<8-char commit>"
```

The value is suffixed `-dirty` when `src/`, `lib/`, `platformio.ini` or `scripts/` differ from that commit, and is the literal `unknown` when there's no usable git checkout — a missing git binary, a source export, any git failure. Edits elsewhere in the repo don't affect it: the stamp answers "was this image built from exactly these committed sources".

The node reports it two ways, so it's readable whether or not the link to the Pi is up: in its `GETCONF` response (`loracom --config` on the Pi) and in the `ready` line of the USB-CDC boot log.

The header is only rewritten when the value changes, so a rebuild after a new commit recompiles just the files that include it. It deliberately isn't a `-D` on the command line: that would change every compile line, the Arduino core's included, and force a full rebuild on every commit.
