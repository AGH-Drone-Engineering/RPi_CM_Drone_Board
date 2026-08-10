#!/bin/sh
# One-shot provisioning for a fresh CM5 image. Runs the four install-*.sh
# scripts, which do all the work and are each usable on their own, then turns
# the onboard Bluetooth off - the one change that belongs to no single one of
# them. Idempotent - re-running it is a no-op except for package upgrades. A
# reboot is required afterwards for config.txt.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

cat <<'EOF'
system-setup.sh changes the following:
  * everything install-cmdb-base.sh, install-utils.sh, install-cam.sh and
    install-loracom.sh change - see each for the details
  * config.txt: onboard Bluetooth disabled (dtoverlay=disable-bt)
  * hciuart.service: disabled, it has nothing left to talk to
EOF

# base first: it names the board, so a failure further down still leaves
# something reachable. utils second: it brings in the toolchain
# install-loracom.sh compiles with, and the MAVProxy the base service runs.
"$SCRIPT_DIR/install-cmdb-base.sh"
"$SCRIPT_DIR/install-utils.sh"
"$SCRIPT_DIR/install-cam.sh"
"$SCRIPT_DIR/install-loracom.sh"

# ---------------------------------------------------------------------------
# Bluetooth
# ---------------------------------------------------------------------------

# Nothing on the drone uses it, and the radio shares the 2.4GHz band with the
# WiFi the board is reached over. Its own block, like the ones the install
# scripts maintain, so the four of them stay untouched by this.
configtxt_init

# disable-bt-pi5 is the bcm2712 version - it only disables the Bluetooth node,
# it does not touch GPIO14/15, so it doesn't collide with the uart0-pi5 overlay
# install-cmdb-base.sh puts in for the FC link. Older firmware only ships the
# non-suffixed overlay, which on Pi 5 silicon may be the pre-Pi 5 one.
if [ -e "$OVERLAY_DIR/disable-bt-pi5.dtbo" ]; then
    BT_OVERLAY=disable-bt-pi5
else
    BT_OVERLAY=disable-bt
    echo "system-setup.sh: WARNING: $OVERLAY_DIR/disable-bt-pi5.dtbo missing, falling back to 'dtoverlay=disable-bt' - update your firmware if 'hciconfig' still shows an adapter after the reboot" >&2
fi

# The disable regex catches a hand-added disable-bt line elsewhere in the file,
# which would load the overlay a second time.
echo "system-setup.sh: disabling onboard Bluetooth ($BT_OVERLAY) in $CONFIG_TXT"
configtxt_set_block cmdb-bt '^[[:space:]]*dtoverlay=disable-bt' <<EOF
dtoverlay=$BT_OVERLAY
EOF

# It uploads the Bluetooth firmware over the UART the overlay just took away, so
# from the next boot on it can only fail. Only touched if it's actually there.
if systemctl is-enabled --quiet hciuart.service 2>/dev/null || systemctl is-active --quiet hciuart.service 2>/dev/null; then
    echo "system-setup.sh: disabling hciuart.service, it has nothing left to talk to"
    systemctl disable --now hciuart.service >/dev/null 2>&1 || :
fi

echo "system-setup.sh: done - reboot for the UART, camera and Bluetooth overlays to take effect."

if command -v apt-mark >/dev/null 2>&1 && [ -n "$(apt-mark showhold 2>/dev/null)" ]; then
    echo "system-setup.sh: kernel updates are held (see install-cam.sh) - 'apt-mark showhold' lists them"
fi
