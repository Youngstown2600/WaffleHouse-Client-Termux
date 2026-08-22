#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

cat <<'EOF2'
WaffleHouse-Client: client-up.sh is now a compatibility wrapper.
Upgrade/uninstall logic lives in build.sh so there is only one lifecycle path to maintain.
EOF2

exec "$ROOT_DIR/build.sh" --upgrade "$@"
