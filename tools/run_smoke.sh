#!/usr/bin/env bash
# run_smoke.sh — run the PS2 smoke harness under PCSX2 in strict mode.
#
# Usage:
#   tools/run_smoke.sh [BUILD_DIR]
#
#   BUILD_DIR   directory containing FALLOUT2.ELF (default: build_ps2)
#
# Environment:
#   PS2_SMOKE_TIMEOUT  whole-suite timeout in seconds (default: 600)
#   PS2_SMOKE_OPTS     extra PCSX2 arguments (default: -batch -nogui)
#   PS2_SMOKE_LOG      full emulator log path
#                      (default: build_ps2/pcsx2_strict.log)
#   PS2_SMOKE_STRICT   set to 0 to skip the strict PCSX2 profile (default: 1)
#
# Strict profile (applied to PCSX2.ini, restored on exit):
#   - speedhacks off: EE cycle rate 100%, no cycle skip, no instruction
#     skip hints (WaitLoop/vuFlagHack/vu1Instant), no fast CDVD
#   - SPU2 output without time-stretch (SyncMode=Disabled)
#   - verbose logging: system console, file log, EE/IOP console, SPU2 debug
#
# The emulator runs in the background while this script polls the log(s) for
# the harness verdict ([SMOKE] SUITE_FINISHED_SUCCESS / SUITE_FAILED_*). As
# soon as the verdict (or a crash) is seen the emulator is stopped, PCSX2.ini
# is restored and the filtered [SMOKE]/[PERF] report is printed.
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
LOG="${PS2_SMOKE_LOG:-${REPO_DIR}/build_ps2/pcsx2_strict.log}"
CONSOLE_LOG="${LOG%.log}.console.log"
TIMEOUT="${PS2_SMOKE_TIMEOUT:-600}"
PCSX2_OPTS="${PS2_SMOKE_OPTS:--batch -nogui}"
STRICT="${PS2_SMOKE_STRICT:-1}"
FLATPAK_ID="net.pcsx2.PCSX2"
PCSX2_INI=""
PCSX2_INI_BACKUP="${REPO_DIR}/build_ps2/PCSX2.ini.pre-smoke"

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

# Locate the settings file (flatpak install first, then a native install).
for cand in \
    "${HOME}/.var/app/${FLATPAK_ID}/config/PCSX2/PCSX2.ini" \
    "${HOME}/.config/PCSX2/PCSX2.ini"; do
    if [[ -f "${cand}" ]]; then
        PCSX2_INI="${cand}"
        break
    fi
done

if [[ "${STRICT}" == "1" && -z "${PCSX2_INI}" ]]; then
    echo "[run_smoke] PCSX2.ini not found; strict profile impossible" >&2
    exit 2
fi

mkdir -p "$(dirname "${LOG}")" 2>/dev/null || true
: >"${LOG}"
: >"${CONSOLE_LOG}"

wait_for_pcsx2_exit() {
    local i

    # PCSX2 writes its settings on shutdown; restoring PCSX2.ini too early
    # would let that final save overwrite the profile.
    for i in $(seq 1 30); do
        if ! flatpak ps --columns=application 2>/dev/null | grep -qx "${FLATPAK_ID}"; then
            return 0
        fi
        sleep 1
    done

    return 0
}

restore_environment() {
    if [[ -f "${PCSX2_INI_BACKUP}" ]]; then
        cp -f "${PCSX2_INI_BACKUP}" "${PCSX2_INI}" 2>/dev/null || true
        rm -f "${PCSX2_INI_BACKUP}"
    fi
}

kill_emulator() {
    flatpak kill "${FLATPAK_ID}" >/dev/null 2>&1 || true
    wait_for_pcsx2_exit
    pkill -f "FALLOUT2[.]ELF" >/dev/null 2>&1 || true
    if [[ -n "${PCSX2_PID:-}" ]]; then
        kill "${PCSX2_PID}" >/dev/null 2>&1 || true
    fi
    wait "${PCSX2_PID:-}" 2>/dev/null || true
    restore_environment

    # PCSX2's -logfile already holds the app log; append the process console
    # so a single file carries the full emulator output (F7 workflow).
    if [[ -f "${CONSOLE_LOG}" ]]; then
        cat "${CONSOLE_LOG}" >>"${LOG}" 2>/dev/null || true
    fi
}

trap kill_emulator EXIT

# ------------------------------------------------------- strict PCSX2 ------
if [[ "${STRICT}" == "1" ]]; then
    cp -f "${PCSX2_INI}" "${PCSX2_INI_BACKUP}"
    echo "[run_smoke] applying strict PCSX2 profile (backup: ${PCSX2_INI_BACKUP})"

    python3 - "${PCSX2_INI}" <<'PY' || exit 2
import re
import sys

path = sys.argv[1]

settings = {
    "EmuCore/Speedhacks": {
        "EECycleRate": "0",
        "EECycleSkip": "0",
        "WaitLoop": "false",
        "vuFlagHack": "false",
        "vu1Instant": "false",
        "IntcStat": "false",
        "fastCDVD": "false",
    },
    "SPU2/Output": {
        "SyncMode": "Disabled",
    },
    "SPU2/Debug": {
        "Global_Enable": "true",
        "Show_Messages": "true",
        "Log_DMA_Transfers": "true",
        "Log_Register_Access": "true",
    },
    "Logging": {
        "EnableSystemConsole": "true",
        "EnableFileLogging": "true",
        "EnableVerbose": "true",
        "EnableEEConsole": "true",
        "EnableIOPConsole": "true",
    },
}

with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
    lines = f.read().splitlines()

current = None
seen = {section: set() for section in settings}
out = []


def flush_missing():
    if current in settings:
        for key, value in settings[current].items():
            if key not in seen[current]:
                out.append("%s = %s" % (key, value))
                seen[current].add(key)


header_re = re.compile(r"^\s*\[(.+?)\]\s*$")
option_re = re.compile(r"^(\s*)([^=]+?)\s*=\s*(.*)$")

for line in lines:
    header = header_re.match(line)
    if header:
        flush_missing()
        current = header.group(1)
        out.append(line)
        continue

    if current in settings:
        option = option_re.match(line)
        if option and option.group(2) in settings[current]:
            key = option.group(2)
            out.append("%s%s = %s" % (option.group(1), key, settings[current][key]))
            seen[current].add(key)
            continue

    out.append(line)

flush_missing()

with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
    f.write("\n".join(out) + "\n")
PY
fi

echo "[run_smoke] PCSX2: ${PCSX2_BIN} ${PCSX2_OPTS}"
echo "[run_smoke] ELF:   ${ELF}"
echo "[run_smoke] log:   ${LOG}"

# --------------------------------------------------------------- launch ----
"${PCSX2_BIN}" ${PCSX2_OPTS} -logfile "${LOG}" "${ELF}" >"${CONSOLE_LOG}" 2>&1 &
PCSX2_PID=$!

VERDICT=""
DEADLINE=$((SECONDS + TIMEOUT))

while (( SECONDS < DEADLINE )); do
    if grep -Eq "SUITE_(FINISHED_SUCCESS|FAILED_)" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null; then
        VERDICT="$(grep -Eh "SUITE_(FINISHED_SUCCESS|FAILED_)" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null \
            | grep -Eo "SUITE_(FINISHED_SUCCESS|FAILED_[A-Z_]*)" | tail -n 1)"
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
grep -Eh "\[SMOKE\]|\[PERF\]" "${LOG}" 2>/dev/null | awk '!seen[$0]++' || true

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
