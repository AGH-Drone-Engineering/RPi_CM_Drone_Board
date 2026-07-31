#!/bin/sh
# FRAMOS FSM:GO IMX900 on the CAM0 connector.
#
# The stock Raspberry Pi camera stack does not know this sensor at all, so all
# three layers have to be replaced:
#
#   1. kernel    - the FRAMOS out-of-tree sensor + GMSL modules and their device
#                  tree overlays. Sources live in framos-rpi-drivers/, vendored
#                  here *patched*: see its PATCHES.md, upstream hides three of
#                  the five IMX900 modes and exposes the rest under the wrong
#                  Bayer order.
#   2. userspace - framos-libcamera, a libcamera fork carrying the IMX900
#                  CamHelper and tuning files, plus rpicam-apps rebuilt against
#                  it, both installed into /usr/local. Replaces the distro
#                  rpicam-apps package.
#   3. firmware  - the dtoverlay line in config.txt.
#
# Layers 1 and 2 ship prebuilt in cam-prebuilt/, so the normal install copies
# files and takes seconds. Both halves are pinned - the modules to one exact
# kernel, the /usr/local payload to one OS release - so each is checked against
# the running system and falls back to building from source when it does not
# match. The fallback is what lets a board be provisioned from an image whose
# kernel is not the one the payload was built on.
#
# The fallback is not protection against a kernel upgrade on a board that is
# already set up - nothing re-runs this script at boot. Holding the kernel is
# what covers that, see step 1c for why and for what it does not cover.
#
#   CMDB_CAM_FROM_SOURCE=1    ignore cam-prebuilt/ entirely and build
#   CMDB_REBUILD_LIBCAMERA=1  reinstall layer 2 even if it is already there
#   CMDB_HOLD_KERNEL=0        do not hold the kernel packages
#
# Run cam-prebuilt/make-prebuilt.sh to refresh the payload after a build.
set -eu

# Every apt call below is non-interactive; set once rather than per invocation.
export DEBIAN_FRONTEND=noninteractive

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

DRIVERS_DIR=$SCRIPT_DIR/framos-rpi-drivers
PREBUILT_DIR=$SCRIPT_DIR/cam-prebuilt

# framos-libcamera is a whole libcamera fork (16 MB of sources) and we carry no
# patches against it, so the source fallback fetches it. The branch is the one
# FRAMOS pins to this RPi OS release - do not track the default branch.
LIBCAMERA_URL=https://github.com/framosimaging/framos-libcamera.git
LIBCAMERA_BRANCH=framos_v0.5.2+rpt20250903
LIBCAMERA_WORK=/opt/framos-src

# Written by framos-libcamera's install script and present in the prebuilt
# payload; stands in for "layer 2 is installed", being the sensor-specific
# part of it.
LIBCAMERA_MARKER=/usr/local/share/libcamera/ipa/rpi/pisp/fr_imx900.json

KVER=$(uname -r)
KARCH=$(uname -m)
MODULE_DIR=/lib/modules/$KVER/updates/drivers

configtxt_init

# True when a glob matched something. An unmatched glob expands to itself, so
# the first word is then a literal that does not exist - which is the whole
# test. Written as a function because the components below check for their own
# files independently, and because `[ -d dir ]` is the wrong question: a
# half-populated payload directory would pass it and fail later on cp.
have_glob() {
    [ -e "$1" ]
}

pkg_installed() {
    # Anchored on the trailing "ok installed", not the whole status: the first
    # word is the *wanted* state, which is "install" normally but "hold" once
    # hold_kernel has run. Matching "install ok installed" would make every
    # re-run believe the kernel packages are not installed.
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'ok installed$'
}

# Whether to consider cam-prebuilt/ at all. Resolved once: the components below
# each decide whether the payload *fits* them, and none of them should also
# have to remember the policy override.
if [ "${CMDB_CAM_FROM_SOURCE:-0}" = 1 ]; then
    echo "install-cam.sh: CMDB_CAM_FROM_SOURCE=1, building everything from source"
    USE_PAYLOAD=0
else
    USE_PAYLOAD=1
fi

