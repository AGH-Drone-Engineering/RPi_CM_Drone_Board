#!/bin/sh
# One-shot provisioning for a fresh CM5 image. Runs the four install-*.sh
# scripts, which do all the work and are each usable on their own. Idempotent -
# re-running it is a no-op except for package upgrades. A reboot is required
# afterwards for config.txt.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

cat <<'EOF'
system-setup.sh changes the following:
  * everything install-cmdb-base.sh, install-utils.sh, install-cam.sh and
    install-loracom.sh change - see each for the details
EOF

# base first: it names the board, so a failure further down still leaves
# something reachable. utils second: it brings in the toolchain
# install-loracom.sh compiles with, and the MAVProxy the base service runs.
"$SCRIPT_DIR/install-cmdb-base.sh"
"$SCRIPT_DIR/install-utils.sh"
"$SCRIPT_DIR/install-cam.sh"
"$SCRIPT_DIR/install-loracom.sh"

echo "system-setup.sh: done - reboot for the UART and camera overlays to take effect."

if command -v apt-mark >/dev/null 2>&1 && [ -n "$(apt-mark showhold 2>/dev/null)" ]; then
    echo "system-setup.sh: kernel updates are held (see install-cam.sh) - 'apt-mark showhold' lists them"
fi
