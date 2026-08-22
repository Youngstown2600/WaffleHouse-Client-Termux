#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TERM="$ROOT/src/terminalui.cpp"
HDR="$ROOT/src/terminalui.h"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The CLI should reserve the bottom three rows for the persistent shortcut
# hint, an Irssi-style status bar, and the input prompt.
grep -F 'void TerminalUi::drawShortcutHint(int row, int width)' "$TERM" >/dev/null \
    || fail 'drawShortcutHint implementation missing'
grep -F 'void drawShortcutHint(int row, int width);' "$HDR" >/dev/null \
    || fail 'drawShortcutHint declaration missing'
grep -F 'Tab completes /commands | Ctrl-N/P buffers | Alt-1..9/F1..F9 jump | PgUp/PgDn scroll |' "$TERM" >/dev/null \
    || fail 'persistent shortcut hint text missing'
grep -F 'drawShortcutHint(height - 3, width);' "$TERM" >/dev/null \
    || fail 'shortcut hint is not directly above the status bar'
grep -F 'void TerminalUi::drawStatusBar(int row, int width)' "$TERM" >/dev/null \
    || fail 'drawStatusBar implementation missing'
grep -F 'void drawStatusBar(int row, int width);' "$HDR" >/dev/null \
    || fail 'drawStatusBar declaration missing'
grep -F 'drawStatusBar(height - 2, width);' "$TERM" >/dev/null \
    || fail 'status bar is not anchored one row above input'
grep -F 'drawInputLine(height - 1, width);' "$TERM" >/dev/null \
    || fail 'input line is not on the terminal bottom row'
! grep -Fq 'drawFooter(height - 1, width);' "$TERM" \
    || fail 'old footer still occupies the bottom row'

grep -F 'QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))' "$TERM" >/dev/null \
    || fail 'status bar clock missing'
! awk '/void TerminalUi::drawStatusBar\(int row, int width\)/{f=1} f{print} /^}/{if(f){exit}}' "$TERM" | grep -Fq 'selectedConnection()' \
    || fail 'status bar still falls back to selected connection'
! awk '/void TerminalUi::drawStatusBar\(int row, int width\)/{f=1} f{print} /^}/{if(f){exit}}' "$TERM" | grep -Fq '.arg(connectionText)' \
    || fail 'status bar still renders redundant unnumbered connection label'
grep -F 'buffer->number' "$TERM" >/dev/null \
    || fail 'status bar buffer number missing'
grep -F 'A_REVERSE | A_BOLD' "$TERM" >/dev/null \
    || fail 'status bar does not use full-width highlighted styling'

echo 'PASS: CLI shortcut hint, Irssi-style status bar, and prompt are anchored at the bottom'
