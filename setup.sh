#!/usr/bin/env bash
# mp6-native local setup tool -- Linux/macOS (and Git Bash) launcher.
# See setup/README.md. NOTE: the actual native Windows exe build
# (tools/build.py) currently requires a Windows host -- see setup/README.md's
# "Platform status" section. This launcher still runs the cross-platform
# parts (prereq check, decomp clone, disc extraction) on any host, and gives
# an honest, specific message if it reaches a Windows-only step elsewhere.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PY=""
for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1; then
        PY="$cand"
        break
    fi
done

if [ -z "$PY" ]; then
    echo ""
    echo "Python 3 was not found on PATH."
    echo "Install it (e.g. 'sudo apt install python3', 'brew install python3'),"
    echo "then re-run this script."
    echo ""
    exit 1
fi

exec "$PY" "$SCRIPT_DIR/setup/setup.py" "$@"
