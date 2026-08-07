#!/bin/sh
# Regenerates the prebuilt camera payload in this directory, which install-cam.sh
# then uses to install the same stack on identical boards without compiling.
#
# Run on a board where the FRAMOS stack has already been built and installed from
# source (i.e. after `CMDB_REBUILD_LIBCAMERA=1 ./install-cam.sh` took the build
# path), then commit whatever changes here.
#
# Modules and overlays are rebuilt from ../framos-rpi-drivers rather than copied
# out of /lib/modules, so the payload provably matches the sources in this repo.
# The /usr/local half can only be harvested from a live install - it is a
# ten-minute meson build we are deliberately not repeating.
#
# Its file list comes from meson's install logs, the runtime package list from
# the DT_NEEDED entries of what those logs name. Both are derived rather than
# hand-kept: the package names are release-specific (libavcodec61,
# libqt5core5t64), so a hand-kept list would name the previous release's
# packages the first time this runs on a new one - and install-cam.sh's manifest
# check would wave that through, everything else in it having been updated
# correctly.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRIVERS_DIR=$SCRIPT_DIR/../framos-rpi-drivers
FRAMOS_BUILD_DIR=${FRAMOS_BUILD_DIR:-/opt/framos}

LIBCAMERA_LOG=$FRAMOS_BUILD_DIR/framos-libcamera/build/meson-logs/install-log.txt
RPICAM_LOG=$FRAMOS_BUILD_DIR/rpicam-apps/build/meson-logs/install-log.txt

cat <<EOF
make-prebuilt.sh changes the following (nothing outside this repo):
  * $SCRIPT_DIR/modules, overlays: rebuilt from ../framos-rpi-drivers
  * $SCRIPT_DIR/usrlocal.tar.gz: harvested from the live /usr/local
  * $SCRIPT_DIR/manifest: the pins install-cam.sh checks against
EOF

for log in "$LIBCAMERA_LOG" "$RPICAM_LOG"; do
    if [ ! -f "$log" ]; then
        echo "make-prebuilt.sh: $log not found - build the stack from source first (CMDB_REBUILD_LIBCAMERA=1 ../install-cam.sh)" >&2
        exit 1
    fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# ---------------------------------------------------------------------------
# 1. kernel modules + overlays, rebuilt from the vendored sources
# ---------------------------------------------------------------------------

KVER=$(uname -r)
KARCH=$(uname -m)
echo "make-prebuilt.sh: building modules and overlays for $KVER"
make -C "$DRIVERS_DIR" modules dtbs >/dev/null

rm -rf "$SCRIPT_DIR/modules" "$SCRIPT_DIR/overlays"
mkdir -p "$SCRIPT_DIR/modules" "$SCRIPT_DIR/overlays"

# Matches what `make modules_install` produces; depmod reads .ko.xz natively.
for ko in "$DRIVERS_DIR"/drivers/fr_*.ko; do
    xz -9 -c "$ko" > "$SCRIPT_DIR/modules/$(basename "$ko").xz"
done
cp "$DRIVERS_DIR"/overlays/fr_*.dtbo "$SCRIPT_DIR/overlays/"

make -C "$DRIVERS_DIR" clean >/dev/null 2>&1 || :
rm -rf "$DRIVERS_DIR/overlays/preprocessed"

# ---------------------------------------------------------------------------
# 2. /usr/local payload, harvested from the live install
# ---------------------------------------------------------------------------

echo "make-prebuilt.sh: collecting /usr/local payload"

{
    grep -hv '^#' "$LIBCAMERA_LOG" "$RPICAM_LOG"

    # Signed by a post-install step meson does not log. They must ship
    # unmodified next to the .so and the libcamera.so holding the matching
    # public key: a signature libcamera cannot verify sends the IPA into an
    # isolated process instead of loading it in-process. Never strip anything
    # here for the same reason.
    find /usr/local/lib -name '*.so.sign' 2>/dev/null
} | sed 's#^/usr/local/##' | grep -v '^/' | grep -v '__pycache__' | sort -u > "$work/all.txt"

while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -L "/usr/local/$rel" ] || [ -f "/usr/local/$rel" ]; then
        printf '%s\n' "$rel"
    fi
