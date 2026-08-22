#!/usr/bin/env bash
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
need() { grep -F "$2" "$ROOT/$1" >/dev/null || { echo "FAIL: $1 missing $2" >&2; exit 1; }; }
reject() { ! grep -F "$2" "$ROOT/$1" >/dev/null || { echo "FAIL: $1 still contains $2" >&2; exit 1; }; }
need src/telnetbackend.cpp 'decodeTerminalText'
need src/telnetbackend.cpp 'cp437[256]'
need src/telnetbackend.cpp 'CommandType::RawBytes'
reject src/telnetbackend.cpp 'sanitizeTerminalText'
need src/ansiterminal.cpp 'handleCsi'
need src/ansiterminalwidget.cpp 'terminalBytes'
need src/terminalui.cpp 'command == QStringLiteral("telnet")'
need src/terminalui.cpp 'command == QStringLiteral("bbsimport")'
need src/terminalui.cpp 'protocolToken == "aim"'
need src/terminalui.cpp 'none active — /connect PROTOCOL:name or /telnet host:port'
need src/mainwindow.cpp 'Import &BBS List…'
need src/mainwindow.cpp 'attachBackend(backend, true, false, false, false)'
need src/terminalui.cpp 'settings, secretRequired, !secret.isEmpty(), true, false'
echo 'PASS: WaffleHouse 2.5.2 BBS terminal / explicit-connect source regression checks'
