#!/bin/sh
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/led.sock"
log_path="$test_dir/led.log"
frame_path="$test_dir/ring"
pid=0

cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$test_dir"
}
trap cleanup EXIT INT TERM

${LIBREECHO_LED_TEST_BINARY:-./build/libreecho-ledd} --foreground --frame-file "$frame_path" --socket "$socket_path" \
    >"$log_path" 2>&1 &
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

LIBREECHO_LED_TEST_SOCKET="$socket_path" LIBREECHO_LED_TEST_FRAME="$frame_path" python3 - <<'PY'
import json
import os
import socket

path = os.environ["LIBREECHO_LED_TEST_SOCKET"]
sequence = 0


def call(command, arguments=None):
    global sequence
    sequence += 1
    if command == "pattern" and arguments.get("name") != "stop":
        arguments = {"r": 255, "g": 0, "b": 0, "brightness": 60,
                     "repeats": 0, **arguments}
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(1)
    client.connect(path)
    request = {
        "v": 1,
        "id": sequence,
        "cmd": command,
        "args": arguments or {},
    }
    client.sendall(
        json.dumps(request, separators=(",", ":")).encode("ascii") + b"\n"
    )
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    parsed = json.loads(response)
    assert parsed["ok"], parsed
    return parsed.get("data", {})


import time


def ring():
    """The twelve pixels as ledd last wrote them to the frame file ("rrggbb" x 12)."""
    text = open(os.environ["LIBREECHO_LED_TEST_FRAME"]).read().strip()
    return [tuple(int(text[i + k:i + k + 2], 16) for k in (0, 2, 4)) for i in range(0, 72, 6)]


# Muted (a solid red ring), then the volume buttons: the level shows over the mute ring while the meter
# holds, then the red comes back -- else a muted Echo gives no sign that the volume changed.
call("pattern", {"name": "solid", "r": 255, "g": 0, "b": 0, "brightness": 60, "repeats": 0, "owner": "mute"})
time.sleep(0.5)
assert all(p[0] > 0 and p[1] == 0 for p in ring()), ring()
call("meter", {"value": 50, "r": 0, "g": 255, "b": 0, "brightness": 60, "hold_ms": 800, "owner": "volume"})
time.sleep(0.3)
during = ring()
assert any(p[1] > 0 and p[0] == 0 for p in during), during
time.sleep(1.2)
after = ring()
assert all(p[0] > 0 and p[1] == 0 for p in after), after
assert call("status")["pattern_owner"] == "mute"
PY

echo "LED meter over the mute ring: ok"
