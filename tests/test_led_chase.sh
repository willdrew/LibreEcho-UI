#!/bin/sh
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/led.sock"
log_path="$test_dir/led.log"
pid=0

cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rmdir "$test_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

${LIBREECHO_LED_TEST_BINARY:-./build/libreecho-ledd} --foreground --stub --socket "$socket_path" \
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

LIBREECHO_LED_TEST_SOCKET="$socket_path" python3 - <<'PY'
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


def raw(command, arguments):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(1)
    client.connect(path)
    client.sendall(json.dumps({"v": 1, "id": 99, "cmd": command,
                               "args": arguments}).encode("ascii") + b"\n")
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    return json.loads(response)


import time


def lit(status):
    return [max(p["r"], p["g"], p["b"]) for p in status["pixels"]]


# Chase: a comet goes round, so the brightest pixel moves and the ring is
# never uniform while it runs; after its laps the ring goes back.
call("pattern", {"name": "chase", "r": 255, "g": 0, "b": 0, "brightness": 100,
                 "repeats": 2, "period_ms": 600, "owner": "effect"})
heads = set()
for _ in range(6):
    status = call("status")
    assert status["pattern"] == "chase", status
    levels = lit(status)
    assert max(levels) > 0 and min(levels) < max(levels), levels
    heads.add(levels.index(max(levels)))
    time.sleep(0.1)
assert len(heads) >= 3, heads
time.sleep(0.8)
assert call("status")["pattern_active"] is False

# Scanner: a front-to-back bar sweeps left and right (the front centre is
# between pixels 10 and 11, the far left between 7 and 8), so pixel i and
# its mirror across the left-right line, 3 - i, always match.
call("pattern", {"name": "scanner", "r": 0, "g": 0, "b": 255, "brightness": 100,
                 "repeats": 0, "owner": "effect"})
for _ in range(5):
    levels = lit(call("status"))
    assert max(levels) > 0, levels
    for i in range(12):
        assert abs(levels[i] - levels[(3 - i) % 12]) <= 2, levels
    time.sleep(0.13)
# A flash on top returns to the scanner, which runs until stopped.
call("pattern", {"name": "flash", "r": 255, "g": 170, "b": 0,
                 "brightness": 70, "repeats": 1, "owner": "missed"})
time.sleep(0.6)
assert call("status")["pattern"] == "scanner"
call("pattern", {"name": "stop", "owner": "effect"})
assert call("status")["pattern_active"] is False

bad = raw("pattern", {"name": "chase", "r": 1, "g": 1, "b": 1, "brightness": 50,
                      "repeats": 0, "period_ms": 50})
assert not bad["ok"], bad
PY

echo "LED chase and scanner patterns: ok"
