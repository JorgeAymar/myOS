#!/usr/bin/env bash
# Headless boot test. Boots the ISO under BIOS and UEFI, captures serial output,
# and passes only if the kernel printed "[TEST] boot OK" and exited via
# isa-debug-exit with the success code (0x10 -> QEMU exit status 33).
set -u
TIMEOUT="${TIMEOUT:-30}"
LOG_DIR="build"
PASS_MARK="[TEST] boot OK"
status=0

run_one() {
    local name="$1"; shift
    local log="$LOG_DIR/serial-$name.log"
    # shellcheck disable=SC2086
    $QEMU $QEMU_ARGS "$@" -cdrom "$ISO" > "$log" 2>&1 &
    local pid=$!
    ( sleep "$TIMEOUT"; kill "$pid" 2>/dev/null ) &
    local watchdog=$!
    wait "$pid"; local code=$?
    kill "$watchdog" 2>/dev/null; wait "$watchdog" 2>/dev/null

    if [[ $code -eq 33 ]] && grep -qF "$PASS_MARK" "$log"; then
        echo "PASS [$name]"
    else
        echo "FAIL [$name] (exit=$code, log=$log)"
        sed 's/^/    | /' "$log" | tail -20
        status=1
    fi
}

run_one bios
if [[ -f "${OVMF:-}" ]]; then
    run_one uefi -drive "if=pflash,unit=0,format=raw,file=$OVMF,readonly=on"
else
    echo "SKIP [uefi] (OVMF firmware not found)"
fi
exit $status
