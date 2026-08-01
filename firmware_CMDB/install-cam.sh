#!/bin/sh
# FRAMOS FSM:GO IMX900 on the CAM0 connector.
#
# The stock Raspberry Pi camera stack does not know this sensor, so all three
# layers are replaced: the out-of-tree FRAMOS kernel modules and their overlays
# (sources vendored in framos-rpi-drivers/, patched - see its PATCHES.md), a
# framos-libcamera + rpicam-apps build in /usr/local, and the dtoverlay line in
# config.txt.
#
# Layers 1 and 2 ship prebuilt in cam-prebuilt/, so the normal install copies
# files and takes seconds. Both halves are pinned in its manifest - the modules
# to one exact kernel (vermagic and modversion CRCs), the /usr/local payload to
# one OS release (sonames like libavcodec61) - and each independently falls back
# to building from source when the running system does not match. That is what
# lets a board be provisioned from an image whose kernel is not the one the
# payload was built on.
#
# The fallback does not protect a board that is already set up: nothing re-runs
# this at boot, so an apt kernel upgrade would leave the modules behind in the
# old /lib/modules tree and the sensor would silently stop probing after the
# next reboot. Step 1c holds the kernel *meta*-packages to stop that (the
# versioned ones never upgrade in place, so holding those would do nothing).
# Only apt is blocked - rpi-update and reflashing bypass it.
#
#   CMDB_CAM_FROM_SOURCE=1    ignore cam-prebuilt/ entirely and build
#   CMDB_REBUILD_LIBCAMERA=1  reinstall layer 2 even if it is already there
#   CMDB_HOLD_KERNEL=0        do not hold the kernel packages
#
# Run cam-prebuilt/make-prebuilt.sh to refresh the payload after a build.
set -eu

export DEBIAN_FRONTEND=noninteractive

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

DRIVERS_DIR=$SCRIPT_DIR/framos-rpi-drivers
PREBUILT_DIR=$SCRIPT_DIR/cam-prebuilt

# The branch FRAMOS pins to this RPi OS release - do not track the default one.
LIBCAMERA_URL=https://github.com/framosimaging/framos-libcamera.git
LIBCAMERA_BRANCH=framos_v0.5.2+rpt20250903
LIBCAMERA_WORK=/opt/framos-src

# Stands in for "layer 2 is installed", being the sensor-specific part of it.
LIBCAMERA_MARKER=/usr/local/share/libcamera/ipa/rpi/pisp/fr_imx900.json

KVER=$(uname -r)
KARCH=$(uname -m)
MODULE_DIR=/lib/modules/$KVER/updates/drivers

configtxt_init

cat <<EOF
install-cam.sh changes the following:
  * $MODULE_DIR: FRAMOS sensor + GMSL kernel modules
  * $OVERLAY_DIR: fr_*.dtbo overlays
  * /usr/local: framos-libcamera + rpicam-apps, replacing the distro rpicam-apps
  * apt: runtime libraries for them, and the kernel meta-packages held
  * config.txt: camera_auto_detect=0, dtoverlay=fr_imx900,cam0
EOF

# Takes the *unexpanded* glob: an unmatched one expands to itself, so $1 is then
# a literal that does not exist.
have_glob() {
    [ -e "$1" ]
}

# Anchored on the trailing "ok installed": the first word is the wanted state,
# which becomes "hold" once hold_kernel has run.
pkg_installed() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'ok installed$'
}

if [ "${CMDB_CAM_FROM_SOURCE:-0}" = 1 ]; then
    echo "install-cam.sh: CMDB_CAM_FROM_SOURCE=1, building everything from source"
    USE_PAYLOAD=0
else
    USE_PAYLOAD=1
fi

# Set by the manifest; defaulted for the case where it is absent, since the
# comparisons below run under set -u either way.
PREBUILT_KERNEL=
PREBUILT_ARCH=
PREBUILT_OS_ID=
PREBUILT_OS_VERSION_ID=
PREBUILT_RUNTIME_PACKAGES=
if [ "$USE_PAYLOAD" = 1 ] && [ -f "$PREBUILT_DIR/manifest" ]; then
    . "$PREBUILT_DIR/manifest"
fi

# ---------------------------------------------------------------------------
# 1a. kernel modules
# ---------------------------------------------------------------------------

use_prebuilt_modules=0
if [ "$USE_PAYLOAD" != 1 ]; then
    :  # already announced
elif ! have_glob "$PREBUILT_DIR"/modules/fr_*.ko.xz; then
    echo "install-cam.sh: no prebuilt modules in $PREBUILT_DIR, building from source"
