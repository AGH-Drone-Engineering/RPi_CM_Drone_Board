#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "install.sh must be run as root" >&2
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
INSTALL_DIR=/opt/cmdb/bin

if [ ! -x "$SCRIPT_DIR/loracom/bin/loracom" ]; then
    make -C "$SCRIPT_DIR/loracom"
fi

mkdir -p "$INSTALL_DIR"
install -m 755 "$SCRIPT_DIR/loracom/bin/loracom" "$INSTALL_DIR/loracom"
install -m 755 "$SCRIPT_DIR/bootstrap/bootstrap.sh" "$INSTALL_DIR/bootstrap.sh"

cat > /etc/profile.d/cmdb.sh <<EOF
export PATH="$INSTALL_DIR:\$PATH"
EOF
chmod 644 /etc/profile.d/cmdb.sh

install -m 644 "$SCRIPT_DIR/bootstrap/cmdb-bootstrap.service" /etc/systemd/system/cmdb-bootstrap.service
systemctl daemon-reload
systemctl enable cmdb-bootstrap.service

echo "Installed: $INSTALL_DIR/{loracom,bootstrap.sh}, $INSTALL_DIR on PATH via /etc/profile.d/cmdb.sh, cmdb-bootstrap.service enabled (runs on next boot)."
