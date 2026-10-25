#!/bin/sh
# capture-mux built for the Echo Spot (-DLE_TARGET_ROOK): PCM 22 has six 24-bit lanes, four
# microphones (0-3, averaged by micd, no beamformer) and the playback loopback (4, 5). An
# injected phrase must reach all four microphone lanes and leave the loopback lanes silent,
# at the codec's frame size, so micd and waked cannot tell it from the room.
set -eu

make BUILD=build-rook CPPFLAGS=-DLE_TARGET_ROOK build-rook/libreecho-capture-mux >/dev/null
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT INT TERM

# The codec: 50 periods of silence (640 frames x 6 lanes x 3 bytes), then it ends.
cat > "$test_dir/fake-tinycap" <<EOF
#!/bin/sh
echo "\$*" > "$test_dir/args"
head -c $((50 * 640 * 6 * 3)) /dev/zero
EOF
chmod +x "$test_dir/fake-tinycap"
mkdir -p "$test_dir/inject"
python3 -c '
import math, struct, sys
sys.stdout.buffer.write(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 1000 * i / 16000))) for i in range(16000)))
' > "$test_dir/inject/pending.raw"

./build-rook/libreecho-capture-mux --capture-bin "$test_dir/fake-tinycap" \
    --control-dir "$test_dir/inject" > "$test_dir/out.raw"

# tinycap must open the Spot's capture PCM with its six lanes, as micd itself does.
grep -q -- "-D 0 -d 22 -c 6 -r 16000 -b 24 -p 640 -n 4" "$test_dir/args" ||
    { echo "capture-mux rook: tinycap got: $(cat "$test_dir/args")"; exit 1; }
python3 - "$test_dir/out.raw" <<'EOF'
import math, sys
d = open(sys.argv[1], "rb").read()
frame = 6 * 3
assert len(d) == 50 * 640 * frame, f"{len(d)} bytes, want {50 * 640 * frame} (six lanes)"
def lane(c, n=8000):
    out = []
    for i in range(n):
        b = d[i * frame + c * 3: i * frame + c * 3 + 3]
        v = int.from_bytes(b, "little", signed=True) >> 8
        out.append(v)
    return out
def amp(x):
    re = sum(v * math.cos(2 * math.pi * 1000 * i / 16000) for i, v in enumerate(x))
    im = sum(v * math.sin(2 * math.pi * 1000 * i / 16000) for i, v in enumerate(x))
    return 2 * math.hypot(re, im) / len(x)
for c in range(4):
    a = amp(lane(c))
    assert 7000 < a < 9000, f"mic lane {c}: 1 kHz at {a:.0f}, want ~8000"
for c in (4, 5):
    assert max(abs(v) for v in lane(c)) == 0, f"loopback lane {c} not silent"
print("capture-mux rook: injected audio on mic lanes 0-3, loopback lanes silent, six-lane frames")
EOF

# Mute: the Spot's codec has no capture switch, so audiod's mute is a flag file the mux obeys.
# While <control-dir>/../mic-muted exists, every microphone sample it forwards is zero.
cat > "$test_dir/noisy-tinycap" <<EOF2
#!/bin/sh
head -c $((50 * 640 * 6 * 3)) /dev/urandom
EOF2
chmod +x "$test_dir/noisy-tinycap"
rm -f "$test_dir/inject/"*
touch "$test_dir/mic-muted"
LE_CAPTURE_MUX_MUTE_FILE="$test_dir/mic-muted" ./build-rook/libreecho-capture-mux \
    --capture-bin "$test_dir/noisy-tinycap" --control-dir "$test_dir/inject" > "$test_dir/muted.raw"
python3 - "$test_dir/muted.raw" <<'EOF2'
import sys
d = open(sys.argv[1], "rb").read()
assert len(d) == 50 * 640 * 18, f"{len(d)} bytes"
nonzero = sum(1 for b in d if b)
assert nonzero == 0, f"muted output has {nonzero} nonzero bytes"
print("capture-mux rook: muted, the room's audio becomes digital silence")
EOF2
# Unmuted, the room passes through untouched.
rm -f "$test_dir/mic-muted"
LE_CAPTURE_MUX_MUTE_FILE="$test_dir/mic-muted" ./build-rook/libreecho-capture-mux \
    --capture-bin "$test_dir/noisy-tinycap" --control-dir "$test_dir/inject" > "$test_dir/open.raw"
python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); assert sum(1 for b in d if b) > len(d)//2, "unmuted output is silent"; print("capture-mux rook: unmuted, the room passes through")' "$test_dir/open.raw"
