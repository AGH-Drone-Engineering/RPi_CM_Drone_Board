# Shared helpers for the CMDB setup scripts. Sourced, never executed.
#
# The config.txt helpers live here because install-loracom.sh and
# install-cam.sh each maintain their own marked block in the same file - the
# drop-and-reappend logic has to behave identically in both, or one will eat
# the other's settings.

: "${CONFIG_TXT:=}"
: "${OVERLAY_DIR:=}"

# Temp files awaiting the mv that puts them in place. Without this a script
# dying mid-rewrite (read-only or full boot partition, a signal, set -e on any
# of the commands in between) leaves a stray config.txt.XXXXXX behind - on the
# boot partition, where it is both visible and useless.
#
# Nothing ever has to be untracked: once mv has consumed a temp file the path
# is simply gone and the rm -f below is a no-op for it.
CMDB_TMPFILES=

cmdb_track_tmp() {
    CMDB_TMPFILES="$CMDB_TMPFILES $1"
}

cmdb_cleanup_tmp() {
    if [ -n "$CMDB_TMPFILES" ]; then
        # Unquoted on purpose - the list is space-separated paths we created
        # ourselves from CONFIG_TXT/ENV_FILE, none of which contain spaces.
        rm -f $CMDB_TMPFILES
        CMDB_TMPFILES=
    fi
}

trap cmdb_cleanup_tmp EXIT
trap 'cmdb_cleanup_tmp; exit 1' HUP INT TERM

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "$(basename "$0") must be run as root" >&2
        exit 1
    fi
}

# Locates config.txt and the overlay directory next to it, setting CONFIG_TXT
# and OVERLAY_DIR. Both can be pre-set to override the search (used by tests).
configtxt_init() {
    if [ -z "$CONFIG_TXT" ]; then
        # Bookworm and later moved the boot partition to /boot/firmware; older
        # images still mount it at /boot.
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
# Replaces the named block, or appends it if it isn't there yet - so callers
# are idempotent and can't accumulate duplicate settings. A stray BEGIN with no
# matching END (an interrupted previous run) is self-healing: everything from
# it to EOF is dropped along with it.
#
# disable-regex, if given, comments out matching lines *outside* the block.
# Needed where an explicit setting has to win over one the image shipped with
# and last-one-wins can't be relied on.
configtxt_set_block() {
    block_name=$1
    disable_re=${2:-}
    block_body=$(cat)

    block_begin="# BEGIN $block_name"
    block_end="# END $block_name"

    block_tmp=$(mktemp "${CONFIG_TXT}.XXXXXX")
    cmdb_track_tmp "$block_tmp"
    chmod 644 "$block_tmp" 2>/dev/null || :  # boot partition is FAT, mode is cosmetic there

    # Each step removes the temp file itself rather than leaning on the EXIT
    # trap: a caller that pipes the block body in (`printf ... | set_block`)
    # runs this function in a subshell, and POSIX resets caught signals there,
    # so the trap would never fire for it.
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

    # [all] resets any conditional filter section the file happened to end
    # inside, so these settings apply unconditionally.
    if ! {
        echo "$block_begin"
        echo "[all]"
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
