#!/bin/sh
# Runs early at boot (see cmdb-bootstrap.service). Fetches config from the ESP
# via `loracom --config` (whitespace-separated KEY=VALUE pairs) and publishes
# every pair two ways: `systemctl set-environment` (for systemd units started
# afterwards) and a self-owned block in /etc/environment (for PAM sessions,
# e.g. SSH). Then, if present among the propagated variables:
#   - CMDB_GETCONF_TIMESTAMP: sets the system clock to this unix timestamp.
#   - CMDB_POST_INIT_CMD: runs this command, with the propagated variables
#     already in its environment.
#   - CMDB_ID: sets the hostname to <prefix>-<CMDB_ID> (prefix defaults to
#     "raspi-usa", override with CMDB_HOSTNAME_PREFIX).
# If the ESP can't be reached at all, or it doesn't hand back a usable
# CMDB_ID, the hostname instead falls back to <prefix>-fallback-<mac>, using
# the MAC address of the first non-loopback interface, so the board is still
# reachable by name. Every hostname change is followed by a best-effort
# avahi-daemon restart so the new name is what shows up over mDNS
# (*.local) too, not just /etc/hostname.
# All of those are best-effort: failures are logged and ignored, they don't
# affect this script's own exit status.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${LORACOM_CMD:=$SCRIPT_DIR/loracom --config}"
: "${ENV_FILE:=/etc/environment}"
: "${HOSTS_FILE:=/etc/hosts}"
: "${UART_DEVICE:=/dev/ttyAMA3}"
: "${SKIP_SYSTEMCTL:=}"

MARKER_BEGIN="# BEGIN cmdb-bootstrap"
MARKER_END="# END cmdb-bootstrap"

# Temp files awaiting the mv that puts them in place, removed if we die in
# between (set -e, a signal) so no stray /etc/environment.XXXXXX is left
# behind. A path already consumed by mv is gone, so rm -f skips it.
TMPFILES=

track_tmp() {
    TMPFILES="$TMPFILES $1"
}

cleanup_tmp() {
    if [ -n "$TMPFILES" ]; then
        # Unquoted on purpose - space-separated paths we created ourselves.
        rm -f $TMPFILES
        TMPFILES=
    fi
}

trap cleanup_tmp EXIT
trap 'cleanup_tmp; exit 1' HUP INT TERM

