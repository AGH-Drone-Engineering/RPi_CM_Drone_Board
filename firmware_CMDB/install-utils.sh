#!/bin/sh
# Everything we develop and run with on the board: apt packages (including the
# toolchain install-loracom.sh needs), pip packages, VNC, and Claude Code.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

# g++/make come from build-essential; they're listed explicitly because
# install-loracom.sh compiles loracom and would fail without them.
# avahi-daemon: bootstrap.sh restarts it after every hostname change so the
# board's *.local mDNS name tracks CMDB_ID (or the MAC-based fallback)
# instead of going stale - usually preinstalled on Raspberry Pi OS, but not
# guaranteed on every image.
APT_PACKAGES="screen build-essential g++ make git curl cmake pkg-config python3-pip python3-venv python3-dev i2c-tools minicom avahi-daemon"
PIP_PACKAGES="opencv-python ultralytics"

echo "install-utils.sh: installing apt packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update

# Kernel headers, for building anything out-of-tree against the running kernel.
# The package name has moved around across RPi OS releases, so take the first
# candidate the archive actually has.
KERNEL_HEADERS=""
for candidate in "linux-headers-$(uname -r)" raspberrypi-kernel-headers linux-headers-rpi-2712 linux-headers-rpi-v8; do
    if apt-cache show "$candidate" >/dev/null 2>&1; then
        KERNEL_HEADERS=$candidate
        echo "install-utils.sh: kernel headers package: $KERNEL_HEADERS"
        break
    fi
done
if [ -z "$KERNEL_HEADERS" ]; then
    echo "install-utils.sh: WARNING: no kernel headers package found in the archive, skipping" >&2
fi

# shellcheck disable=SC2086  # deliberate word splitting into separate packages
apt-get install -y $APT_PACKAGES $KERNEL_HEADERS

if command -v raspi-config >/dev/null 2>&1; then
    echo "install-utils.sh: enabling VNC"
    raspi-config nonint do_vnc 0
else
    echo "install-utils.sh: WARNING: raspi-config not found, skipping VNC setup" >&2
fi

# Debian marks the system Python as externally managed (PEP 668), so a plain
# `pip install` refuses to touch it. We want these importable from any script
# and any user on the drone, not shut inside a venv, so opt out explicitly.
PIP_FLAGS=""
for marker in /usr/lib/python3*/EXTERNALLY-MANAGED; do
    if [ -e "$marker" ]; then
        PIP_FLAGS="--break-system-packages"
        break
    fi
done

echo "install-utils.sh: installing pip packages"
# shellcheck disable=SC2086  # deliberate word splitting into separate packages
pip3 install --upgrade $PIP_FLAGS $PIP_PACKAGES
# NOTE: `json` is in the Python standard library - there is nothing to install
# for it (the PyPI package of that name is unrelated). Just `import json`.

# Claude Code installs per-user, so run it as the invoking user rather than
# dropping it in root's home where nobody will have it on their PATH.
CLAUDE_USER=${SUDO_USER:-}
if [ -z "$CLAUDE_USER" ] || [ "$CLAUDE_USER" = root ]; then
    echo "install-utils.sh: WARNING: no SUDO_USER, skipping Claude Code install - run 'curl -fsSL https://claude.ai/install.sh | bash' as your normal user" >&2
elif su - "$CLAUDE_USER" -c 'command -v claude' >/dev/null 2>&1; then
    echo "install-utils.sh: Claude Code already installed for $CLAUDE_USER"
else
    echo "install-utils.sh: installing Claude Code for $CLAUDE_USER"
    if ! su - "$CLAUDE_USER" -c 'curl -fsSL https://claude.ai/install.sh | bash'; then
        echo "install-utils.sh: WARNING: Claude Code install failed (ignored, not critical)" >&2
    fi
fi

echo "install-utils.sh: done"
