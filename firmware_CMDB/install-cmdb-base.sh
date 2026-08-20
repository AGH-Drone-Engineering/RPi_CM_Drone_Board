#!/bin/sh
# What makes a board a CMDB, independent of the camera and the HAT tools:
# hardware identity, a name, and the MAVLink link to the flight controller.
#
# The hostname is only a baseline: bootstrap.sh replaces it at every boot with
# the HAT-provided name. It exists for the case where cmdb-bootstrap.service
# never comes up at all, so the board isn't left on the image default
# ("raspberrypi", which collides over mDNS as soon as there are two of them).
# A CMDB-* name on a running board therefore means it never reached the HAT.
#
# The FC hangs off GPIO14/15 (J_FC_UART on the IARC HAT), which is uart0 on Pi 5
# silicon - not the UART3 install-loracom.sh enables for the HAT itself.
# MAVProxy listens rather than connects (udpin), so any number of ground
# stations can attach to ports 14550/14551 without this knowing their addresses.
# It comes from pip, in install-utils.sh.
#
# The 5V rail is fed either by the onboard buck or by VBUS from the USB-C
# connector. Off the buck nothing negotiates a contract, so the firmware assumes
# the weakest supply and budgets 600mA for everything on USB together. 
# usb_max_current_enable below lifts that to 1.6A.
# PSU_MAX_CURRENT in the bootloader EEPROM would lift it too, but is left as default, 
# same so a USB-C supply really negotiate.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

: "${ENV_FILE:=/etc/environment}"

# Fixed per board variant; the per-unit identity comes from the HAT at boot.
HW_REVISION=1
HW_TYPE=STANDARD

HOSTNAME_PREFIX=CMDB

# Still named after system-setup.sh, which used to own this block: renaming it
# would orphan the old one on boards already provisioned, leaving two copies of
# CMDB_HARDWARE_* in the file.
MARKER_BEGIN="# BEGIN cmdb-system-setup"
MARKER_END="# END cmdb-system-setup"

FC_UART_DEVICE=/dev/ttyAMA0
FC_UART_BAUD=57600
MAVLINK_UDP_PORTS="14550 14551"
MAVLINK_SERVICE=cmdb-mavlink.service
MAVPROXY_STATE_DIR=/var/log/mavproxy

# One --out per port, built once and reused by the banner and the unit file.
MAVPROXY_OUTS=""
for mavlink_port in $MAVLINK_UDP_PORTS; do
    MAVPROXY_OUTS="$MAVPROXY_OUTS --out=udpin:0.0.0.0:$mavlink_port"
done

configtxt_init

cat <<EOF
install-cmdb-base.sh changes the following:
  * $ENV_FILE: CMDB_HARDWARE_REVISION, CMDB_HARDWARE_TYPE
  * hostname: $HOSTNAME_PREFIX-<mac>, until the HAT provides the real one
  * config.txt: uart0 enabled ($FC_UART_DEVICE, the FC link), usb_max_current_enable=1
  * $MAVLINK_SERVICE: MAVProxy bridging $FC_UART_DEVICE to udpin:0.0.0.0 on ports: $MAVLINK_UDP_PORTS
EOF

# ---------------------------------------------------------------------------
# 1. hardware identity
# ---------------------------------------------------------------------------

# Own block, separate from the one bootstrap.sh maintains in the same file.
echo "install-cmdb-base.sh: recording hardware identity in $ENV_FILE"

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
echo "install-cmdb-base.sh: CMDB_HARDWARE_REVISION=$HW_REVISION CMDB_HARDWARE_TYPE=$HW_TYPE"

# ---------------------------------------------------------------------------
# 2. baseline hostname
# ---------------------------------------------------------------------------

# Skipped when the service is up (oneshot + RemainAfterExit, so "active" means
# it ran): the name it set is the real one.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet cmdb-bootstrap.service 2>/dev/null; then
    echo "install-cmdb-base.sh: cmdb-bootstrap.service is up, leaving the hostname it set ($(hostname)) alone"
