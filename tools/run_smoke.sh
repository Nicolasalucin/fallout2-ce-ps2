#!/usr/bin/env bash
# run_smoke.sh — run the PS2 smoke harness (F6) under PCSX2 headless.
#
# Usage:
#   tools/run_smoke.sh [BUILD_DIR]
#
#   BUILD_DIR   directory containing FALLOUT2.ELF (default: build_ps2)
#
# Environment:
#   PS2_SMOKE_TIMEOUT  whole-suite timeout in seconds (default: 600)
#   PS2_SMOKE_OPTS     extra PCSX2 arguments (default: -batch -nogui)
#   PS2_SMOKE_LOG      full emulator log path (default: /tmp/opencode/ps2_smoke.log)
#
# The emulator runs in the background while this script polls the log for the
# harness verdict ([SMOKE] SUITE_FINISHED_SUCCESS / SUITE_FAILED_*). As soon as
# the verdict (or a crash) is seen the emulator is stopped and the filtered
# [SMOKE]/[PERF] report is printed.
#
# Exit codes:
#   0  SUITE_FINISHED_SUCCESS
#   1  SUITE_FAILED_* (or the emulator died without a verdict)
#   2  ELF or PCSX2 not found / launch failure
#   3  Timeout

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_DIR}/build_ps2}"
ELF="${BUILD_DIR}/FALLOUT2.ELF"
LOG="${PS2_SMOKE_LOG:-/tmp/opencode/ps2_smoke.log}"
TIMEOUT="${PS2_SMOKE_TIMEOUT:-600}"
PCSX2_OPTS="${PS2_SMOKE_OPTS:--batch -nogui}"

if [[ ! -f "${ELF}" ]]; then
    echo "[run_smoke] ELF not found: ${ELF}" >&2
    exit 2
fi

# Locate PCSX2.
PCSX2_BIN=""
for cand in \
    "pcsx2-qt" \
    "pcsx2" \
    "${HOME}/.local/bin/pcsx2-qt" \
    "${HOME}/.local/bin/pcsx2" \
    "/usr/bin/pcsx2-qt" \
    "/usr/bin/pcsx2" \
    "/usr/local/bin/pcsx2-qt" \
    "/usr/local/bin/pcsx2"; do
    if command -v "${cand}" >/dev/null 2>&1; then
        PCSX2_BIN="${cand}"
        break
    fi
done

if [[ -z "${PCSX2_BIN}" ]]; then
    echo "[run_smoke] PCSX2 not found" >&2
    exit 2
fi

mkdir -p "$(dirname "${LOG}")" 2>/dev/null || true
: >"${LOG}"

kill_emulator() {
    flatpak kill net.pcsx2.PCSX2 >/dev/null 2>&1 || true
    pkill -f "FALLOUT2[.]ELF" >/dev/null 2>&1 || true
    if [[ -n "${PCSX2_PID:-}" ]]; then
        kill "${PCSX2_PID}" >/dev/null 2>&1 || true
    fi
    wait "${PCSX2_PID:-}" 2>/dev/null || true
}

trap kill_emulator EXIT

echo "[run_smoke] PCSX2: ${PCSX2_BIN} ${PCSX2_OPTS}"
echo "[run_smoke] ELF:   ${ELF}"
echo "[run_smoke] log:   ${LOG}"

"${PCSX2_BIN}" ${PCSX2_OPTS} "${ELF}" >"${LOG}" 2>&1 &
PCSX2_PID=$!

VERDICT=""
DEADLINE=$((SECONDS + TIMEOUT))

while (( SECONDS < DEADLINE )); do
    if grep -Eq "SUITE_(FINISHED_SUCCESS|FAILED_)" "${LOG}" 2>/dev/null; then
        VERDICT="$(grep -Eo "SUITE_(FINISHED_SUCCESS|FAILED_[^\"]*)" "${LOG}" | tail -n 1)"
        break
    fi
    if ! kill -0 "${PCSX2_PID}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill_emulator
trap - EXIT

echo "[run_smoke] ---- filtered report ----"
grep -E "\[SMOKE\]|\[PERF\]" "${LOG}" || true

if [[ -z "${VERDICT}" ]]; then
    echo "[run_smoke] ---- last 40 log lines ----" >&2
    tail -n 40 "${LOG}" >&2
    if (( SECONDS >= DEADLINE )); then
        echo "[run_smoke] TIMEOUT after ${TIMEOUT}s" >&2
        exit 3
    fi
    echo "[run_smoke] emulator exited without a verdict" >&2
    exit 1
fi

echo "[run_smoke] verdict: ${VERDICT}"

case "${VERDICT}" in
    SUITE_FINISHED_SUCCESS) exit 0 ;;
    *) exit 1 ;;
esac
