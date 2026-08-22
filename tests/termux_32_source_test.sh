#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail(){ echo "WaffleHouse-Client 3.2-Termux source gate failed: $*" >&2; exit 1; }
need(){ grep -Fq -- "$2" "$ROOT/$1" || fail "$1 missing: $2"; }
forbid(){ if grep -Fqi -- "$2" "$ROOT/$1"; then fail "$1 contains forbidden text: $2"; fi; }
need_file(){ [ -s "$ROOT/$1" ] || fail "missing/empty $1"; }

# Product identity and CLI-only Termux target.
need CMakeLists.txt 'project(WaffleHouseClientTermux VERSION 3.2.0 LANGUAGES CXX)'
need CMakeLists.txt 'set(APP_VERSION_STRING "3.2-Termux"'
need CMakeLists.txt 'find_package(Qt6 REQUIRED COMPONENTS Core Network)'
forbid CMakeLists.txt 'Qt6::Widgets'
forbid CMakeLists.txt 'Qt6::Gui'
need CMakeLists.txt 'WAFFLEHOUSE_TERMUX=1'
need CMakeLists.txt 'src/main_termux.cpp'
need src/appbranding.h '#define APP_VERSION_STRING "3.2-Termux"'
need include/trunkmonkey/Version.h '#define WAFFLEHOUSE_SOFTPHONE_VERSION "3.2-Termux"'
need include/trunkmonkey/Version.h 'WaffleHouse-Client/3.2-Termux'
need src/main_termux.cpp 'QCoreApplication app(argc, argv);'
need src/main_termux.cpp 'TerminalUi ui;'
need src/main_termux.cpp 'migrateLegacyWaffleHouseProfiles();'

# Termux builder and PJSIP/audio path.
need build-termux.sh 'qt6-qtbase libsodium ncurses openssl libuuid portaudio libopus'
need build-termux.sh 'wafflehouse-xdg-utils-compat'
need build-termux.sh 'Provides: xdg-utils'
forbid build-termux.sh 'pkg install -y perl'
forbid build-termux.sh 'prepare_termux_cpan_home'
forbid build-termux.sh 'cpan.metacpan.org'
need build-termux.sh 'mpv ffmpeg termux-api'
need build-termux.sh 'PJSIP_PREFIX='
need build-termux.sh 'sh scripts/bootstrap-pjsip.sh'
need scripts/bootstrap-pjsip.sh 'sh "$ROOT_DIR/scripts/build-pjsip.sh"'
need build-termux.sh '--upgrade) CLEAN=1'
need build-termux.sh '--uninstall|--remove-only) UNINSTALL=1'
need scripts/build-pjsip.sh 'HOST_OS=Termux'
need scripts/build-pjsip.sh '--with-external-pa'
need scripts/build-pjsip.sh 'portaudio-2.0:portaudio:Android OpenSL ES audio'
need scripts/build-pjsip.sh 'opus:libopus:Opus codec'
need scripts/build-pjsip.sh "sed 's/-lstdc++/-lc++/g'"
need scripts/build-pjsip.sh 'PJSUA_MAX_ACC 32'
need scripts/build-pjsip.sh 'PJSUA_MAX_CALLS 64'

# Phone-sized ncurses adaptation.
need src/terminalui.cpp 'if (COLS < 72)'
need src/terminalui.cpp '3.2-TERMUX'
need src/terminalui.cpp 'if (height < 8 || width < 30)'
need src/terminalui.cpp 'QStringLiteral("/menu")'
need src/terminalui.cpp 'QStringLiteral("Termux Command Map")'
need src/terminalui.cpp 'fitDialogWidth('
need src/terminalui.cpp 'fitDialogHeight('

# Protocol/core feature parity.
for f in src/oscarbackend.cpp src/ircbackend.cpp src/telnetbackend.cpp src/ansiterminal.cpp \
         src/securechannel.cpp src/secureroom.cpp src/filetransfer.cpp src/directtransfer.cpp \
         src/sipcontroller.cpp src/sipbackend.cpp src/mediacontroller.cpp src/notificationmanager.cpp; do
  need_file "$f"
done
need src/terminalui.cpp 'QStringLiteral("/join")'
need src/terminalui.cpp 'QStringLiteral("/nick")'
need src/terminalui.cpp 'QStringLiteral("/telnet")'
need src/terminalui.cpp 'QStringLiteral("/secure")'
need src/terminalui.cpp 'QStringLiteral("/sendfile")'
need src/terminalui.cpp 'QStringLiteral("/phone")'
need src/terminalui.cpp 'QStringLiteral("/dial")'
need src/terminalui.cpp 'QStringLiteral("/ladder")'
need src/terminalui.cpp 'QStringLiteral("/audio-devices")'
need src/terminalui.cpp 'QStringLiteral("/media")'
need src/terminalui.cpp 'QStringLiteral("/mshoutcast")'
need src/terminalui.cpp 'QStringLiteral("/mplaylist")'
need src/terminalui.cpp 'QStringLiteral("/themes")'
need src/terminalui.cpp 'QStringLiteral("/notifications")'

# Android integration points.
need src/platforminfo.cpp 'Android / Termux'
need src/notificationmanager.cpp 'termux-media-player'
need src/terminalui.cpp 'termux-open-url'
need src/terminalui.cpp 'storage/downloads'
need README.md 'termux-setup-storage'

# 3.2-Termux baseline intentionally has YouTube-specific media removed.
for file in src/mediacontroller.cpp src/terminalui.cpp build-termux.sh; do
  forbid "$file" 'yt-dlp'
  forbid "$file" '/myoutube'
done

# Branding request: the former media-edition sub-brand must be absent.
if grep -RqiE 'Waffle[Rr]adio' "$ROOT" \
    --exclude-dir=build-termux --exclude='*.zip' --exclude='termux_32_source_test.sh' 2>/dev/null; then
  fail 'former media-edition branding remains'
fi

echo 'WaffleHouse-Client 3.2-Termux source/feature parity gate: PASS'
