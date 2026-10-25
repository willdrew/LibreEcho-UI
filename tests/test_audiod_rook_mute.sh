#!/bin/sh
# audiod built for the Echo Spot (-DLE_TARGET_ROOK): the Spot's codec has no capture switch, so
# set_mute could never silence the microphones (measured 2026-10-25: the room's noise energy was
# unchanged while "muted"). On the Spot, mute is a flag file that capture-mux obeys
# (LE_AUDIO_MUTE_FILE, default /run/libreecho/mic-muted). Across a reboot the mute is the web's
# own setting (web-config.json microphone_muted, applied at start), as on the Dots.
set -eu

make BUILD=build-rook CPPFLAGS=-DLE_TARGET_ROOK build-rook/libreecho-audiod >/dev/null
d=$(mktemp -d)
pid=0
cleanup() {
    [ "$pid" -gt 1 ] && { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
    rm -rf "$d"
}
trap cleanup EXIT INT TERM
export LE_AUDIO_MUTE_FILE="$d/run/mic-muted"
mkdir -p "$d/run"

start() {
    ./build-rook/libreecho-audiod --foreground --socket "$d/audio.sock" --card 31 >>"$d/log" 2>&1 &
    pid=$!
    i=0
    while [ ! -S "$d/audio.sock" ]; do
        i=$((i + 1)); [ "$i" -lt 50 ] || { cat "$d/log"; exit 1; }
        sleep 0.1
    done
}
stop() { kill "$pid"; wait "$pid" 2>/dev/null || true; pid=0; rm -f "$d/audio.sock"; }
call() {
    python3 - "$d/audio.sock" "$1" "$2" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall((json.dumps({"v": 1, "id": 1, "cmd": sys.argv[2], "args": json.loads(sys.argv[3])}) + "\n").encode())
print(s.makefile().readline().strip())
EOF
}
fail() { echo "audiod rook mute: $*"; cat "$d/log"; exit 1; }

start
r=$(call set_mute '{"muted":true}')
case "$r" in *'"ok":true'*) ;; *) fail "set_mute true: $r" ;; esac
[ -e "$LE_AUDIO_MUTE_FILE" ] || fail "muted but no $LE_AUDIO_MUTE_FILE for capture-mux"
case "$(call status '{}')" in *'"muted":true'*) ;; *) fail "status does not say muted" ;; esac
echo "audiod rook mute: set_mute leaves the flag capture-mux obeys"

call set_mute '{"muted":false}' >/dev/null
[ ! -e "$LE_AUDIO_MUTE_FILE" ] || fail "unmuted but the flag remains"
case "$(call status '{}')" in *'"muted":false'*) ;; *) fail "status does not say unmuted" ;; esac
echo "audiod rook mute: unmute clears it"

# A restart starts unmuted and clears a stale flag; the web reapplies its saved setting.
call set_mute '{"muted":true}' >/dev/null
stop
start
[ ! -e "$LE_AUDIO_MUTE_FILE" ] || fail "restarted unmuted, but a stale flag still silences the microphones"
echo "audiod rook mute: a restart begins unmuted, with no stale flag"
