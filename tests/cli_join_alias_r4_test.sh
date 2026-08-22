#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/terminalui.cpp"

grep -F 'QStringLiteral("/j")' "$SRC" >/dev/null
grep -F 'command == QStringLiteral("join") || command == QStringLiteral("j")' "$SRC" >/dev/null
grep -F 'QStringLiteral("  /j ROOM                      alias of /join")' "$SRC" >/dev/null
# Verify the alias shares the join branch rather than having a second backend implementation.
count=$(grep -F 'command == QStringLiteral("join") || command == QStringLiteral("j")' "$SRC" | wc -l | tr -d ' ')
[ "$count" = "1" ]
grep -A40 -F 'command == QStringLiteral("join") || command == QStringLiteral("j")' "$SRC" | grep -F 'entry->backend->joinRoom(room, privateRoom);' >/dev/null
printf '%s\n' 'cli /j join alias regression test passed'