# Recorded by make-prebuilt.sh. Defaulted first: absent when the payload was
# never generated or was deliberately dropped from the checkout, and the
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
    # The usual reason to land here is an apt kernel upgrade. Prebuilt modules
    # carry a vermagic and modversions CRCs tied to one kernel build; loading
    # them under another fails outright rather than misbehaving, so rebuilding
    # is the only correct answer.
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

    # The repo's own modules_install target shells out to sudo, which is
    # pointless and one more dependency when we are already root.
    #
    # INSTALL_MOD_DIR is pinned so this lands in the same place the prebuilt
    # path writes to. Debian's kbuild already defaults it to updates (upstream
    # says extra), but it does so with ?=, and the two paths quietly writing to
    # different directories would leave both copies installed with depmod
    # picking the winner - a forced source rebuild would then appear to do
    # nothing.
    echo "install-cam.sh: installing kernel modules"
    make -C "$KDIR" M="$DRIVERS_DIR" INSTALL_MOD_DIR=updates modules_install
fi

depmod -a "$KVER"

# ---------------------------------------------------------------------------
# 1b. device tree overlays
# ---------------------------------------------------------------------------

# Overlays are plain compiled DTBs - no vermagic, no libc, nothing to pin them
# to a kernel or an OS release - so the prebuilt ones are used whenever they
# exist, even on the source path. Only their absence pulls in dtc.
if [ "$USE_PAYLOAD" = 1 ] && have_glob "$PREBUILT_DIR"/overlays/fr_*.dtbo; then
    echo "install-cam.sh: installing prebuilt overlays into $OVERLAY_DIR"
    cp -f "$PREBUILT_DIR"/overlays/fr_*.dtbo "$OVERLAY_DIR/"
else
    # Reachable with the prebuilt modules already installed - overlays and
    # modules are chosen independently - so the sources have to be checked for
    # here too, not only in the modules branch above.
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

# The modules just installed sit in /lib/modules/<running kernel>/updates and
# carry a vermagic tied to that exact build. An upgraded kernel gets its own
# empty module directory alongside, so after the next reboot the sensor simply
# does not probe - and nothing in the logs points at apt as the cause. Held
# here rather than left to the operator because the failure is silent, delayed,
# and shows up as "the camera broke", not as "the kernel moved".
#
# What gets held are the *meta* packages. The versioned ones
# (linux-image-6.18.34+rpt-rpi-2712) never upgrade in place - every kernel is
# its own package - so holding those would accomplish nothing. What carries a
# board forward is the meta's Depends flipping to the next version, and that is
# what a hold on the meta stops. Note this only blocks apt: rpi-update pulls
# kernels straight from GitHub and reflashing the image obviously bypasses it.
#
# The headers metas go on hold too, so that a board whose headers came from the
# meta rather than from install-utils.sh's versioned pick cannot end up with
# headers for a kernel it is not running - which is the tree the source fallback
# builds against.
KERNEL_HOLD_PACKAGES="linux-image-rpi-2712 linux-image-rpi-v8
    linux-headers-rpi-2712 linux-headers-rpi-v8
    raspberrypi-kernel raspberrypi-kernel-headers"

hold_kernel() {
    if ! command -v apt-mark >/dev/null 2>&1; then
        echo "install-cam.sh: WARNING: apt-mark not found, leaving the kernel unpinned" >&2
        return
    fi

    # Which of these exist depends on the image - the -2712/-v8 split is recent,
    # older RPi OS releases carried a single raspberrypi-kernel. Hold whichever
    # are actually installed and say nothing about the rest.
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

    # apt-mark accepts an already-held package silently, so re-running is a no-op.
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

    # Runtime libraries only - no -dev packages, no meson/ninja/cmake. This is
    # the whole point of the prebuilt path. The list is a property of the
    # tarball, so it travels in the manifest rather than being maintained here:
    # the names are release-specific (libavcodec61, libqt5core5t64, ...) and a
    # hand-kept copy would go stale exactly when the payload is regenerated on
    # a new Debian release - the one case the manifest check would wave through.
    if [ -n "$PREBUILT_RUNTIME_PACKAGES" ]; then
        apt-get update
        # shellcheck disable=SC2086  # deliberate word splitting into separate packages
        apt-get install -y $PREBUILT_RUNTIME_PACKAGES
    else
        echo "install-cam.sh: WARNING: payload predates PREBUILT_RUNTIME_PACKAGES, not checking its library dependencies - regenerate it with cam-prebuilt/make-prebuilt.sh" >&2
    fi

    # The distro package installs its own rpicam-* into /usr/bin, built against
    # distro libcamera, which knows nothing about the IMX900. Purged for parity
    # with what framos-libcamera's own installer does on the source path.
    if pkg_installed rpicam-apps; then
        echo "install-cam.sh: removing the distro rpicam-apps package"
        apt-get remove --purge -y rpicam-apps
    fi

    # --no-same-owner is belt and braces: the archive already records root:root,
    # but a payload harvested some other way must not be able to hand parts of
    # /usr/local to whatever uid happened to be baked into it.
    tar xzf "$PREBUILT_DIR/usrlocal.tar.gz" -C /usr/local --no-same-owner
    ldconfig
}

