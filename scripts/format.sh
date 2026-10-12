#!/usr/bin/env bash
# Check (default) or auto-fix formatting of all C++ sources with clang-format.
# Usage:
#   scripts/format.sh          # check only, exits 1 if anything is misformatted
#   scripts/format.sh --fix    # rewrite files in place
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
MODE="check"

if [[ "${1:-}" == "--fix" ]]; then
    MODE="fix"
fi

FILES=()
while IFS= read -r f; do
    FILES+=("$f")
done < <(git ls-files '*.h' '*.hpp' '*.c' '*.cc' '*.cpp')

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No C++ files found."
    exit 0
fi

if [[ "$MODE" == "fix" ]]; then
    "$CLANG_FORMAT" -i "${FILES[@]}"
    echo "Formatted ${#FILES[@]} files."
    exit 0
fi

FAILED=()
for f in "${FILES[@]}"; do
    if ! diff -q "$f" <("$CLANG_FORMAT" "$f") >/dev/null 2>&1; then
        FAILED+=("$f")
    fi
done

if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "Needs formatting:"
    printf '  %s\n' "${FAILED[@]}"
    echo
    echo "Run: scripts/format.sh --fix"
    exit 1
fi

echo "All ${#FILES[@]} files are correctly formatted."
