#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
fail(){ echo "WaffleHouse 3.0r1 compact GUI cleanup failed: $*" >&2; exit 1; }
need(){ grep -F "$2" "$1" >/dev/null || fail "$1 missing: $2"; }
forbid(){ if grep -F "$2" "$1" >/dev/null; then fail "$1 still contains: $2"; fi; }
need src/mainwindow.cpp 'resize(860, 560);'
need src/mainwindow.cpp 'sidebar->setFixedWidth(184);'
need src/mainwindow.cpp 'm_connectionsWindow->resize(620, 430);'
need src/softphonewindow.cpp 'resize(740, 550);'
need src/softphonewindow.cpp 'sidebar->setFixedWidth(164);'
need src/chatwindow.cpp 'QStringLiteral("chat") ? 560 : 480, 360'
need src/transferwindow.cpp 'resize(600, 380);'
forbid src/mainwindow.cpp 'QStringLiteral("Quick Actions")'
forbid src/mainwindow.cpp 'm_optionsButton'
forbid src/mainwindow.cpp 'm_newImButton'
forbid src/mainwindow.cpp 'm_buddyConnectButton'
forbid src/mainwindow.cpp 'm_buddyDisconnectButton'
need src/mainwindow.cpp 'auto *navSettings = new QPushButton(QStringLiteral("Settings"), sidebar);'
need src/mainwindow.cpp 'showAccountContextMenu'
need src/mainwindow.cpp 'QListWidget::customContextMenuRequested'
need src/mainwindow.cpp 'showAccountContextMenu(state, m_connectionList->viewport()->mapToGlobal(pos));'
echo 'WaffleHouse 3.0r1 compact GUI + redundant-action cleanup: PASS'
