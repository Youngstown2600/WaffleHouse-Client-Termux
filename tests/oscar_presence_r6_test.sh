#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
proto="$ROOT/src/oscarprotocol.h"
oscar_h="$ROOT/src/oscarbackend.h"
oscar_cpp="$ROOT/src/oscarbackend.cpp"
tui="$ROOT/src/terminalui.cpp"
gui="$ROOT/src/mainwindow.cpp"

grep -q 'OS_IDLE_NOTIFICATION = 0x0011' "$proto"
grep -q 'LOCATE_SET_INFO = 0x0004' "$proto"
grep -q 'LOCATE_TLV_UNAVAILABLE_DATA = 0x0004' "$proto"
grep -q 'void setAwayMessage' "$oscar_h"
grep -q 'void setAfkMessage' "$oscar_h"
grep -q 'void setIdleSeconds' "$oscar_h"
grep -q 'void setBack' "$oscar_h"
grep -q 'sendSnac(FAM_LOCATE, LOCATE_SET_INFO' "$oscar_cpp"
grep -q 'sendSnac(FAM_OSERVICE, OS_IDLE_NOTIFICATION' "$oscar_cpp"
grep -q '\[AFK\]' "$oscar_cpp"
grep -q 'command == QStringLiteral("away")' "$tui"
grep -q 'command == QStringLiteral("afk")' "$tui"
grep -q 'command == QStringLiteral("idle")' "$tui"
grep -q 'command == QStringLiteral("back")' "$tui"
grep -q 'command == QStringLiteral("status")' "$tui"
grep -q 'Set AIM Status / AFK' "$gui"
grep -q 'OscarBackend::presenceChanged' "$gui"
grep -q 'OscarBackend::presenceChanged' "$tui"
echo 'OSCAR presence/AFK r6 source regression passed'
