#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MAIN="$ROOT/src/mainwindow.cpp"
MAIN_H="$ROOT/src/mainwindow.h"
SIP="$ROOT/src/sipbackend.cpp"
SIP_CTL="$ROOT/src/sipcontroller.cpp"
BACKEND="$ROOT/src/backend.h"
TERM="$ROOT/src/terminalui.cpp"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }
has() { grep -Fq -- "$2" "$1" || fail "$3"; }
nohas() { ! grep -Fq -- "$2" "$1" || fail "$3"; }

# 2.5.1 Buddy List cleanup.
nohas "$MAIN" 'new QPushButton(QStringLiteral("Add SIP")' 'Buddy List still creates Add SIP button'
nohas "$MAIN" 'new QPushButton(QStringLiteral("Open Softphone")' 'Buddy List still creates Open Softphone button'
nohas "$MAIN_H" 'm_buddySipAddButton' 'stale Add SIP Buddy List member remains'
nohas "$MAIN_H" 'm_phoneButton' 'stale Open Softphone Buddy List member remains'
pass 'Buddy List no longer exposes Add SIP/Open Softphone buttons'

# Top-level menu model.
has "$MAIN" 'bar->addMenu(QStringLiteral("&Accounts"))' 'Accounts top-level menu missing'
has "$MAIN" 'QMenu *toolsMenu = bar->addMenu(QStringLiteral("&Tools"));' 'Tools top-level menu missing'
nohas "$MAIN" 'bar->addMenu(QStringLiteral("&Phone"))' 'Phone top-level menu still exists'
nohas "$MAIN" 'bar->addMenu(QStringLiteral("&Buddies"))' 'Buddies top-level menu still exists'
nohas "$MAIN" 'bar->addMenu(QStringLiteral("&Conversation"))' 'Conversation top-level menu still exists'
has "$MAIN" 'toolsMenu->addAction(QStringLiteral("Open &Softphone…"))' 'Softphone is not under Tools'
has "$MAIN" 'toolsMenu->addAction(QStringLiteral("Show &Connections Window"))' 'Connections Window is not under Tools'
has "$MAIN" 'toolsMenu->addAction(QStringLiteral("Change AIM &Password…"))' 'Change AIM Password is not under Tools'
has "$MAIN" 'toolsMenu->addAction(QStringLiteral("Secure Identity &Fingerprint…"))' 'Fingerprint is not under Tools'
pass 'GUI menu layout matches 2.5.1 Accounts/Tools design'

# Dynamic per-account workflow.
has "$MAIN" 'void MainWindow::rebuildAccountsMenu()' 'dynamic Accounts menu builder missing'
has "$MAIN" 'account->addAction(QStringLiteral("IM / Chatroom…"))' 'per-account IM/Chatroom action missing'
has "$MAIN" 'account->addAction(QStringLiteral("Add / Remove Buddies…"))' 'per-account AIM/IRC buddy manager missing'
has "$MAIN" 'account->addAction(QStringLiteral("Add / Remove Buddies / Contacts…"))' 'per-account SIP contact manager missing'
has "$MAIN" 'void MainWindow::openMessagingDialog' 'unified IM/Chatroom window missing'
has "$MAIN" 'tabs->addTab(imTab, QStringLiteral("Instant Message"))' 'Instant Message tab missing'
has "$MAIN" 'tabs->addTab(roomTab, QStringLiteral("Chat Room"))' 'Chat Room tab missing'
has "$MAIN" 'void MainWindow::openBuddyManager' 'per-account buddy/contact manager missing'
pass 'per-account messaging and buddy/contact dialogs are present'

# Add-SIP crash regression: SipBackend constructor must not add a PJSUA account.
CTOR=$(sed -n '/SipBackend::SipBackend/,/^}/p' "$SIP")
printf '%s\n' "$CTOR" | grep -Fq 'addAccount(' && fail 'SipBackend constructor still adds a PJSUA account re-entrantly'
has "$SIP" 'bool SipBackend::initializeAccount(QString *error)' 'deferred SIP account initializer missing'
has "$MAIN" 'm_states.insert(backend->id(), state);' 'backend state is not inserted before deferred SIP init'
has "$MAIN" 'wireBackend(backend);' 'backend is not wired before deferred SIP init'
has "$MAIN" 'backendReady = sip->initializeAccount(&sipError);' 'deferred SIP initialization is not called from attachBackend'
has "$SIP_CTL" 'catch (...)' 'unknown PJSUA2 exception guard missing'
has "$SIP_CTL" 'm_selectedAccountId = previousSelection;' 'SIP add-account rollback does not restore selection'

# Verify ordering inside attachBackend, not merely global presence.
ATTACH=$(sed -n '/void MainWindow::attachBackend/,/^}/p' "$MAIN")
INSERT_LINE=$(printf '%s\n' "$ATTACH" | grep -n -m1 'm_states.insert' | cut -d: -f1)
WIRE_LINE=$(printf '%s\n' "$ATTACH" | grep -n -m1 'wireBackend(backend)' | cut -d: -f1)
INIT_LINE=$(printf '%s\n' "$ATTACH" | grep -n -m1 'initializeAccount(&sipError)' | cut -d: -f1)
[ -n "$INSERT_LINE" ] && [ -n "$WIRE_LINE" ] && [ -n "$INIT_LINE" ] || fail 'could not determine SIP attach ordering'
[ "$INSERT_LINE" -lt "$WIRE_LINE" ] && [ "$WIRE_LINE" -lt "$INIT_LINE" ] || fail 'SIP account initializes before WaffleHouse state/wiring is complete'
pass 'Add-SIP lifecycle is deferred until GUI state is fully attached'

# SIP contacts persist in both GUI and CLI settings paths.
has "$BACKEND" 'QStringList sipContacts;' 'SIP local contact model missing'
has "$MAIN" 'sipContacts' 'GUI SIP contact persistence missing'
has "$TERM" 'sipContacts' 'CLI SIP contact persistence missing'
pass 'SIP local contacts are preserved across GUI/CLI settings'

echo 'All WaffleHouse 2.5.1 GUI workflow regression checks passed.'
