#!/bin/sh
# Runs early at boot (see cmdb-bootstrap.service). Fetches config from the ESP
# via `loracom --config` (whitespace-separated KEY=VALUE pairs), publishes every
# pair as an environment variable, and acts on the ones it recognises: CMDB_ID
# names the board, CMDB_GETCONF_TIMESTAMP sets the clock, CMDB_POST_INIT_CMD is
# run. All of that is best-effort: failures are logged and ignored.
#
# The hostname helpers are copies of the ones in ../common.sh - this script is
# installed alone into /opt/cmdb/bin and can't source it. Fix a bug in one, fix
# it in the other.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${LORACOM_CMD:=$SCRIPT_DIR/loracom --config}"
: "${ENV_FILE:=/etc/environment}"
: "${HOSTS_FILE:=/etc/hosts}"
: "${UART_DEVICE:=/dev/ttyAMA3}"
: "${SKIP_SYSTEMCTL:=}"

MARKER_BEGIN="# BEGIN cmdb-bootstrap"
MARKER_END="# END cmdb-bootstrap"

HOSTNAME_PREFIX=${CMDB_HOSTNAME_PREFIX:-raspi-usa}

cat <<EOF
cmdb-bootstrap: changes the following, from what the HAT reports:
  * systemd environment and $ENV_FILE: every KEY=VALUE pair returned
  * hostname: $HOSTNAME_PREFIX-<CMDB_ID>, or $HOSTNAME_PREFIX-fallback-<mac> without one
  * system clock, if CMDB_GETCONF_TIMESTAMP is among them
  * whatever CMDB_POST_INIT_CMD runs, if it is among them
EOF

TMPFILES=

track_tmp() {
    TMPFILES="$TMPFILES $1"
}

cleanup_tmp() {
    if [ -n "$TMPFILES" ]; then
        # shellcheck disable=SC2086  # deliberate: paths we created ourselves
        rm -f $TMPFILES
        TMPFILES=
    fi
}

trap cleanup_tmp EXIT
trap 'cleanup_tmp; exit 1' HUP INT TERM

# Keeps the 127.0.1.1 entry in $HOSTS_FILE in sync - an unresolvable hostname
# makes sudo stall until its DNS timeout expires.
set_hostname() {
    new_hostname=$1
    old_hostname=$(hostname)

    if [ "$old_hostname" = "$new_hostname" ]; then
        return 0
    fi

    # The caller only validates CMDB_ID, but the prefix comes from the ESP too.
    case $new_hostname in
        *[!A-Za-z0-9-]* | -* | *-)
            echo "cmdb-bootstrap: '$new_hostname' is not a valid hostname" >&2
            return 1
            ;;
    esac

    # sethostname(2) would fail after /etc/hostname was already written.
    if [ ${#new_hostname} -gt 64 ]; then
        echo "cmdb-bootstrap: '$new_hostname' is ${#new_hostname} characters, over the 64-character limit" >&2
        return 1
    fi

    # hostnamectl needs dbus, which may not be up this early at boot.
    if ! (command -v hostnamectl >/dev/null 2>&1 && hostnamectl set-hostname "$new_hostname" 2>/dev/null); then
        printf '%s\n' "$new_hostname" > /etc/hostname || return 1
        hostname "$new_hostname" || return 1
    fi

    hosts_tmp=$(mktemp "${HOSTS_FILE}.XXXXXX") || return 1
    track_tmp "$hosts_tmp"
    chmod 644 "$hosts_tmp"
    # Keeps any other aliases sharing the line.
    awk -v old="$old_hostname" -v new="$new_hostname" '
        $1=="127.0.1.1" {
            line = "127.0.1.1"
            if (!seen) { line = line "\t" new; seen=1 }
            for (i = 2; i <= NF; i++) {
                if ($i != old && $i != new) line = line "\t" $i
            }
            if (line == "127.0.1.1") next
            print line
            next
        }
        {print}
        END {if (!seen) print "127.0.1.1\t" new}
    ' "$HOSTS_FILE" 2>/dev/null > "$hosts_tmp" || { rm -f "$hosts_tmp"; return 1; }
    mv "$hosts_tmp" "$HOSTS_FILE" || { rm -f "$hosts_tmp"; return 1; }

    # avahi caches the hostname at startup, so *.local would go stale otherwise.
    if command -v systemctl >/dev/null 2>&1; then
        systemctl try-restart avahi-daemon >/dev/null 2>&1 || :
    fi
}

# First MAC among the non-loopback interfaces, hyphenated for use in a hostname.
get_mac() {
    for addr_file in /sys/class/net/*/address; do
        iface=${addr_file%/address}
        iface=${iface##*/}
        [ "$iface" = "lo" ] && continue
        mac=$(cat "$addr_file" 2>/dev/null) || continue
        if [ -n "$mac" ] && [ "$mac" != "00:00:00:00:00:00" ]; then
            printf '%s\n' "$mac" | tr ':' '-'
            return 0
        fi
    done
    return 1
}

