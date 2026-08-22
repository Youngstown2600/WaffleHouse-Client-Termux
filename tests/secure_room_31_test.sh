#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
fail(){ echo "WaffleHouse 3.1 secure-room regression failed: $*" >&2; exit 1; }
need(){ grep -F "$2" "$1" >/dev/null || fail "$1 missing: $2"; }
forbid(){ if grep -F "$2" "$1" >/dev/null; then fail "$1 contains forbidden text: $2"; fi; }

need CMakeLists.txt 'src/secureroom.cpp'
need src/securechannel.cpp 'QStringLiteral("secure-room-v1")'
need src/secureroom.h 'class SecureRoomManager'
need src/secureroom.cpp 'crypto_aead_xchacha20poly1305_ietf_encrypt'
need src/secureroom.cpp 'crypto_aead_xchacha20poly1305_ietf_decrypt'
need src/secureroom.cpp '[[CPXROOM1:'
need src/secureroom.cpp '[[CPXROOMKEY1:'
need src/secureroom.cpp 'connectionId is deliberately NOT part of the authenticated wire data'
forbid src/secureroom.cpp 'QByteArray("CPXROOM1|") + connectionId.toUtf8()'
need src/chatwindow.cpp 'Start &Secure Room'
need src/mainwindow.cpp '[secure-room]'
need src/mainwindow.cpp '[plaintext]'
need src/mainwindow.cpp 'distributeSecureRoomKeyToMembers'
need src/mainwindow.cpp 'Membership changed; rotated shared key'
need src/terminalui.cpp '🔒 ROOM'
need src/terminalui.cpp 'startSecureRoom(entry, buffer)'
need src/terminalui.cpp '[secure-room]'
need src/terminalui.cpp '[plaintext]'
need src/chatwindow.cpp 'QStringLiteral("Secure Room")'
need src/mainwindow.cpp 'QStringLiteral("  Communications")'
need src/mainwindow.cpp 'auto *navSoftphone = makeNav(QStringLiteral("  Softphone"));'
need src/mainwindow.cpp 'connect(navSoftphone, &QPushButton::clicked'
need src/mainwindow.cpp 'including SIP/VoIP. Only the old embedded quick-dial panel was removed.'
forbid src/mainwindow.cpp 'SoftphoneCard'
need src/mainwindow.cpp 'Open &Softphone…'
need src/mainwindow.cpp 'Tools > Open Softphone'

echo 'WaffleHouse 3.1 secure AIM/IRC rooms + main-GUI softphone removal regression: PASS'