elif mac=$(cmdb_get_mac); then
    setup_hostname="$HOSTNAME_PREFIX-$mac"
    if cmdb_set_hostname "$setup_hostname"; then
        echo "install-cmdb-base.sh: hostname set to $setup_hostname"
    else
        echo "install-cmdb-base.sh: WARNING: failed to set hostname to $setup_hostname (ignored, not critical)" >&2
    fi
else
    echo "install-cmdb-base.sh: WARNING: no MAC address found, leaving the hostname as it is" >&2
fi

# ---------------------------------------------------------------------------
# 3. config.txt: FC UART and the USB current budget
# ---------------------------------------------------------------------------

# Older firmware only ships the non-suffixed overlay.
if [ -e "$OVERLAY_DIR/uart0-pi5.dtbo" ]; then
    FC_UART_OVERLAY=uart0-pi5
else
    FC_UART_OVERLAY=uart0
    echo "install-cmdb-base.sh: WARNING: $OVERLAY_DIR/uart0-pi5.dtbo missing, falling back to 'dtoverlay=uart0' - update your firmware if $FC_UART_DEVICE doesn't show up after the reboot" >&2
fi

# The disable regex kills any hand-added usb_max_current_enable: a stray =0
# somewhere later in the file would put the 600mA cap straight back.
echo "install-cmdb-base.sh: enabling uart0 ($FC_UART_OVERLAY) and lifting the USB current limit in $CONFIG_TXT"
configtxt_set_block cmdb-base '^[[:space:]]*usb_max_current_enable=' <<EOF
dtoverlay=$FC_UART_OVERLAY
usb_max_current_enable=1
EOF

# A login console on the same device would eat the MAVLink stream. Not the
# default on a CM5 (the console lives on ttyAMA10), so only touched if someone
# turned it on.
getty_unit="serial-getty@$(basename "$FC_UART_DEVICE").service"
if systemctl is-enabled --quiet "$getty_unit" 2>/dev/null || systemctl is-active --quiet "$getty_unit" 2>/dev/null; then
    echo "install-cmdb-base.sh: disabling $getty_unit, it would fight MAVProxy for $FC_UART_DEVICE"
    systemctl disable --now "$getty_unit" >/dev/null 2>&1 || :
fi

# ---------------------------------------------------------------------------
# 4. MAVLink bridge
# ---------------------------------------------------------------------------

# Written here rather than shipped as a file: the device, baud and port above
# are the single source of truth for them.
MAVPROXY_BIN=$(command -v mavproxy.py 2>/dev/null || echo /usr/local/bin/mavproxy.py)

mkdir -p "$MAVPROXY_STATE_DIR"

cat > "/etc/systemd/system/$MAVLINK_SERVICE" <<EOF
[Unit]
Description=MAVLink bridge between the flight controller and UDP clients
After=dev-$(basename "$FC_UART_DEVICE").device
Wants=dev-$(basename "$FC_UART_DEVICE").device

[Service]
# --daemon keeps MAVProxy off the console without forking, so systemd still
# tracks the process. --state-basedir keeps its tlogs out of the working
# directory, which would otherwise be /.
ExecStart=$MAVPROXY_BIN --master=$FC_UART_DEVICE,$FC_UART_BAUD$MAVPROXY_OUTS --daemon --state-basedir=$MAVPROXY_STATE_DIR
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
chmod 644 "/etc/systemd/system/$MAVLINK_SERVICE"

systemctl daemon-reload
systemctl enable "$MAVLINK_SERVICE"

echo "install-cmdb-base.sh: done"

if [ ! -x "$MAVPROXY_BIN" ]; then
    echo "install-cmdb-base.sh: NOTE: $MAVPROXY_BIN not found - run install-utils.sh (it pip-installs MAVProxy), otherwise $MAVLINK_SERVICE will fail on the next boot." >&2
fi

if [ ! -e "$FC_UART_DEVICE" ]; then
    echo "install-cmdb-base.sh: NOTE: $FC_UART_DEVICE does not exist yet - reboot for the uart0 overlay to take effect." >&2
fi