# Sets the system hostname and keeps the 127.0.1.1 entry in /etc/hosts in sync
# with it - an unresolvable hostname makes sudo (and anything else doing a
# reverse lookup) stall until its DNS timeout expires.
set_hostname() {
    new_hostname=$1
    old_hostname=$(hostname)

    if [ "$old_hostname" = "$new_hostname" ]; then
        return 0
    fi

    # The caller only validates CMDB_ID; CMDB_HOSTNAME_PREFIX comes from the
    # ESP too and lands in the same name, so the composed result is what
    # actually has to be a legal hostname.
    case $new_hostname in
        *[!A-Za-z0-9-]* | -* | *-)
            echo "cmdb-bootstrap: '$new_hostname' is not a valid hostname" >&2
            return 1
            ;;
    esac

    # HOST_NAME_MAX is 64 on Linux; longer and sethostname(2) fails with
    # ENAMETOOLONG after /etc/hostname has already been written.
    if [ ${#new_hostname} -gt 64 ]; then
        echo "cmdb-bootstrap: '$new_hostname' is ${#new_hostname} characters, over the 64-character limit" >&2
        return 1
    fi

    # hostnamectl needs dbus, which may not be up yet this early at boot -
    # fall back to writing /etc/hostname and setting the live name directly.
    if ! (command -v hostnamectl >/dev/null 2>&1 && hostnamectl set-hostname "$new_hostname" 2>/dev/null); then
        printf '%s\n' "$new_hostname" > /etc/hostname || return 1
        hostname "$new_hostname" || return 1
    fi

    hosts_tmp=$(mktemp "${HOSTS_FILE}.XXXXXX") || return 1
    track_tmp "$hosts_tmp"
    chmod 644 "$hosts_tmp"
    # Rewrites the 127.0.1.1 mapping in place of the old name, keeping any
    # other aliases that share the line - replacing the whole line would drop
    # them. Further 127.0.1.1 lines (there shouldn't be any) get the old name
    # stripped too, so nothing keeps resolving to it, and are dropped entirely
    # if that leaves them with no names at all.
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

    # Best-effort: avahi-daemon caches the hostname at startup and doesn't
    # pick up changes on its own, so mDNS (*.local) would keep announcing
    # the old name until the next reboot without this. try-restart (not
    # restart) so we don't start it on boards where it's disabled/absent.
    if command -v systemctl >/dev/null 2>&1; then
        systemctl try-restart avahi-daemon >/dev/null 2>&1 || :
    fi
}

# First MAC address found among the non-loopback interfaces, used to build a
# fallback hostname when the ESP can't give us a CMDB_ID. Colons aren't a
# legal hostname character, so it comes back hyphenated.
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

# Called whenever we don't have a usable CMDB_ID from the ESP - keeps the
# board reachable by a stable, unique name instead of whatever hostname the
# image shipped with.
set_fallback_hostname() {
    mac=$(get_mac) || {
        echo "cmdb-bootstrap: no MAC address found, can't set fallback hostname" >&2
        return 1
    }
    fallback_hostname="${CMDB_HOSTNAME_PREFIX:-raspi-usa}-fallback-$mac"
    if set_hostname "$fallback_hostname"; then
        echo "cmdb-bootstrap: hostname set to $fallback_hostname (fallback, no CMDB_ID from HAT)"
    else
        echo "cmdb-bootstrap: failed to set fallback hostname to $fallback_hostname" >&2
    fi
}

# Checked before talking to the ESP: without UART3 the loracom call below can
# only fail, and "device missing" is a much more actionable message than the
# open() error it would produce.
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

set -f  # CONFIG comes from the ESP - don't let a stray '*'/'?' in it glob-expand
export $CONFIG  # also makes every KEY=VALUE visible below (CMDB_GETCONF_TIMESTAMP, CMDB_POST_INIT_CMD, ...)

ok=1

if [ -z "$SKIP_SYSTEMCTL" ]; then
    if ! systemctl set-environment $CONFIG; then
        echo "cmdb-bootstrap: systemctl set-environment failed" >&2
        ok=0
    fi
fi

tmp=$(mktemp "${ENV_FILE}.XXXXXX")
track_tmp "$tmp"
chmod 644 "$tmp"

# Drop any existing cmdb-bootstrap block (and self-heals a stray BEGIN with
# no matching END, e.g. from an interrupted previous run, by dropping
# everything from it to EOF); a missing ENV_FILE is silently treated as empty.
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

if [ -n "${CMDB_ID:-}" ]; then
    case $CMDB_ID in
        # Anything outside [A-Za-z0-9-] (or a leading/trailing '-') would make
        # an invalid hostname - refuse rather than half-apply it.
        *[!A-Za-z0-9-]* | -* | *-)
            echo "cmdb-bootstrap: CMDB_ID='$CMDB_ID' can't be used in a hostname (ignored, not critical)" >&2
            set_fallback_hostname
            ;;
        *)
            hostname_new="${CMDB_HOSTNAME_PREFIX:-raspi-usa}-$CMDB_ID"
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

if [ -n "${CMDB_GETCONF_TIMESTAMP:-}" ]; then
    if date -s "@$CMDB_GETCONF_TIMESTAMP" >/dev/null; then
        echo "cmdb-bootstrap: system clock set to unix time $CMDB_GETCONF_TIMESTAMP"
    else
        echo "cmdb-bootstrap: failed to set system clock from CMDB_GETCONF_TIMESTAMP=$CMDB_GETCONF_TIMESTAMP (ignored, not critical)" >&2
    fi
fi

if [ -n "${CMDB_POST_INIT_CMD:-}" ]; then
    if ! $CMDB_POST_INIT_CMD; then
        echo "cmdb-bootstrap: CMDB_POST_INIT_CMD failed" >&2
    fi
fi

[ "$ok" = 1 ]
