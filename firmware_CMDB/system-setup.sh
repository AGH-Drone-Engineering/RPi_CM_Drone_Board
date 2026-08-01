#!/bin/sh
# One-shot provisioning for a fresh CM5 image: records what this board is, gives
# it a name, then runs the three install-*.sh scripts, which do the actual work
# and are each usable on their own. Idempotent - re-running it is a no-op except
# for package upgrades. A reboot is required afterwards for config.txt.
#
# The hostname is only a baseline: bootstrap.sh replaces it at every boot with
# the HAT-provided name. It exists for the case where cmdb-bootstrap.service
# never comes up at all, so the board isn't left on the image default
# ("raspberrypi", which collides over mDNS as soon as there are two of them).
# A CMDB-* name on a running board therefore means it never reached the HAT.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

: "${ENV_FILE:=/etc/environment}"

# Fixed per board variant; the per-unit identity comes from the HAT at boot.
HW_REVISION=1
HW_TYPE=STANDARD

HOSTNAME_PREFIX=CMDB

MARKER_BEGIN="# BEGIN cmdb-system-setup"
MARKER_END="# END cmdb-system-setup"

cat <<EOF
system-setup.sh changes the following:
  * $ENV_FILE: CMDB_HARDWARE_REVISION, CMDB_HARDWARE_TYPE
  * hostname: $HOSTNAME_PREFIX-<mac>, until the HAT provides the real one
  * everything install-utils.sh, install-cam.sh and install-loracom.sh change
EOF

# ---------------------------------------------------------------------------
# 1. hardware identity
# ---------------------------------------------------------------------------

# Own block, separate from the one bootstrap.sh maintains in the same file.
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

# ---------------------------------------------------------------------------
# 2. baseline hostname
# ---------------------------------------------------------------------------

# Before the installs, so a board still has a reachable name if one fails
# halfway. Skipped when the service is up (oneshot + RemainAfterExit, so
# "active" means it ran): the name it set is the real one.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet cmdb-bootstrap.service 2>/dev/null; then
    echo "system-setup.sh: cmdb-bootstrap.service is up, leaving the hostname it set ($(hostname)) alone"
elif mac=$(cmdb_get_mac); then
    setup_hostname="$HOSTNAME_PREFIX-$mac"
    if cmdb_set_hostname "$setup_hostname"; then
        echo "system-setup.sh: hostname set to $setup_hostname"
    else
        echo "system-setup.sh: WARNING: failed to set hostname to $setup_hostname (ignored, not critical)" >&2
    fi
else
    echo "system-setup.sh: WARNING: no MAC address found, leaving the hostname as it is" >&2
fi

# ---------------------------------------------------------------------------
# 3. install scripts
# ---------------------------------------------------------------------------

# utils first: it brings in the toolchain install-loracom.sh compiles with.
"$SCRIPT_DIR/install-utils.sh"
"$SCRIPT_DIR/install-cam.sh"
"$SCRIPT_DIR/install-loracom.sh"

echo "system-setup.sh: done - reboot for the UART3 and camera overlays to take effect."

if command -v apt-mark >/dev/null 2>&1 && [ -n "$(apt-mark showhold 2>/dev/null)" ]; then
    echo "system-setup.sh: kernel updates are held (see install-cam.sh) - 'apt-mark showhold' lists them"
fi
