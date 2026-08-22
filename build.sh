#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/build-termux.sh" "$@"
