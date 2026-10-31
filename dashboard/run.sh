#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$DIR"

# Pick python with flask installed
if [ -f "/home/raj/miniconda3/bin/python" ]; then
    PYTHON="/home/raj/miniconda3/bin/python"
elif which python3 >/dev/null 2>&1; then
    PYTHON="python3"
else
    echo "Error: Python 3 not found."
    exit 1
fi

if nm -D "$DIR/libeduroute.so" 2>/dev/null | grep -q "__asan_init"; then
    LIBASAN=$(gcc -print-file-name=libasan.so 2>/dev/null || true)
    if [ -f "$LIBASAN" ]; then
        export LD_PRELOAD="$LIBASAN:$LD_PRELOAD"
    fi
fi

echo "=========================================================="
echo " Starting EduRoute C++ ABI Adaptive Exam & Dashboard Webapp"
echo " Python: $PYTHON"
echo " C++ Shared Library: $DIR/libeduroute.so"
echo "=========================================================="

exec "$PYTHON" app.py "$@"
