#!/usr/bin/env bash
# Boots the ISO headless, waits, and dumps the screen to build/screenshot.png
# through QMP (QEMU's JSON control protocol). Usage: WAIT=5 make screenshot
set -u
WAIT="${WAIT:-4}"
OUT="build/screenshot.png"
SOCK="build/qmp.sock"
rm -f "$SOCK" "$OUT"

# shellcheck disable=SC2086
$QEMU $QEMU_ARGS -display none -cdrom "$ISO" -serial file:build/serial-screenshot.log \
    -qmp "unix:$SOCK,server,nowait" &
pid=$!
sleep "$WAIT"

python3 -I - "$SOCK" "$PWD/$OUT" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
f = s.makefile("rw")
def cmd(name, **args):
    f.write(json.dumps({"execute": name, "arguments": args}) + "\n"); f.flush()
    while True:
        msg = json.loads(f.readline())
        if "return" in msg or "error" in msg:
            if "error" in msg: print("QMP error:", msg["error"], file=sys.stderr)
            return msg
f.readline()  # greeting
cmd("qmp_capabilities")
cmd("screendump", filename=sys.argv[2], format="png")
try: cmd("quit")
except Exception: pass
EOF
wait "$pid" 2>/dev/null
if [[ -f "$OUT" ]]; then echo "Saved $OUT"; else echo "Screenshot failed"; exit 1; fi
