#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CPP="$ROOT/src/terminalui.cpp"

block=$(awk '/void TerminalUi::drawStatusBar\(int row, int width\)/{f=1} f{print} /^}/{if(f){exit}}' "$CPP")
printf '%s\n' "$block" | grep -F 'ConnectionEntry *entry = buffer ? connectionById(buffer->connectionId) : nullptr;' >/dev/null
! printf '%s\n' "$block" | grep -Fq 'selectedConnection()'
! printf '%s\n' "$block" | grep -Fq 'connectionText'
printf '%s\n' "$block" | grep -F 'QStringLiteral(" [%1] [%2] [%3]")' >/dev/null
printf '%s\n' "$block" | grep -F '.arg(bufferText)' >/dev/null
printf '%s\n' "$block" | grep -F '.arg(stateText)' >/dev/null

echo 'CLI r3 status-bar context regression: PASS'
