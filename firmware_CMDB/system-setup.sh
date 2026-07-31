#!/bin/sh
# One-shot provisioning for a fresh CM5 image.
#
# Records what this board is, then hands off to the install-*.sh scripts, which
# do the actual work and are each usable on their own:
#   install-utils.sh    apt/pip packages, VNC, Claude Code
#   install-cam.sh      IMX900 on CAM0
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

# install-utils.sh first: it brings in the toolchain install-loracom.sh
# compiles with.
"$SCRIPT_DIR/install-utils.sh"
"$SCRIPT_DIR/install-cam.sh"
"$SCRIPT_DIR/install-loracom.sh"

echo "system-setup.sh: done - reboot for the UART3 and camera overlays to take effect."
