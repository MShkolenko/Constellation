#!/bin/bash
# Wire this module into a core checkout. Idempotent.
#   tools/integrate.sh /opt/algalon/src [/path/to/this/repo]
set -u
CORE="${1:?usage: integrate.sh <core-src-dir> [module-dir]}"
SELF="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
CUSTOM="$CORE/src/server/scripts/Custom"

[ -d "$CUSTOM" ] || { echo "not a core tree: $CUSTOM missing"; exit 1; }

# 1. symlink the module sources into the Custom subtree
if [ -L "$CUSTOM/Constellation" ]; then
    echo "symlink already present: $(readlink "$CUSTOM/Constellation")"
elif [ -e "$CUSTOM/Constellation" ]; then
    echo "REFUSING: $CUSTOM/Constellation exists and is not a symlink"; exit 1
else
    ln -s "$SELF/src" "$CUSTOM/Constellation"
    echo "linked: $CUSTOM/Constellation -> $SELF/src"
fi

# 2. verify the guarded hook is present in the fork
grep -q "Constellation/Registration.h" "$CUSTOM/custom_script_loader.cpp" \
    && echo "loader hook: present" \
    || { echo "loader hook: MISSING — the fork needs its one guarded commit"; exit 1; }

echo "re-run cmake so the new sources are collected"
