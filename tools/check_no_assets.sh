#!/bin/sh
# ---------------------------------------------------------------------------
# CI guard: refuse to track proprietary Fallout game assets.
# Run manually or from CI; exits non-zero if the git index contains any.
# ---------------------------------------------------------------------------
set -eu

# NOTE: proguard-rules.pro is a legitimate upstream Android build file whose
# extension collides with the asset pattern; it is explicitly allowlisted.
BAD_EXT=$(git ls-files | grep -iE '\.(dat|frm|acm|mve|map|pro|int|msg|sve|cfg)$' \
    | grep -v '^os/android/app/proguard-rules.pro$' || true)
BAD_DIR=$(git ls-files | grep -E '^(local_assets|data|mods|saves|build_ps2)/' || true)

if [ -n "$BAD_EXT" ] || [ -n "$BAD_DIR" ]; then
    echo "ERROR: proprietary game assets tracked in git:" >&2
    [ -n "$BAD_EXT" ] && echo "$BAD_EXT" >&2
    [ -n "$BAD_DIR" ] && echo "$BAD_DIR" >&2
    echo "Remove them from the index (git rm --cached) before committing." >&2
    exit 1
fi

echo "OK: no proprietary game assets tracked."
