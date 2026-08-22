#!/data/data/com.termux/files/usr/bin/sh
set -eu
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
echo "WaffleHouse-Client 3.2-Termux: clean rebuild/update"
exec sh "$ROOT_DIR/build.sh" --upgrade "$@"