done < "$work/all.txt" > "$work/payload.txt"

count=$(wc -l < "$work/payload.txt")
if [ "$count" -lt 100 ]; then
    echo "make-prebuilt.sh: only $count files collected - that is far too few, is the stack actually installed?" >&2
    exit 1
fi

# gzip: unpacked on a possibly minimal image, where it is the one always there.
# Explicit root ownership, since it is unpacked over /usr/local as root.
tar czf "$SCRIPT_DIR/usrlocal.tar.gz" -C /usr/local \
    --owner=0 --group=0 --numeric-owner -T "$work/payload.txt"

# ---------------------------------------------------------------------------
# 3. runtime dependencies of the payload, derived from the payload
# ---------------------------------------------------------------------------

# Direct DT_NEEDED only; apt resolves the rest of the closure. Excluded: what
# the payload ships for itself, and what every Debian system has by definition.
echo "make-prebuilt.sh: deriving runtime dependencies"

while IFS= read -r rel; do
    case $rel in
        bin/*|lib/*) [ -x "/usr/local/$rel" ] || continue ;;
        *) continue ;;
    esac
    objdump -p "/usr/local/$rel" 2>/dev/null | awk '/NEEDED/ {print $2}'
done < "$work/payload.txt" |
    sort -u | grep -vE '^(libcamera|libpisp|librpicam)' > "$work/sonames.txt"

: > "$work/pkgs.txt"
while IFS= read -r soname; do
    path=$(ldconfig -p | awk -v s="$soname" '$1==s {print $NF; exit}')
    [ -n "$path" ] || continue
    dpkg -S "$(readlink -f "$path")" 2>/dev/null | cut -d: -f1 >> "$work/pkgs.txt"
done < "$work/sonames.txt"

runtime_packages=$(sort -u "$work/pkgs.txt" |
    grep -vE '^(libc6|libgcc-s1|libstdc\+\+6|libudev1)$' | tr '\n' ' ')
runtime_packages=${runtime_packages% }

if [ -z "$runtime_packages" ]; then
    echo "make-prebuilt.sh: derived no runtime dependencies from $(wc -l < "$work/sonames.txt") sonames - refusing to write a manifest that would install none" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 4. manifest
# ---------------------------------------------------------------------------

. /etc/os-release
: "${ID:=}" "${VERSION_ID:=}"  # missing on rolling images, and set -u is on

# The real file, not the symlinks: it carries the full version.
libcamera_ver=$(find /usr/local/lib -maxdepth 2 -type f -name 'libcamera.so.*' -printf '%f\n' 2>/dev/null | sort -V | tail -1)
[ -n "$libcamera_ver" ] || libcamera_ver=unknown

cat > "$SCRIPT_DIR/manifest" <<EOF
# Generated by make-prebuilt.sh - do not edit by hand.
#
# Sourced as shell by install-cam.sh, so keep every value single-quoted: an
# unquoted parenthesis is a syntax error there. The first four are the pins it
# refuses the payload on, the fifth is what the payload needs installed before
# it will run, the rest is provenance.
PREBUILT_KERNEL='$KVER'
PREBUILT_ARCH='$KARCH'
PREBUILT_OS_ID='$ID'
PREBUILT_OS_VERSION_ID='$VERSION_ID'
PREBUILT_RUNTIME_PACKAGES='$runtime_packages'
PREBUILT_LIBCAMERA='$libcamera_ver'
PREBUILT_DRIVERS='vendored (see ../framos-rpi-drivers/PATCHES.md)'
PREBUILT_FILES='$count'
PREBUILT_DATE='$(date -u +%Y-%m-%d)'
EOF

echo "make-prebuilt.sh: done"
echo "  modules:  $(ls "$SCRIPT_DIR/modules" | wc -l) (.ko.xz, $KVER)"
echo "  overlays: $(ls "$SCRIPT_DIR/overlays" | wc -l) (.dtbo)"
echo "  usrlocal: $count files, $(du -h "$SCRIPT_DIR/usrlocal.tar.gz" | cut -f1)"
echo "  runtime:  $(echo "$runtime_packages" | wc -w) packages"
