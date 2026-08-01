# Shared helpers for the CMDB setup scripts. Sourced, never executed - on its
# own it changes nothing in the system. Provides:
#
#   * temp-file tracking, cleaned up if a script dies mid-rewrite
#   * require_root
#   * config.txt: locate it, replace a named block in it
#   * hostname: MAC lookup, and hostname + /etc/hosts + avahi in one call
#
# install-loracom.sh and install-cam.sh each maintain their own block in the
# same config.txt, so that logic has to behave identically in both and lives
# here. The hostname helpers are copies of the ones in bootstrap/bootstrap.sh,
# which is installed alone into /opt/cmdb/bin and can't source this file - fix a
# bug in one, fix it in the other.

: "${CONFIG_TXT:=}"
: "${OVERLAY_DIR:=}"
: "${HOSTS_FILE:=/etc/hosts}"

# ---------------------------------------------------------------------------
# temp files
# ---------------------------------------------------------------------------

CMDB_TMPFILES=

cmdb_track_tmp() {
    CMDB_TMPFILES="$CMDB_TMPFILES $1"
}

cmdb_cleanup_tmp() {
    if [ -n "$CMDB_TMPFILES" ]; then
        # shellcheck disable=SC2086  # deliberate: paths we created ourselves
        rm -f $CMDB_TMPFILES
        CMDB_TMPFILES=
    fi
}

trap cmdb_cleanup_tmp EXIT
trap 'cmdb_cleanup_tmp; exit 1' HUP INT TERM

# ---------------------------------------------------------------------------
# root
# ---------------------------------------------------------------------------

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "$(basename "$0") must be run as root" >&2
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# config.txt
# ---------------------------------------------------------------------------

# Sets CONFIG_TXT and OVERLAY_DIR. Both can be pre-set to override the search.
configtxt_init() {
    if [ -z "$CONFIG_TXT" ]; then
        # Bookworm and later moved the boot partition to /boot/firmware.
        for candidate in /boot/firmware/config.txt /boot/config.txt; do
            if [ -f "$candidate" ]; then
                CONFIG_TXT=$candidate
                break
            fi
        done
    fi

    if [ -z "$CONFIG_TXT" ]; then
        echo "$(basename "$0"): config.txt not found in /boot/firmware or /boot" >&2
        exit 1
    fi

    [ -n "$OVERLAY_DIR" ] || OVERLAY_DIR=$(dirname "$CONFIG_TXT")/overlays
}

# configtxt_set_block <block-name> [disable-regex]   (block body read from stdin)
#
# Replaces the named block, or appends it. A stray BEGIN with no matching END
# self-heals: everything from it to EOF is dropped. disable-regex, if given,
# comments out matching lines outside the block.
configtxt_set_block() {
    block_name=$1
    disable_re=${2:-}
    block_body=$(cat)

    block_begin="# BEGIN $block_name"
    block_end="# END $block_name"

    block_tmp=$(mktemp "${CONFIG_TXT}.XXXXXX")
    cmdb_track_tmp "$block_tmp"
    chmod 644 "$block_tmp" 2>/dev/null || :  # boot partition is FAT

    # Each step removes the temp file itself: a caller piping the body in runs
    # this in a subshell, where POSIX resets the trap set above.
    if ! awk -v b="$block_begin" -v e="$block_end" -v n="$block_name" -v dis="$disable_re" '
        $0==b {skip=1; next}
        $0==e {skip=0; next}
        skip {next}
        dis != "" && $0 ~ dis {print "#" $0 "  # disabled by " n; next}
        {print}
    ' "$CONFIG_TXT" > "$block_tmp"; then
        rm -f "$block_tmp"
        echo "$(basename "$0"): failed to read $CONFIG_TXT" >&2
        exit 1
    fi

    if ! {
        echo "$block_begin"
        echo "[all]"  # resets any conditional filter section the file ends inside
        printf '%s\n' "$block_body"
        echo "$block_end"
    } >> "$block_tmp"; then
        rm -f "$block_tmp"
        echo "$(basename "$0"): failed to write $block_tmp (is the boot partition full?)" >&2
        exit 1
    fi

    if ! mv "$block_tmp" "$CONFIG_TXT"; then
        rm -f "$block_tmp"
        echo "$(basename "$0"): failed to replace $CONFIG_TXT" >&2
        exit 1
    fi
    sync
}

# ---------------------------------------------------------------------------
# hostname
# ---------------------------------------------------------------------------

# First MAC among the non-loopback interfaces, hyphenated for use in a hostname.
cmdb_get_mac() {
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

# Sets the hostname and keeps the 127.0.1.1 entry in $HOSTS_FILE in sync with
# it - an unresolvable hostname makes sudo stall until its DNS timeout expires.
cmdb_set_hostname() {
    new_hostname=$1
    old_hostname=$(hostname)

    if [ "$old_hostname" = "$new_hostname" ]; then
        return 0
    fi

    case $new_hostname in
        *[!A-Za-z0-9-]* | -* | *-)
            echo "$(basename "$0"): '$new_hostname' is not a valid hostname" >&2
            return 1
            ;;
    esac

    # sethostname(2) would fail after /etc/hostname was already written.
    if [ ${#new_hostname} -gt 64 ]; then
        echo "$(basename "$0"): '$new_hostname' is ${#new_hostname} characters, over the 64-character limit" >&2
        return 1
    fi

    if ! (command -v hostnamectl >/dev/null 2>&1 && hostnamectl set-hostname "$new_hostname" 2>/dev/null); then
        printf '%s\n' "$new_hostname" > /etc/hostname || return 1
        hostname "$new_hostname" || return 1
    fi

    hosts_tmp=$(mktemp "${HOSTS_FILE}.XXXXXX") || return 1
    cmdb_track_tmp "$hosts_tmp"
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