# Whenever we don't have a usable CMDB_ID: a stable, unique name beats whatever
# the image shipped with.
set_fallback_hostname() {
    mac=$(get_mac) || {
        echo "cmdb-bootstrap: no MAC address found, can't set fallback hostname" >&2
        return 1
    }
    fallback_hostname="$HOSTNAME_PREFIX-fallback-$mac"
    if set_hostname "$fallback_hostname"; then
        echo "cmdb-bootstrap: hostname set to $fallback_hostname (fallback, no CMDB_ID from HAT)"
    else
        echo "cmdb-bootstrap: failed to set fallback hostname to $fallback_hostname" >&2
    fi
}

# ---------------------------------------------------------------------------
# 1. fetch the config
# ---------------------------------------------------------------------------

# "device missing" is more actionable than the open() error loracom would give.
if [ ! -e "$UART_DEVICE" ]; then
    echo "cmdb-bootstrap: WARNING: $UART_DEVICE does not exist - UART3 is probably not enabled; run system-setup.sh and reboot" >&2
fi

CONFIG=$($LORACOM_CMD) || {
    echo "cmdb-bootstrap: $LORACOM_CMD failed" >&2
    set_fallback_hostname
    exit 1
}

if [ -z "$CONFIG" ]; then
    echo "cmdb-bootstrap: $LORACOM_CMD returned nothing to propagate" >&2
    set_fallback_hostname
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. publish it
# ---------------------------------------------------------------------------

set -f  # CONFIG comes from the ESP - don't let a stray '*'/'?' in it glob-expand
export $CONFIG

# Re-resolved: the ESP may have just supplied its own prefix.
HOSTNAME_PREFIX=${CMDB_HOSTNAME_PREFIX:-raspi-usa}

ok=1

# For systemd units started after this one.
if [ -z "$SKIP_SYSTEMCTL" ]; then
    if ! systemctl set-environment $CONFIG; then
        echo "cmdb-bootstrap: systemctl set-environment failed" >&2
        ok=0
    fi
fi

# For PAM sessions, e.g. SSH logins. Own block, self-healing if a previous run
# was interrupted between the markers.
tmp=$(mktemp "${ENV_FILE}.XXXXXX")
track_tmp "$tmp"
chmod 644 "$tmp"

awk -v b="$MARKER_BEGIN" -v e="$MARKER_END" '
    $0==b {skip=1; next}
    $0==e {skip=0; next}
    !skip {print}
' "$ENV_FILE" 2>/dev/null > "$tmp" || :

{
    echo "$MARKER_BEGIN"
    for kv in $CONFIG; do
        echo "$kv"
    done
    echo "$MARKER_END"
} >> "$tmp"

if mv "$tmp" "$ENV_FILE"; then
    sync
else
    echo "cmdb-bootstrap: failed to update $ENV_FILE" >&2
    rm -f "$tmp"
    ok=0
fi

if [ "$ok" = 1 ]; then
    echo "cmdb-bootstrap: propagated config variables"
fi

# ---------------------------------------------------------------------------
# 3. hostname
# ---------------------------------------------------------------------------

if [ -n "${CMDB_ID:-}" ]; then
    case $CMDB_ID in
        *[!A-Za-z0-9-]* | -* | *-)
            echo "cmdb-bootstrap: CMDB_ID='$CMDB_ID' can't be used in a hostname (ignored, not critical)" >&2
            set_fallback_hostname
            ;;
        *)
            hostname_new="$HOSTNAME_PREFIX-$CMDB_ID"
            if set_hostname "$hostname_new"; then
                echo "cmdb-bootstrap: hostname set to $hostname_new"
            else
                echo "cmdb-bootstrap: failed to set hostname to $hostname_new (ignored, not critical)" >&2
                set_fallback_hostname
            fi
            ;;
    esac
else
    echo "cmdb-bootstrap: no CMDB_ID propagated by the HAT" >&2
    set_fallback_hostname
fi

# ---------------------------------------------------------------------------
# 4. clock
# ---------------------------------------------------------------------------

if [ -n "${CMDB_GETCONF_TIMESTAMP:-}" ]; then
    if date -s "@$CMDB_GETCONF_TIMESTAMP" >/dev/null; then
        echo "cmdb-bootstrap: system clock set to unix time $CMDB_GETCONF_TIMESTAMP"
    else
        echo "cmdb-bootstrap: failed to set system clock from CMDB_GETCONF_TIMESTAMP=$CMDB_GETCONF_TIMESTAMP (ignored, not critical)" >&2
    fi
fi

# ---------------------------------------------------------------------------
# 5. post-init command
# ---------------------------------------------------------------------------

if [ -n "${CMDB_POST_INIT_CMD:-}" ]; then
    if ! $CMDB_POST_INIT_CMD; then
        echo "cmdb-bootstrap: CMDB_POST_INIT_CMD failed" >&2
    fi
fi

[ "$ok" = 1 ]
