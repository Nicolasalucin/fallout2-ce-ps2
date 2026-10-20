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
#   PS2_SMOKE_FASTMEM  set to 1 to re-enable EE fastmem for A/B diagnosis
#                      only (default: 0 = strict MMU/TLB path)
#
# Strict profile (applied to PCSX2.ini, restored on exit):
#   - fast memory off: EnableFastmem=false forces the MMU/TLB path, so an
#     unaligned 32/64/128-bit access or a null/bad pointer faults immediately
#     instead of being absorbed by the host's flat memory mapping
#   - speedhacks off: EE cycle rate 100%, no cycle skip, no instruction-skip
#     hints (WaitLoop/vuFlagHack/vu1Instant/vuThread), no fast CDVD
#   - FPU/VU: round-to-nearest with full clamp (overflow + extra + preserve
#     sign) and denormal flushing disabled, i.e. strict IEEE-754 behaviour
#   - SPU2 output without time-stretch (SyncMode=Disabled)
#   - GS host-refresh sync and vsync timing off
#   - verbose logging: system console, file log, EE/IOP console, SPU2 debug
#
# Note: PCSX2 2.8 has no single "SpeedHacks" ini key; the individual
# [EmuCore/Speedhacks] toggles below are the equivalent. Likewise the
# FPU/VU clamp modes are stored as fpu*/vu* booleans in
# [EmuCore/CPU/Recompiler], not as a numeric Clampmode key.
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
FASTMEM="${PS2_SMOKE_FASTMEM:-0}"
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
import os
import re
import sys

path = sys.argv[1]
fastmem = "true" if os.environ.get("PS2_SMOKE_FASTMEM", "0") == "1" else "false"