build_userspace_from_source() {
    echo "install-cam.sh: building framos-libcamera and rpicam-apps - this takes a while"

    # install_libcamera.sh clones rpicam-apps next to its own checkout and
    # copies both into /opt/framos, so it needs a scratch parent directory of
    # its own. Wiped first: the clone it does fails on a leftover tree, and it
    # does not check.
    rm -rf "$LIBCAMERA_WORK"
    mkdir -p "$LIBCAMERA_WORK"

    if [ -d "$SCRIPT_DIR/framos-libcamera" ]; then
        # Honoured if someone drops a checkout next to this script - lets the
        # source path run without reaching GitHub.
        echo "install-cam.sh: using local framos-libcamera checkout"
        cp -a "$SCRIPT_DIR/framos-libcamera" "$LIBCAMERA_WORK/"
    else
        echo "install-cam.sh: cloning framos-libcamera ($LIBCAMERA_BRANCH)"
        git clone --depth 1 -b "$LIBCAMERA_BRANCH" "$LIBCAMERA_URL" "$LIBCAMERA_WORK/framos-libcamera"
    fi

    # Pulls its own build dependencies, purges the distro rpicam-apps package,
    # installs both trees into /usr/local and runs ldconfig.
    ( cd "$LIBCAMERA_WORK/framos-libcamera" && ./install_libcamera.sh )
}

if [ -e "$LIBCAMERA_MARKER" ] && [ "${CMDB_REBUILD_LIBCAMERA:-0}" != 1 ]; then
    echo "install-cam.sh: libcamera + rpicam-apps already installed, skipping (CMDB_REBUILD_LIBCAMERA=1 to reinstall)"
else
    . /etc/os-release

    # Only ID is mandatory in the os-release spec; VERSION_ID is absent on
    # rolling images (Debian testing/sid). Defaulted rather than left unset, or
    # set -u would abort on the comparison below - on exactly the images whose
    # mismatch it is there to detect and route to the source build.
    : "${ID:=}" "${VERSION_ID:=}"

    # Same flat shape as the modules cascade above: each rejection reason states
    # itself once, rather than the acceptance test being written here and its
    # negation repeated inside the else to work out what to print.
    if [ "$USE_PAYLOAD" != 1 ]; then
        build_userspace_from_source
    elif [ ! -f "$PREBUILT_DIR/usrlocal.tar.gz" ]; then
        echo "install-cam.sh: no prebuilt userspace payload, building from source"
        build_userspace_from_source
    elif [ "$PREBUILT_OS_ID" != "$ID" ] ||
         [ "$PREBUILT_OS_VERSION_ID" != "$VERSION_ID" ] ||
         [ "$PREBUILT_ARCH" != "$KARCH" ]; then
        # Sonames in the payload (libavcodec61, libqt5core5t64, ...) belong to
        # one Debian release; on another the binaries would not resolve.
        echo "install-cam.sh: prebuilt userspace is for ${PREBUILT_OS_ID:-?} ${PREBUILT_OS_VERSION_ID:-?} ${PREBUILT_ARCH:-?}, running $ID $VERSION_ID $KARCH"
        build_userspace_from_source
    else
        install_prebuilt_userspace
    fi

    # Neither path can be trusted to report its own failure: install_libcamera.sh
    # has no `set -e` and exits 0 even when meson dies. Check for the artefact.
    if [ ! -e "$LIBCAMERA_MARKER" ]; then
        echo "install-cam.sh: install finished but $LIBCAMERA_MARKER is missing - it failed" >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# 3. config.txt
# ---------------------------------------------------------------------------

# camera_auto_detect probes for Raspberry Pi's own sensors on the same CSI port
# and fights with an explicit overlay, so it gets commented out - as does any
# hand-added fr_imx900 line, which would otherwise load the overlay twice.
echo "install-cam.sh: enabling fr_imx900 on CAM0 in $CONFIG_TXT"
configtxt_set_block cmdb-cam '^[[:space:]]*(camera_auto_detect=|dtoverlay=(fr_)?imx900)' <<'EOF'
camera_auto_detect=0
dtoverlay=fr_imx900,cam0
EOF

echo "install-cam.sh: done - reboot, then check with 'rpicam-hello --list-cameras'."
