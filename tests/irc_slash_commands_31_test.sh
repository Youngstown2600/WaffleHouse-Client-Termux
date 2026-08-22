#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IRC="$ROOT/src/ircbackend.cpp"
IRCH="$ROOT/src/ircbackend.h"
GUI="$ROOT/src/mainwindow.cpp"
CHAT="$ROOT/src/chatwindow.cpp"
CLI="$ROOT/src/terminalui.cpp"

need() {
    file=$1
    text=$2
    if ! grep -Fq "$text" "$file"; then
        echo "FAIL: missing '$text' in ${file#$ROOT/}" >&2
        exit 1
    fi
}

need "$IRCH" 'bool handleSlashCommand(const QString &contextTarget, const QString &input);'
need "$IRC" 'QStringLiteral("/op")'
need "$IRC" 'QStringLiteral("/deop")'
need "$IRC" 'QStringLiteral("/kick")'
need "$IRC" 'QStringLiteral("/ban")'
need "$IRC" 'QStringLiteral("/unban")'
need "$IRC" 'QStringLiteral("/topic")'
need "$IRC" 'QStringLiteral("/mode")'
need "$IRC" 'QStringLiteral("MODE %1 %2%3 %4")'
need "$IRC" 'QStringLiteral("KICK %1 %2")'
need "$IRC" 'QStringLiteral("MODE %1 %2b %3")'
need "$IRC" 'QStringLiteral("482")'
need "$IRC" 'QStringLiteral("*** %1 sets mode %2")'
need "$CHAT" 'commands << IrcBackend::slashCommands();'
need "$GUI" 'if (irc->handleSlashCommand(roomContext, text)) return;'
need "$GUI" 'm_secureRooms.encrypt(state->profileId, window->target(), text'
need "$CLI" 'if (irc->handleSlashCommand(roomContext, line)) return;'
need "$CLI" 'Unknown slash-prefixed input remains ordinary IRC conversation'
need "$CLI" 'm_secureRooms.encrypt(entry->id, buffer->target, line'
need "$CLI" 'sendPrivateText(entry, buffer->target, line, buffer);'

# The shared IRC parser must explicitly return false for an unrecognized slash
# command so both frontends can fall through to ordinary conversation handling.
python3 - "$IRC" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
a = s.index('bool IrcBackend::handleSlashCommand')
b = s.index('void IrcBackend::sendRoomMessage', a)
body = s[a:b]
assert 'return false;' in body
# Pseudo-message commands are intentionally kept out of the backend parser so
# GUI/CLI secure-message handling cannot be bypassed by /msg or /say.
assert 'command == QStringLiteral("msg")' not in body
assert 'command == QStringLiteral("say")' not in body
PY

echo 'WaffleHouse 3.1 GUI/CLI IRC slash-command + secure-fallthrough regression: PASS'