settings = {
    "EmuCore/Speedhacks": {
        "EECycleRate": "0",
        "EECycleSkip": "0",
        "WaitLoop": "false",
        "vuFlagHack": "false",
        "vu1Instant": "false",
        "vuThread": "false",
        "IntcStat": "false",
        "fastCDVD": "false",
    },
    # Fastmem off reproduces the real EE MMU/TLB path: bad or unaligned
    # accesses become TLB misses instead of silently succeeding.
    # Clamp mode 3 = full (overflow + extra overflow + preserve sign).
    "EmuCore/CPU/Recompiler": {
        "EnableFastmem": fastmem,
        "fpuOverflow": "true",
        "fpuExtraOverflow": "true",
        "fpuFullMode": "true",
        "vu0Overflow": "true",
        "vu0ExtraOverflow": "true",
        "vu0SignOverflow": "true",
        "vu0Underflow": "true",
        "vu1Overflow": "true",
        "vu1ExtraOverflow": "true",
        "vu1SignOverflow": "true",
        "vu1Underflow": "true",
    },
    # Round-to-nearest everywhere; DAZ off so denormals are not silently
    # flushed to zero (strict IEEE-754 / EE FPU behaviour).
    "EmuCore/CPU": {
        "FPU.Roundmode": "0",
        "FPUDiv.Roundmode": "0",
        "VU0.Roundmode": "0",
        "VU1.Roundmode": "0",
        "FPU.DenormalsAreZero": "false",
        "FPUDiv.DenormalsAreZero": "false",
        "VU0.DenormalsAreZero": "false",
        "VU1.DenormalsAreZero": "false",
    },
    "EmuCore/Gamefixes": {
        "InstantDMAHack": "false",
    },
    "EmuCore/GS": {
        "SyncToHostRefreshRate": "false",
        "UseVSyncForTiming": "false",
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

# Fail loudly if PCSX2's INI schema changes: a silently ignored key would
# turn the hyper-strict run back into a permissive one without anyone
# noticing (the suite can still pass while a crash source is masked).
if [[ "${STRICT}" == "1" ]]; then
    verify_setting() {
        local section="$1"
        local key="$2"
        local value="$3"

        awk -v want_section="[${section}]" -v want_key="${key}" -v want_value="${value}" '
            $0 == want_section { in_section = 1; next }
            /^\[/ { in_section = 0 }
            in_section {
                split($0, parts, "=")
                gsub(/^[ \t]+|[ \t]+$/, "", parts[1])
                gsub(/^[ \t]+|[ \t]+$/, "", parts[2])
                if (parts[1] == want_key && parts[2] == want_value) {
                    found = 1
                }
            }
            END { exit found ? 0 : 1 }
        ' "${PCSX2_INI}" || {
            echo "[run_smoke] strict profile not applied: [${section}] ${key} = ${value}" >&2
            exit 2
        }
    }

    if [[ "${FASTMEM}" == "1" ]]; then
        echo "[run_smoke] WARNING: fastmem re-enabled for A/B diagnosis" >&2
        verify_setting "EmuCore/CPU/Recompiler" "EnableFastmem" "true"
    else
        verify_setting "EmuCore/CPU/Recompiler" "EnableFastmem" "false"
    fi
    verify_setting "EmuCore/Speedhacks" "EECycleRate" "0"
    verify_setting "EmuCore/Speedhacks" "EECycleSkip" "0"
    verify_setting "EmuCore/Speedhacks" "vuThread" "false"
    verify_setting "EmuCore/CPU/Recompiler" "fpuFullMode" "true"
    verify_setting "EmuCore/CPU/Recompiler" "vu0SignOverflow" "true"
    verify_setting "EmuCore/CPU" "FPU.Roundmode" "0"
    verify_setting "EmuCore/CPU" "VU1.Roundmode" "0"
    verify_setting "EmuCore/Gamefixes" "InstantDMAHack" "false"
    verify_setting "EmuCore/GS" "SyncToHostRefreshRate" "false"
    if [[ "${FASTMEM}" == "1" ]]; then
        echo "[run_smoke] strict profile verified (fastmem ON for diagnosis, full FPU/VU clamp, no speedhacks)"
    else
        echo "[run_smoke] hyper-strict profile verified (fastmem off, full FPU/VU clamp, no speedhacks)"
    fi
fi

echo "[run_smoke] PCSX2: ${PCSX2_BIN} ${PCSX2_OPTS}"
echo "[run_smoke] ELF:   ${ELF}"
echo "[run_smoke] log:   ${LOG}"

# --------------------------------------------------------------- launch ----
"${PCSX2_BIN}" ${PCSX2_OPTS} -logfile "${LOG}" "${ELF}" >"${CONSOLE_LOG}" 2>&1 &
PCSX2_PID=$!

VERDICT=""
CRASH=""
TLB_WARNED=""
DEADLINE=$((SECONDS + TIMEOUT))

while (( SECONDS < DEADLINE )); do
    if grep -Eq "SUITE_(FINISHED_SUCCESS|FAILED_)" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null; then
        VERDICT="$(grep -Eh "SUITE_(FINISHED_SUCCESS|FAILED_)" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null \
            | grep -Eo "SUITE_(FINISHED_SUCCESS|FAILED_[A-Z_]*)" | tail -n 1)"
        break
    fi
    # TLB misses are reported but not treated as fatal here: PCSX2 logs them
    # and keeps executing unless PauseOnTLBMiss is set. A fatal fault shows
    # up as ReportErrorAsync / Jump to unaligned address / Bus Error.
    if [[ -z "${TLB_WARNED}" ]] && grep -Eq "TLB Miss" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null; then
        echo "[run_smoke] WARNING: EE TLB miss observed with fastmem off" >&2
        TLB_WARNED="1"
    fi
    if grep -Eq "ReportErrorAsync|Jump to unaligned address|Bus Error" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null; then
        CRASH="1"
        break
    fi
    if ! kill -0 "${PCSX2_PID}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill_emulator
trap - EXIT

# `grep -c` exits 1 when there are no matches; keep `set -e`/pipefail from
# aborting a clean run before the verdict is printed.
TLB_COUNT="$( (grep -hc "TLB Miss" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null || true) \
    | awk '{s += $1} END {print s + 0}')"

if [[ -n "${CRASH}" ]]; then
    echo "[run_smoke] ---- EMULATOR HARDWARE FAULT (strict profile) ----" >&2
    grep -Eh "TLB Miss|ReportErrorAsync|Jump to unaligned|Bus Error|Exception" \
        "${LOG}" "${CONSOLE_LOG}" 2>/dev/null | awk '!seen[$0]++' | head -n 12 >&2
    PC="$(grep -Eho "pc=0x[0-9a-fA-F]+" "${LOG}" 2>/dev/null | head -n 1 | cut -d= -f2)"
    if [[ -n "${PC}" ]]; then
        SYMBOLIZED=""
        if command -v mips64r5900el-ps2-elf-addr2line >/dev/null 2>&1; then
            echo "[run_smoke] fault PC ${PC}:" >&2
            mips64r5900el-ps2-elf-addr2line -f -C -e "${ELF}" "${PC}" >&2 && SYMBOLIZED=1 || true
        fi
        if [[ -z "${SYMBOLIZED}" ]] && docker inspect -f '{{.State.Running}}' fo2-ps2-build 2>/dev/null | grep -q true; then
            REL_ELF="${ELF#${REPO_DIR}/}"
            echo "[run_smoke] fault PC ${PC} (via docker):" >&2
            docker exec fo2-ps2-build mips64r5900el-ps2-elf-addr2line \
                -f -C -e "/src/${REL_ELF}" "${PC}" >&2 || true
        fi
    fi
    echo "[run_smoke] verdict: EMULATOR_HARDWARE_FAULT" >&2
    exit 1
fi

# A successful harness verdict with even one TLB miss means the strict profile
# caught a real bad access (fastmem would have hidden it). Treat it as a
# failure so a later run cannot silently regress.
if [[ "${VERDICT}" == "SUITE_FINISHED_SUCCESS" && "${TLB_COUNT}" -gt 0 ]]; then
    echo "[run_smoke] FAIL: ${TLB_COUNT} EE TLB miss line(s) with fastmem off" >&2
    grep -Eh "TLB Miss" "${LOG}" "${CONSOLE_LOG}" 2>/dev/null | awk '!seen[$0]++' | head -n 12 >&2
    VERDICT="SUITE_FAILED_TLB_MISS"
fi

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
