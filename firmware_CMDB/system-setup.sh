#!/bin/sh
# One-shot provisioning for a fresh CM5 image.
#
# Records what this board is and gives it a hostname of CMDB-<mac> (a baseline
# for the case where cmdb-bootstrap.service never comes up to set the real one),
# then hands off to the install-*.sh scripts, which do the actual work and are
# each usable on their own:
#   install-utils.sh    apt/pip packages, VNC, Claude Code
#   install-cam.sh      FRAMOS IMX900 on CAM0 - drivers, libcamera, overlay.
#                       Also holds the kernel packages: the sensor modules are
#                       out-of-tree and stop loading when the kernel moves.
#   install-loracom.sh  UART3 + the CMDB tools
#
# Everything here is idempotent: re-running it is a no-op except for package
# upgrades. A reboot is required afterwards for the config.txt changes.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

: "${ENV_FILE:=/etc/environment}"

# What this board is. Fixed per board variant - the per-unit identity
# (CMDB_ID) comes from the HAT at boot instead, see bootstrap/bootstrap.sh.
HW_REVISION=1
HW_TYPE=STANDARD

# Prefix of the MAC-based hostname set below. Intentionally not the
# "raspi-usa" bootstrap.sh names the board with once the HAT has answered - a
# CMDB-* name is the visible sign that this board is running on the provisioning
# baseline and hasn't been given its identity by the HAT yet.
HOSTNAME_PREFIX=CMDB

MARKER_BEGIN="# BEGIN cmdb-system-setup"
MARKER_END="# END cmdb-system-setup"

# Own block in $ENV_FILE, separate from the one bootstrap.sh maintains there
# for the HAT-provided variables - each script only ever rewrites its own.
echo "system-setup.sh: recording hardware identity in $ENV_FILE"

env_tmp=$(mktemp "${ENV_FILE}.XXXXXX")
cmdb_track_tmp "$env_tmp"
chmod 644 "$env_tmp"

awk -v b="$MARKER_BEGIN" -v e="$MARKER_END" '
    $0==b {skip=1; next}
    $0==e {skip=0; next}
    !skip {print}
' "$ENV_FILE" 2>/dev/null > "$env_tmp" || :

{
    echo "$MARKER_BEGIN"
    echo "CMDB_HARDWARE_REVISION=$HW_REVISION"
    echo "CMDB_HARDWARE_TYPE=$HW_TYPE"
    echo "$MARKER_END"
} >> "$env_tmp"

mv "$env_tmp" "$ENV_FILE"
echo "system-setup.sh: CMDB_HARDWARE_REVISION=$HW_REVISION CMDB_HARDWARE_TYPE=$HW_TYPE"

# Baseline hostname, so the board never sits on the image's default name (every
# fresh CM5 is "raspberrypi", which collides over mDNS the moment two of them
# are on the same network). bootstrap.sh overwrites this at every boot with the
# HAT-provided <prefix>-<CMDB_ID>, or with its own MAC-based fallback if the HAT
# doesn't answer - but that only happens once cmdb-bootstrap.service actually
# runs. This is what's left if the service never comes up at all: not installed
# yet (we're upstream of install-loracom.sh here), disabled, or failing before
# it gets as far as setting a name.
#
# Done before the install-*.sh calls on purpose: if one of them fails halfway,
# the board still has a unique, reachable name to debug it over.
#
# Skipped when cmdb-bootstrap.service is up, which is the re-run case on an
# already provisioned board: the name it's carrying is the real one, and
# renaming it to the baseline until the next reboot would only break whatever
# is currently reaching it under that name. The unit is oneshot with
# RemainAfterExit, so "active" means it ran and succeeded - a failed or
# never-installed unit is exactly the case this baseline exists for.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet cmdb-bootstrap.service 2>/dev/null; then
    echo "system-setup.sh: cmdb-bootstrap.service is up, leaving the hostname it set ($(hostname)) alone"
elif mac=$(cmdb_get_mac); then
    setup_hostname="$HOSTNAME_PREFIX-$mac"
    if cmdb_set_hostname "$setup_hostname"; then
        echo "system-setup.sh: hostname set to $setup_hostname (baseline - cmdb-bootstrap.service replaces it with the HAT-provided name at boot)"
    else
        echo "system-setup.sh: WARNING: failed to set hostname to $setup_hostname (ignored, not critical)" >&2
    fi
else
    echo "system-setup.sh: WARNING: no MAC address found, leaving the hostname as it is" >&2
fi

# install-utils.sh first: it brings in the toolchain install-loracom.sh
# compiles with.
"$SCRIPT_DIR/install-utils.sh"
"$SCRIPT_DIR/install-cam.sh"
"$SCRIPT_DIR/install-loracom.sh"

echo "system-setup.sh: done - reboot for the UART3 and camera overlays to take effect."
# Surfaced here because it is the one board-wide side effect of provisioning:
# the camera's out-of-tree modules only load against the kernel they were built
# for, so install-cam.sh pins it. Anyone reading only this script's output would
# otherwise not learn that the board no longer takes kernel updates.
if command -v apt-mark >/dev/null 2>&1 && [ -n "$(apt-mark showhold 2>/dev/null)" ]; then
    echo "system-setup.sh: kernel updates are held (see install-cam.sh) - 'apt-mark showhold' lists them"
fi
