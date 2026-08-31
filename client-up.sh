#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
printf '%s\n' 'WaffleHouse-Termux 1.0: rebuilding/upgrading from this source tree.'
exec "$ROOT_DIR/build.sh" --upgrade "$@"
