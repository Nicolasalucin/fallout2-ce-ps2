#!/usr/bin/env bash
# make_dist.sh — create the release bundle for Fallout 2 CE PS2
#
# Usage:
#   tools/make_dist.sh <version> [build_dir]
#
#   version     version string, e.g. 1.0.2
#   build_dir   directory containing FALLOUT2.ELF (default: build_ps2_prod)
#
# The script:
#   1. Verifies the source ELF exists and is a release build (no smoke markers)
#   2. Copies the ELF to dist/FALLOUT2/
#   3. Verifies the ELF contains hardening strings
#   4. Creates the ZIP from dist/FALLOUT2/
#   5. Prints SHA-256 of both ELF and ZIP

set -euo pipefail

VERSION="${1:?usage: tools/make_dist.sh <version> [build_dir]}"
BUILD_DIR="${2:-build_ps2_prod}"
ELF="${BUILD_DIR}/FALLOUT2.ELF"
DIST_DIR="dist/FALLOUT2"
ZIP="dist/fallout2-ce-ps2-v${VERSION}.zip"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "[make_dist] Version: ${VERSION}"
echo "[make_dist] Build dir: ${BUILD_DIR}"
echo "[make_dist] ELF: ${ELF}"
echo "[make_dist] Dist dir: ${DIST_DIR}"
echo "[make_dist] ZIP: ${ZIP}"

# 1. Source ELF must exist
if [[ ! -f "${ELF}" ]]; then
    echo -e "${RED}[make_dist] ERROR: ELF not found: ${ELF}${NC}" >&2
    exit 1
fi

# 2. Dist dir must exist with required files
for req in "${DIST_DIR}/README_PS2.txt" "${DIST_DIR}/SYSTEM.CNF"; do
    if [[ ! -f "${req}" ]]; then
        echo -e "${RED}[make_dist] ERROR: Required file missing: ${req}${NC}" >&2
        exit 1
    fi
done

# 3. Freshness: the ELF must be newer than the newest build input, otherwise
# a stale artifact could be packaged as a release.
NEWEST_SRC=$(find src cmake CMakeLists.txt -type f \
    \( -name "*.c" -o -name "*.cc" -o -name "*.S" -o -name "*.h" -o -name "*.txt" -o -name "*.cmake" \) \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | awk '{print $2}')
if [[ -n "${NEWEST_SRC}" && "${NEWEST_SRC}" -nt "${ELF}" ]]; then
    echo -e "${RED}[make_dist] ERROR: ${ELF} is older than ${NEWEST_SRC}${NC}" >&2
    echo -e "${RED}[make_dist]        Rebuild before packaging.${NC}" >&2
    exit 1
fi
echo -e "${GREEN}[make_dist] OK: ELF is newer than all build inputs${NC}"

# 4. Release ELF must NOT contain smoke harness markers
if grep -aq "SUITE_FINISHED" "${ELF}"; then
    echo -e "${RED}[make_dist] ERROR: Smoke marker (SUITE_FINISHED) found in release ELF${NC}" >&2
    exit 1
fi
echo -e "${GREEN}[make_dist] OK: No smoke markers in ELF${NC}"

# 5. Release ELF MUST contain hardening strings (proves it's the new build)
for str in \
    "waiting for mass" \
    "mass:/debug.log" \
    "R5900 vectors installed" \
    "stack: top=" \
    "GS PRESENT TIMEOUT" \
    "gsKit could not reserve"; do
    if ! grep -aq "${str}" "${ELF}"; then
        echo -e "${RED}[make_dist] ERROR: Hardening string not found: ${str}${NC}" >&2
        exit 1
    fi
done
echo -e "${GREEN}[make_dist] OK: All hardening strings present in ELF${NC}"

# 6. Copy the fresh ELF to dist/
cp -f "${ELF}" "${DIST_DIR}/FALLOUT2.ELF"
echo "[make_dist] Copied ELF to ${DIST_DIR}/FALLOUT2.ELF"

# 7. Verify dist ELF matches build ELF (byte-identical => identical SHA-256)
if ! cmp -s "${ELF}" "${DIST_DIR}/FALLOUT2.ELF"; then
    echo -e "${RED}[make_dist] ERROR: dist ELF differs from build ELF${NC}" >&2
    exit 1
fi
echo -e "${GREEN}[make_dist] OK: dist ELF matches build ELF${NC}"

# 8. Create ZIP from dist/ (reproducible)
rm -f "${ZIP}"
( cd dist && python3 -m zipfile -c "fallout2-ce-ps2-v${VERSION}.zip" FALLOUT2 )
echo "[make_dist] Created ZIP: ${ZIP}"

# 9. Print SHA-256
echo ""
echo "=== SHA-256 CHECKSUMS ==="
BUILD_SHA=$(sha256sum "${ELF}" | awk '{print $1}')
ELF_SHA=$(sha256sum "${DIST_DIR}/FALLOUT2.ELF" | awk '{print $1}')
ZIP_SHA=$(sha256sum "${ZIP}" | awk '{print $1}')
echo "build ${ELF}: ${BUILD_SHA}"
echo "dist  ${DIST_DIR}/FALLOUT2.ELF: ${ELF_SHA}"
echo "fallout2-ce-ps2-v${VERSION}.zip: ${ZIP_SHA}"
if [[ "${BUILD_SHA}" != "${ELF_SHA}" ]]; then
    echo -e "${RED}[make_dist] ERROR: build/dist ELF SHA-256 differ${NC}" >&2
    exit 1
fi
echo -e "${GREEN}[make_dist] OK: build and dist ELF SHA-256 are identical${NC}"
echo ""

# 10. Verify ZIP contents
echo "=== ZIP CONTENTS ==="
python3 -c "
import zipfile
z = zipfile.ZipFile('${ZIP}')
for info in z.infolist():
    print(f'{info.filename}  {info.file_size} bytes  {info.date_time}')
"
echo ""

echo -e "${GREEN}[make_dist] Bundle ready: ${ZIP}${NC}"
echo -e "${YELLOW}Next steps:${NC}"
echo "  git add dist/FALLOUT2/FALLOUT2.ELF dist/fallout2-ce-ps2-v${VERSION}.zip"
echo "  git commit -m 'release: Fallout 2 CE PS2 v${VERSION}'"
echo "  git tag -a v${VERSION}-ps2 -m 'Fallout 2 CE PS2 v${VERSION}: hardware boot hardening'"
echo "  git push origin ps2 && git push origin v${VERSION}-ps2"
echo "  gh release create v${VERSION}-ps2 ${ZIP} --latest --title 'Fallout 2 CE PS2 v${VERSION} — Hardware Boot Hardening' --notes-file <notes.md>"