#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CPP="$ROOT/src/terminalui.cpp"
HDR="$ROOT/src/terminalui.h"
TEL="$ROOT/src/telnetbackend.cpp"

grep -q 'QStringLiteral("/active")' "$CPP"
grep -q 'QStringLiteral("/accounts")' "$CPP"
grep -q 'command == QStringLiteral("active")' "$CPP"
grep -q 'command == QStringLiteral("accounts")' "$CPP"
grep -q 'void TerminalUi::listActiveConnections()' "$CPP"
grep -q 'bool persistent = true;' "$HDR"
grep -q 'bool sensitiveInput = false;' "$HDR"
grep -q '!entry->persistent' "$CPP"
grep -q 'settings.setArrayIndex(writeIndex++)' "$CPP"
grep -q 'm_hiddenConnectionBuffers.insert(entry->id);' "$CPP"
grep -q 'entry->settings.protocol != ConnectionSettings::Protocol::Telnet' "$CPP"
grep -q 'buffer->sensitiveInput = sensitivePrompt.match(promptLine).hasMatch();' "$CPP"
grep -q 'displayInput = QString(m_input.size(), QLatin1Char' "$CPP"
grep -q 'Raw BBS input is mirrored by the UI' "$TEL"
# Ensure the old raw-byte echo injection is gone from the RawBytes block.
! awk '/case CommandType::RawBytes:/{f=1} f{print} /break;/{if(f){exit}}' "$TEL" | grep -q 'eventReceived'
# Regression: only one push for addConnectionEntry.
awk '/TerminalUi::ConnectionEntry \*TerminalUi::addConnectionEntry/{f=1} f{print} /return entry;/{if(f){exit}}' "$CPP" | grep -c 'm_connections.push_back(entry);' | grep -q '^1$'
echo "2.5.4 active/BBS lifecycle source regression: PASS"
