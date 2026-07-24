#!/bin/sh
# Runs early at boot (see cmdb-bootstrap.service). Fetches config from the ESP
# via `loracom --config` (whitespace-separated KEY=VALUE pairs) and publishes
# every pair two ways: `systemctl set-environment` (for systemd units started
# afterwards) and a self-owned block in /etc/environment (for PAM sessions,
# e.g. SSH). Then, if present among the propagated variables:
#   - CMDB_GETCONF_TIMESTAMP: sets the system clock to this unix timestamp.
#   - CMDB_POST_INIT_CMD: runs this command, with the propagated variables
#     already in its environment.
# Both of those are best-effort: failures are logged and ignored, they don't
# affect this script's own exit status.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${LORACOM_CMD:=$SCRIPT_DIR/loracom --config}"
: "${ENV_FILE:=/etc/environment}"
: "${SKIP_SYSTEMCTL:=}"

MARKER_BEGIN="# BEGIN cmdb-bootstrap"
MARKER_END="# END cmdb-bootstrap"

CONFIG=$($LORACOM_CMD) || {
    echo "cmdb-bootstrap: $LORACOM_CMD failed" >&2
    exit 1
}

if [ -z "$CONFIG" ]; then
    echo "cmdb-bootstrap: $LORACOM_CMD returned nothing to propagate" >&2
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
