#!/bin/sh
# Enables the UART the HAT is wired to, then builds and deploys the CMDB tools
# (loracom + the boot-time config service). Needs a toolchain, so run
# install-utils.sh first - or just system-setup.sh, which sequences both.
#
# The CM5 is Pi 5 silicon, where each PL011 has its own overlay and UART3 is the
# one wired to GPIO8 (TX) / GPIO9 (RX).
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

INSTALL_DIR=/opt/cmdb/bin

: "${UART_DEVICE:=/dev/ttyAMA3}"

cat <<EOF
install-loracom.sh changes the following:
  * config.txt: UART3 enabled ($UART_DEVICE)
  * $INSTALL_DIR: loracom, bootstrap.sh
  * /usr/local/bin/loracom: symlink, and $INSTALL_DIR on PATH via /etc/profile.d/cmdb.sh
  * cmdb-bootstrap.service: installed and enabled, runs on next boot
EOF

configtxt_init

# ---------------------------------------------------------------------------
# 1. UART3 overlay
# ---------------------------------------------------------------------------

# Older firmware only ships the non-suffixed overlay.
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

# ---------------------------------------------------------------------------
# 2. build
# ---------------------------------------------------------------------------

# As the invoking user: obj/ and bin/ live in the working tree, and root-owned
# objects there break every subsequent non-root make.
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

# ---------------------------------------------------------------------------
# 3. install
# ---------------------------------------------------------------------------

mkdir -p "$INSTALL_DIR"
install -m 755 "$SCRIPT_DIR/loracom/bin/loracom" "$INSTALL_DIR/loracom"
install -m 755 "$SCRIPT_DIR/bootstrap/bootstrap.sh" "$INSTALL_DIR/bootstrap.sh"

# profile.d only covers login shells; systemd units, cron and `ssh board loracom`
# never read it.
mkdir -p /usr/local/bin
ln -sfn "$INSTALL_DIR/loracom" /usr/local/bin/loracom

cat > /etc/profile.d/cmdb.sh <<EOF
export PATH="$INSTALL_DIR:\$PATH"
EOF
chmod 644 /etc/profile.d/cmdb.sh

# ---------------------------------------------------------------------------
# 4. bootstrap service
# ---------------------------------------------------------------------------

install -m 644 "$SCRIPT_DIR/bootstrap/cmdb-bootstrap.service" /etc/systemd/system/cmdb-bootstrap.service
systemctl daemon-reload
systemctl enable cmdb-bootstrap.service

echo "install-loracom.sh: done"

if [ ! -e "$UART_DEVICE" ]; then
    echo "install-loracom.sh: NOTE: $UART_DEVICE does not exist yet - reboot for the UART3 overlay to take effect, otherwise cmdb-bootstrap.service will fail on the next boot." >&2
fi
