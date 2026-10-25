#!/bin/sh
# ledd --frame-file PATH: the ring's twelve pixels go to a plain file, in the
# IS31FL3236 frame format ("rrggbb" x 12, one line). The Echo Spot has no LED
# ring; its screen draws the frame round the rim (tools/spot spot-clock).
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/led.sock"
frame_path="$test_dir/ring"
log_path=./build/test-ledd-frame-file.log
pid=0

cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$test_dir"
}
trap cleanup EXIT INT TERM

./build/libreecho-ledd --foreground --socket "$socket_path" \
    --frame-file "$frame_path" >"$log_path" 2>&1 &
pid=$!

i=0
while [ ! -S "$socket_path" ]; do
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        cat "$log_path"
        exit 1
    fi
    sleep 0.1
done

# Shows a solid colour (a transient pattern, so nothing is persisted), then
# prints the frame once ledd has written it.
frame_after() {
    python3 - "$socket_path" "$frame_path" "$1" "$2" "$3" <<'EOF'
import json, socket, sys, time
sock, path, r, g, b = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:])
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
s.sendall((json.dumps({"v": 1, "id": 1, "cmd": "pattern",
                       "args": {"name": "solid", "r": r, "g": g, "b": b,
                                "brightness": 100, "repeats": 1}}) + "\n").encode())
reply = s.makefile().readline()
assert '"ok":true' in reply, reply
time.sleep(0.5)
print(open(path).read().strip())
EOF
}

check() { # name frame r-lit g-lit b-lit
    python3 - "$@" <<'EOF'
import sys
name, frame, want = sys.argv[1], sys.argv[2], [c == "1" for c in sys.argv[3:]]
assert len(frame) == 72, f"{name}: frame is {frame!r}"
pixels = [frame[i:i + 6] for i in range(0, 72, 6)]
assert len(set(pixels)) == 1, f"{name}: pixels differ: {pixels}"
lit = [int(pixels[0][i:i + 2], 16) > 0 for i in (0, 2, 4)]
assert lit == want, f"{name}: pixel {pixels[0]}, want lit {want}"
print(f"ledd frame file: {name} ok ({pixels[0]})")
EOF
}

[ -f "$frame_path" ] || { echo "ledd did not create $frame_path"; cat "$log_path"; exit 1; }
check red "$(frame_after 255 0 0)" 1 0 0
check blue "$(frame_after 0 0 255)" 0 0 1
