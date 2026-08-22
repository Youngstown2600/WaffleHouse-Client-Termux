#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/terminalui.cpp"

BLOCK=$(sed -n '/command == QStringLiteral("join") || command == QStringLiteral("j")/,/if (command == QStringLiteral("joinprivate"))/p' "$SRC")
printf '%s\n' "$BLOCK" | grep -F 'entry->settings.protocol == ConnectionSettings::Protocol::Oscar' >/dev/null
printf '%s\n' "$BLOCK" | grep -F 'const bool privateRoom =' >/dev/null
printf '%s\n' "$BLOCK" | grep -F 'entry->backend->joinRoom(room, privateRoom);' >/dev/null
printf '%s\n' "$BLOCK" | grep -F 'entry->settings.protocol == ConnectionSettings::Protocol::Irc' >/dev/null
printf '%s\n' "$BLOCK" | grep -F "room.prepend(QLatin1Char('#'));" >/dev/null

grep -F 'QStringLiteral("  /join ROOM                   IRC channel; AIM private chatroom")' "$SRC" >/dev/null
printf '%s\n' 'cli AIM /join private-room regression test passed'
