#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEL="$ROOT/src/telnetbackend.cpp"
UI="$ROOT/src/terminalui.cpp"

grep -q 'socket->state() != QAbstractSocket::UnconnectedState' "$TEL"
grep -q 'socket->disconnectFromHost();' "$TEL"
# waitForDisconnected must be nested behind a post-disconnect state recheck.
python3 - "$TEL" <<'PY'
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text()
needle='''socket->disconnectFromHost();\n            if (socket->state() != QAbstractSocket::UnconnectedState) {\n                socket->waitForDisconnected(500);'''
if needle not in s:
    raise SystemExit('guarded waitForDisconnected block missing')
PY
# Disconnect completion must invalidate curses' physical screen cache.
grep -q 'clearok(stdscr, TRUE);' "$UI"
grep -q 'touchwin(stdscr);' "$UI"
echo 'Telnet disconnect/curses repaint regression: PASS'
