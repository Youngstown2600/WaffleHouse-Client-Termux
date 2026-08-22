#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail(){ echo "WaffleHouse 3.0r1 account context-menu regression failed: $*" >&2; exit 1; }
need(){ grep -F "$2" "$ROOT/$1" >/dev/null || fail "$1 missing: $2"; }
need src/mainwindow.h 'void showAccountContextMenu(BackendState *state, const QPoint &globalPos);'
need src/mainwindow.cpp 'm_buddyTree->setContextMenuPolicy(Qt::CustomContextMenu);'
need src/mainwindow.cpp 'QTreeWidget::customContextMenuRequested'
need src/mainwindow.cpp 'if (!item || item->parent()) return; // account rows only'
need src/mainwindow.cpp 'm_connectionList->setContextMenuPolicy(Qt::CustomContextMenu);'
need src/mainwindow.cpp 'QListWidget::customContextMenuRequested'
need src/mainwindow.cpp 'QStringLiteral("Start IM…")'
need src/mainwindow.cpp 'QStringLiteral("Join IRC Channel…")'
need src/mainwindow.cpp 'QStringLiteral("Join AIM Chat…")'
need src/mainwindow.cpp 'QStringLiteral("Add / Remove Buddies…")'
if grep -F 'QStringLiteral("Open Softphone")' "$ROOT/src/mainwindow.cpp" >/dev/null; then fail 'Buddy List account context menu must not expose Softphone'; fi
need src/mainwindow.cpp 'Open &Softphone…'
need src/mainwindow.cpp 'QStringLiteral("Edit Connection…")'
need src/mainwindow.cpp 'menu.exec(globalPos);'
echo 'WaffleHouse 3.0r1 account context-menu regression: PASS'
