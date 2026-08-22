#!/data/data/com.termux/files/usr/bin/sh
set -eu
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec sh "$ROOT_DIR/build-termux.sh" "$@"