elif [ "$PREBUILT_KERNEL" != "$KVER" ] || [ "$PREBUILT_ARCH" != "$KARCH" ]; then
    echo "install-cam.sh: prebuilt modules are for ${PREBUILT_KERNEL:-?} ${PREBUILT_ARCH:-?}, running $KVER $KARCH - building from source"
else
    use_prebuilt_modules=1
fi

if [ "$use_prebuilt_modules" = 1 ]; then
    echo "install-cam.sh: installing prebuilt kernel modules for $KVER"
    mkdir -p "$MODULE_DIR"
    cp -f "$PREBUILT_DIR"/modules/fr_*.ko.xz "$MODULE_DIR/"
else
    if [ ! -d "$DRIVERS_DIR" ]; then
        echo "install-cam.sh: $DRIVERS_DIR is missing and the prebuilt payload is unusable - nothing left to install from" >&2
        exit 1
    fi

    KDIR=/lib/modules/$KVER/build
    if [ ! -d "$KDIR" ]; then
        echo "install-cam.sh: no kernel build tree at $KDIR - install the headers for $KVER first (install-utils.sh does this)" >&2
        exit 1
    fi

    echo "install-cam.sh: building FRAMOS kernel modules against $KVER"
    make -C "$DRIVERS_DIR" modules

    # Not the repo's own modules_install target (it shells out to sudo).
    # INSTALL_MOD_DIR is pinned to match the prebuilt path - kbuild defaults it
    # with ?=, and two copies in different directories let depmod pick.
    echo "install-cam.sh: installing kernel modules"
    make -C "$KDIR" M="$DRIVERS_DIR" INSTALL_MOD_DIR=updates modules_install
fi

depmod -a "$KVER"

# ---------------------------------------------------------------------------
# 1b. device tree overlays
# ---------------------------------------------------------------------------

# Plain DTBs, so nothing pins them - used whenever present, even on the source
# path. Chosen independently of the modules, hence the second $DRIVERS_DIR check.
if [ "$USE_PAYLOAD" = 1 ] && have_glob "$PREBUILT_DIR"/overlays/fr_*.dtbo; then
    echo "install-cam.sh: installing prebuilt overlays into $OVERLAY_DIR"
    cp -f "$PREBUILT_DIR"/overlays/fr_*.dtbo "$OVERLAY_DIR/"
else
    if [ ! -d "$DRIVERS_DIR" ]; then
        echo "install-cam.sh: no prebuilt overlays and no $DRIVERS_DIR to compile them from" >&2
        exit 1
    fi
    if ! command -v dtc >/dev/null 2>&1; then
        echo "install-cam.sh: installing device-tree-compiler"
        apt-get install -y device-tree-compiler
    fi
    echo "install-cam.sh: compiling overlays"
    make -C "$DRIVERS_DIR" dtbs
    cp -f "$DRIVERS_DIR"/overlays/fr_*.dtbo "$OVERLAY_DIR/"
fi

# ---------------------------------------------------------------------------
# 1c. pin the kernel the modules were built for
# ---------------------------------------------------------------------------

# Headers metas too: otherwise a board can end up with headers for a kernel it
# is not running, which is the tree the source fallback builds against.
KERNEL_HOLD_PACKAGES="linux-image-rpi-2712 linux-image-rpi-v8
    linux-headers-rpi-2712 linux-headers-rpi-v8
    raspberrypi-kernel raspberrypi-kernel-headers"

hold_kernel() {
    if ! command -v apt-mark >/dev/null 2>&1; then
        echo "install-cam.sh: WARNING: apt-mark not found, leaving the kernel unpinned" >&2
        return
    fi

    # Which of these exist depends on the image - the -2712/-v8 split is recent.
    held=
    for pkg in $KERNEL_HOLD_PACKAGES; do
        if pkg_installed "$pkg"; then
            held="$held $pkg"
        fi
    done

    if [ -z "$held" ]; then
        echo "install-cam.sh: WARNING: no known kernel meta-package installed, leaving the kernel unpinned - an apt kernel upgrade will stop the sensor probing" >&2
        return
    fi

    # shellcheck disable=SC2086  # deliberate word splitting into separate packages
    apt-mark hold $held >/dev/null

    echo "install-cam.sh: kernel held at $KVER"
    echo "install-cam.sh:   held:$held"
    echo "install-cam.sh:   to move to a newer kernel deliberately: apt-mark unhold$held"
    echo "install-cam.sh:   then apt upgrade, reboot, re-run this script (rebuilds the modules), and"
    echo "install-cam.sh:   cam-prebuilt/make-prebuilt.sh if the new kernel is the one to ship."
}

if [ "${CMDB_HOLD_KERNEL:-1}" = 1 ]; then
    hold_kernel
