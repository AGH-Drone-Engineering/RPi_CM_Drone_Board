#!/bin/sh
# Everything we develop and run with on the board: apt packages (including the
# toolchain install-loracom.sh compiles with), pip packages, VNC, RDP, Claude Code.
#
# g++/make are listed explicitly rather than left to build-essential because
# install-loracom.sh fails without them; avahi-daemon because bootstrap.sh
# restarts it on every hostname change and no image guarantees it. MAVProxy is
# what install-cmdb-base.sh's MAVLink bridge service runs.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

require_root

APT_PACKAGES="screen build-essential g++ make git curl cmake pkg-config python3-pip python3-venv python3-dev i2c-tools minicom avahi-daemon xrdp"
PIP_PACKAGES="opencv-python ultralytics MAVProxy"

SETUP_USER=${SUDO_USER:-}

cat <<EOF
install-utils.sh changes the following:
  * apt: dev toolchain, python3, git/curl/cmake, i2c-tools, minicom, screen,
    avahi-daemon, xrdp, kernel headers
  * pip (system-wide): $PIP_PACKAGES
  * VNC: enabled
  * RDP: xrdp enabled on port 3389, desktop session for \$SUDO_USER
  * Claude Code: installed for \$SUDO_USER
EOF

export DEBIAN_FRONTEND=noninteractive

# ---------------------------------------------------------------------------
# 1. apt packages
# ---------------------------------------------------------------------------

echo "install-utils.sh: installing apt packages"
apt-get update

# The headers package name has moved around across RPi OS releases.
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

# ---------------------------------------------------------------------------
# 2. VNC
# ---------------------------------------------------------------------------

if command -v raspi-config >/dev/null 2>&1; then
    echo "install-utils.sh: enabling VNC"
    raspi-config nonint do_vnc 0
else
    echo "install-utils.sh: WARNING: raspi-config not found, skipping VNC setup" >&2
fi

# ---------------------------------------------------------------------------
# 3. RDP
# ---------------------------------------------------------------------------

# Without this xrdp can't read the snakeoil key and every login drops straight
# back to the greeter.
adduser xrdp ssl-cert >/dev/null 2>&1 || :

echo "install-utils.sh: enabling xrdp"
systemctl enable --now xrdp >/dev/null

if [ -z "$SETUP_USER" ] || [ "$SETUP_USER" = root ]; then
    echo "install-utils.sh: WARNING: no SUDO_USER, skipping the RDP session setup - run this as your normal user with sudo" >&2
else
    # xrdp serves its own Xorg session, and Bookworm ships Wayland-only session
    # files, so without an ~/.xsession the login succeeds into a black screen.
    XSESSION_STARTER=
    for candidate in startlxde-pi startlxde x-session-manager; do
        if command -v "$candidate" >/dev/null 2>&1; then
            XSESSION_STARTER=$candidate
            break
        fi
    done

    SETUP_HOME=$(getent passwd "$SETUP_USER" | cut -d: -f6)
    if [ -z "$XSESSION_STARTER" ] || [ -z "$SETUP_HOME" ]; then
        echo "install-utils.sh: WARNING: no X11 session starter or no home for $SETUP_USER, skipping ~/.xsession - RDP would log in to an empty desktop" >&2
    elif [ -e "$SETUP_HOME/.xsession" ]; then
        echo "install-utils.sh: $SETUP_HOME/.xsession already exists, leaving it alone"
    else
        printf '#!/bin/sh\nexec %s\n' "$XSESSION_STARTER" > "$SETUP_HOME/.xsession"
        chown "$SETUP_USER:$SETUP_USER" "$SETUP_HOME/.xsession"
        chmod 755 "$SETUP_HOME/.xsession"
        echo "install-utils.sh: RDP session for $SETUP_USER set to $XSESSION_STARTER"
    fi

    # RDP has no key auth, and RPi OS leaves the account passwordless if the
    # imager only planted an SSH key.
    if ! passwd -S "$SETUP_USER" 2>/dev/null | awk '{exit $2!="P"}'; then
        echo "install-utils.sh: WARNING: $SETUP_USER has no usable password - set one with 'passwd $SETUP_USER' or RDP will refuse the login" >&2
    fi
fi

# ---------------------------------------------------------------------------
# 4. pip packages
# ---------------------------------------------------------------------------

# PEP 668: these have to be importable from any script and any user on the
# drone, not shut inside a venv.
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

# ---------------------------------------------------------------------------
# 5. Claude Code
# ---------------------------------------------------------------------------

# Installs per-user, so not as root - nobody would have it on their PATH.
CLAUDE_USER=$SETUP_USER
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
