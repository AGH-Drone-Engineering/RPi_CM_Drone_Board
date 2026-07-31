#!/bin/sh
# Enables the IMX900 camera on the CAM0 connector.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

configtxt_init

if [ ! -e "$OVERLAY_DIR/imx900.dtbo" ]; then
    echo "install-cam.sh: WARNING: $OVERLAY_DIR/imx900.dtbo missing - the overlay will be written to config.txt anyway, but the sensor won't probe until the firmware ships it (sudo rpi-update)" >&2
fi

# camera_auto_detect and an explicit sensor overlay fight over the same CSI
# port, so any autodetect the image shipped with gets commented out.
echo "install-cam.sh: enabling IMX900 on CAM0 in $CONFIG_TXT"
configtxt_set_block cmdb-cam '^[[:space:]]*camera_auto_detect=' <<'EOF'
camera_auto_detect=0
dtoverlay=imx900,cam0
EOF

echo "install-cam.sh: done - reboot for the camera overlay to take effect."