else
    echo "install-cam.sh: CMDB_HOLD_KERNEL=0, leaving the kernel unpinned - an apt kernel upgrade will stop the sensor probing until this script is re-run"
fi

# ---------------------------------------------------------------------------
# 2. framos-libcamera + rpicam-apps
# ---------------------------------------------------------------------------

install_prebuilt_userspace() {
    echo "install-cam.sh: installing prebuilt libcamera + rpicam-apps"

    # Release-specific, so the list travels in the manifest rather than here.
    if [ -n "$PREBUILT_RUNTIME_PACKAGES" ]; then
        apt-get update
        # shellcheck disable=SC2086  # deliberate word splitting into separate packages
        apt-get install -y $PREBUILT_RUNTIME_PACKAGES
    else
        echo "install-cam.sh: WARNING: payload predates PREBUILT_RUNTIME_PACKAGES, not checking its library dependencies - regenerate it with cam-prebuilt/make-prebuilt.sh" >&2
    fi

    # The distro rpicam-* are built against distro libcamera, which knows nothing
    # about the IMX900.
    if pkg_installed rpicam-apps; then
        echo "install-cam.sh: removing the distro rpicam-apps package"
        apt-get remove --purge -y rpicam-apps
    fi

    tar xzf "$PREBUILT_DIR/usrlocal.tar.gz" -C /usr/local --no-same-owner
    ldconfig
}

build_userspace_from_source() {
    echo "install-cam.sh: building framos-libcamera and rpicam-apps - this takes a while"

    # install_libcamera.sh clones rpicam-apps next to its own checkout, and its
    # clone fails on a leftover tree without checking.
    rm -rf "$LIBCAMERA_WORK"
    mkdir -p "$LIBCAMERA_WORK"

    if [ -d "$SCRIPT_DIR/framos-libcamera" ]; then
        echo "install-cam.sh: using local framos-libcamera checkout"
        cp -a "$SCRIPT_DIR/framos-libcamera" "$LIBCAMERA_WORK/"
    else
        echo "install-cam.sh: cloning framos-libcamera ($LIBCAMERA_BRANCH)"
        git clone --depth 1 -b "$LIBCAMERA_BRANCH" "$LIBCAMERA_URL" "$LIBCAMERA_WORK/framos-libcamera"
    fi

    # Pulls its own build deps, purges the distro rpicam-apps, installs both
    # trees into /usr/local, runs ldconfig.
    ( cd "$LIBCAMERA_WORK/framos-libcamera" && ./install_libcamera.sh )
}

if [ -e "$LIBCAMERA_MARKER" ] && [ "${CMDB_REBUILD_LIBCAMERA:-0}" != 1 ]; then
    echo "install-cam.sh: libcamera + rpicam-apps already installed, skipping (CMDB_REBUILD_LIBCAMERA=1 to reinstall)"
else
    . /etc/os-release
    : "${ID:=}" "${VERSION_ID:=}"  # absent on rolling images, and set -u is on

    if [ "$USE_PAYLOAD" != 1 ]; then
        build_userspace_from_source
    elif [ ! -f "$PREBUILT_DIR/usrlocal.tar.gz" ]; then
        echo "install-cam.sh: no prebuilt userspace payload, building from source"
        build_userspace_from_source
    elif [ "$PREBUILT_OS_ID" != "$ID" ] ||
         [ "$PREBUILT_OS_VERSION_ID" != "$VERSION_ID" ] ||
         [ "$PREBUILT_ARCH" != "$KARCH" ]; then
        echo "install-cam.sh: prebuilt userspace is for ${PREBUILT_OS_ID:-?} ${PREBUILT_OS_VERSION_ID:-?} ${PREBUILT_ARCH:-?}, running $ID $VERSION_ID $KARCH"
        build_userspace_from_source
    else
        install_prebuilt_userspace
    fi

    # install_libcamera.sh has no `set -e` and exits 0 even when meson dies.
    if [ ! -e "$LIBCAMERA_MARKER" ]; then
        echo "install-cam.sh: install finished but $LIBCAMERA_MARKER is missing - it failed" >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# 3. config.txt
# ---------------------------------------------------------------------------

# The disable regex kills camera_auto_detect (it probes for Pi sensors on the
# same CSI port and fights the explicit overlay) and any hand-added fr_imx900.
echo "install-cam.sh: enabling fr_imx900 on CAM0 in $CONFIG_TXT"
configtxt_set_block cmdb-cam '^[[:space:]]*(camera_auto_detect=|dtoverlay=(fr_)?imx900)' <<'EOF'
camera_auto_detect=0
dtoverlay=fr_imx900,cam0
EOF

echo "install-cam.sh: done - reboot, then check with 'rpicam-hello --list-cameras'."
