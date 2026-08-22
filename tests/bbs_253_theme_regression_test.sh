#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
fail(){ echo "FAIL: $*" >&2; exit 1; }
need(){ grep -F "$2" "$1" >/dev/null || fail "$1 missing: $2"; }

# BBS CLI visibility/local input fixes.
need src/terminalui.cpp 'const bool explicitBright = cell.bold && COLORS >= 16;'
need src/terminalui.cpp '+ (explicitBright ? 64 : 0)'
need src/terminalui.cpp 'm_input.insert(m_inputCursor, text);'
need src/terminalui.cpp 'm_input.clear();'
need src/terminalui.cpp 'entry->backend->setTerminalSize(80, 24);'
need src/ansiterminal.cpp 'ANSI ED clears the display but does not move the cursor.'
need src/ansiterminal.cpp "case 'd': setCursor"
need src/ansiterminal.cpp "case 'X': { // ECH"

# Full S.I.P.H.E.R. theme family should exist in both GUI and CLI sources.
for theme in solarized nord ocean retro-blue monochrome blue-box red-box beige-box 2600 wargames crt-green vt220 cobalt stealth; do
    grep -F "QStringLiteral(\"$theme\")" src/mainwindow.cpp >/dev/null || fail "GUI missing theme $theme"
    grep -F "QStringLiteral(\"$theme\")" src/terminalui.cpp >/dev/null || fail "CLI missing theme $theme"
done
need src/terminalui.cpp 'command == QStringLiteral("themes")'
need src/terminalui.cpp 'command == QStringLiteral("theme")'

echo 'PASS: WaffleHouse 2.5.3 BBS visibility/input + S.I.P.H.E.R. theme regression checks'
