#!/bin/sh
# Enables the UART the HAT is wired to, then builds and deploys the CMDB tools
# (loracom + the boot-time config service). Needs a toolchain, so run
# install-utils.sh first - or just system-setup.sh, which sequences both.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

INSTALL_DIR=/opt/cmdb/bin

: "${UART_DEVICE:=/dev/ttyAMA3}"

configtxt_init

# The CM5 is Pi 5 silicon, where each PL011 has its own overlay and UART3 is
# the one wired to GPIO8 (TX) / GPIO9 (RX). Older firmware only ships the
# non-suffixed overlay, so pick whichever is actually present.
if [ -e "$OVERLAY_DIR/uart3-pi5.dtbo" ]; then
    UART_OVERLAY=uart3-pi5
else
    UART_OVERLAY=uart3
    echo "install-loracom.sh: WARNING: $OVERLAY_DIR/uart3-pi5.dtbo missing, falling back to 'dtoverlay=uart3' - update your firmware if $UART_DEVICE doesn't show up after the reboot" >&2
fi

echo "install-loracom.sh: enabling UART3 ($UART_OVERLAY) in $CONFIG_TXT"
configtxt_set_block cmdb-loracom <<EOF
dtoverlay=$UART_OVERLAY
EOF

# Always run make - it's incremental, and skipping it when a binary happens to
# exist installs a stale one built from older sources.
#
# Built as the invoking user, not as root: obj/ and bin/ live in the working
# tree, and root-owned objects there make every subsequent non-root `make` fail
# on them ("cannot create obj/version.h.tmp: Permission denied").
BUILD_USER=${SUDO_USER:-}
if [ -z "$BUILD_USER" ] || [ "$BUILD_USER" = root ]; then
    make -C "$SCRIPT_DIR/loracom"
elif command -v runuser >/dev/null 2>&1; then
    echo "install-loracom.sh: building as $BUILD_USER"
    runuser -u "$BUILD_USER" -- make -C "$SCRIPT_DIR/loracom"
else
    echo "install-loracom.sh: building as $BUILD_USER"
    su "$BUILD_USER" -s /bin/sh -c "make -C \"$SCRIPT_DIR/loracom\""
fi

mkdir -p "$INSTALL_DIR"
install -m 755 "$SCRIPT_DIR/loracom/bin/loracom" "$INSTALL_DIR/loracom"
install -m 755 "$SCRIPT_DIR/bootstrap/bootstrap.sh" "$INSTALL_DIR/bootstrap.sh"

# /etc/profile.d only covers login shells. Systemd services, cron, and
# `ssh board loracom --get` never read it, and neither does bootstrap.sh's
# CMDB_POST_INIT_CMD - so loracom also goes somewhere that is on the default
# PATH of all of them.
mkdir -p /usr/local/bin
ln -sfn "$INSTALL_DIR/loracom" /usr/local/bin/loracom

cat > /etc/profile.d/cmdb.sh <<EOF
export PATH="$INSTALL_DIR:\$PATH"
EOF
chmod 644 /etc/profile.d/cmdb.sh

install -m 644 "$SCRIPT_DIR/bootstrap/cmdb-bootstrap.service" /etc/systemd/system/cmdb-bootstrap.service
systemctl daemon-reload
systemctl enable cmdb-bootstrap.service

echo "install-loracom.sh: installed $INSTALL_DIR/{loracom,bootstrap.sh}, symlinked /usr/local/bin/loracom, $INSTALL_DIR on PATH via /etc/profile.d/cmdb.sh, cmdb-bootstrap.service enabled (runs on next boot)."

# On a first install the overlay above hasn't taken effect yet, so the device
# is legitimately missing - flag it rather than letting the service we just
# enabled fail at boot on a machine nobody is watching.
if [ ! -e "$UART_DEVICE" ]; then
    echo "install-loracom.sh: NOTE: $UART_DEVICE does not exist yet - reboot for the UART3 overlay to take effect, otherwise cmdb-bootstrap.service will fail on the next boot." >&2
fi
