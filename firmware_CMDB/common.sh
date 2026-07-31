# Shared helpers for the CMDB setup scripts. Sourced, never executed.
#
# The config.txt helpers live here because install-loracom.sh and
# install-cam.sh each maintain their own marked block in the same file - the
# drop-and-reappend logic has to behave identically in both, or one will eat
# the other's settings.

: "${CONFIG_TXT:=}"
: "${OVERLAY_DIR:=}"

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
    chmod 644 "$block_tmp" 2>/dev/null || :  # boot partition is FAT, mode is cosmetic there

    awk -v b="$block_begin" -v e="$block_end" -v n="$block_name" -v dis="$disable_re" '
        $0==b {skip=1; next}
        $0==e {skip=0; next}
        skip {next}
        dis != "" && $0 ~ dis {print "#" $0 "  # disabled by " n; next}
        {print}
    ' "$CONFIG_TXT" > "$block_tmp"

    # [all] resets any conditional filter section the file happened to end
    # inside, so these settings apply unconditionally.
    {
        echo "$block_begin"
        echo "[all]"
        printf '%s\n' "$block_body"
        echo "$block_end"
    } >> "$block_tmp"

    mv "$block_tmp" "$CONFIG_TXT"
    sync
}
