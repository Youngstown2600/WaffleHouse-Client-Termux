#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
pass=0; fail=0
check(){ if eval "$2"; then printf 'PASS: %s\n' "$1"; pass=$((pass+1)); else printf 'FAIL: %s\n' "$1"; fail=$((fail+1)); fi; }

# Release identity / native Termux shell.
check "WaffleHouse-Termux 1.0r1 branding" "grep -q '#define APP_DISPLAY_NAME \"WaffleHouse-Termux\"' '$ROOT/src/appbranding.h' && grep -q '#define APP_VERSION_STRING \"1.0r1\"' '$ROOT/src/appbranding.h'"
check "Dedicated Termux entry point" "test -f '$ROOT/src/main_termux.cpp' && test ! -f '$ROOT/src/main.cpp'"
check "Canonical Termux executable" "grep -q 'APP_EXECUTABLE \"wafflehouse-termux\"' '$ROOT/src/appbranding.h' && grep -q 'APP_EXECUTABLE \"wafflehouse-termux\"' '$ROOT/CMakeLists.txt'"
check "5.1 core identified in CLI" "grep -q 'WaffleHouse-Client 5.1 core' '$ROOT/src/main_termux.cpp' && grep -q 'Core-5.1' '$ROOT/include/trunkmonkey/Version.h'"
check "Qt Core/Network only" "grep -q 'COMPONENTS Core Network)' '$ROOT/CMakeLists.txt'"
check "No Qt Multimedia dependency" "! grep -q 'Qt6::Multimedia' '$ROOT/CMakeLists.txt' && ! grep -q 'qt6-qtmultimedia' '$ROOT/build-termux.sh'"
check "Native PortAudio OSCAR voice" "grep -q 'portaudio-2.0' '$ROOT/CMakeLists.txt' && grep -q 'PkgConfig::PORTAUDIO' '$ROOT/CMakeLists.txt' && grep -q 'Pa_OpenStream' '$ROOT/src/oscarvoice.cpp' && grep -q 'PortAudio voice ready' '$ROOT/src/oscarvoice.cpp'"
check "No Qt Widgets link" "! grep -q 'Qt6::Widgets' '$ROOT/CMakeLists.txt'"
check "No Qt Gui link" "! grep -q 'Qt6::Gui' '$ROOT/CMakeLists.txt'"
check "No QApplication" "! grep -Rqs 'QApplication' '$ROOT/src'"
check "No desktop GUI source files" "test ! -e '$ROOT/src/mainwindow.cpp' && test ! -e '$ROOT/src/softphonewindow.cpp' && test ! -e '$ROOT/src/mediawindow.cpp'"

# 5.1 engine parity.
check "All five protocol/media features enabled" "grep -q 'WAFFLEHOUSE_FEATURE_OSCAR=1' '$ROOT/CMakeLists.txt' && grep -q 'WAFFLEHOUSE_FEATURE_IRC=1' '$ROOT/CMakeLists.txt' && grep -q 'WAFFLEHOUSE_FEATURE_TELNET=1' '$ROOT/CMakeLists.txt' && grep -q 'WAFFLEHOUSE_FEATURE_SIP=1' '$ROOT/CMakeLists.txt' && grep -q 'WAFFLEHOUSE_FEATURE_MEDIA=1' '$ROOT/CMakeLists.txt'"
check "Unified contact/history/capability core" "test -f '$ROOT/src/core/contactstore.cpp' && test -f '$ROOT/src/core/historystore.cpp' && test -f '$ROOT/src/core/capabilityregistry.cpp' && grep -q 'src/core/contactstore.cpp' '$ROOT/CMakeLists.txt'"
check "NINA network profiles" "grep -q 'QString networkProfile = QStringLiteral(\"auto\")' '$ROOT/src/backend.h' && grep -q 'endsWith(QStringLiteral(\".nina.chat\"))' '$ROOT/src/oscarbackend.cpp' && grep -q 'AIM network (auto/nina/custom)' '$ROOT/src/terminalui.cpp'"
check "NINA BOSS multi-connection bootstrap" "grep -q 'TLV_MULTI_CONN = 0x004A' '$ROOT/src/oscarprotocol.h' && grep -q 'signonTlvs.push_back(Tlv{TLV_MULTI_CONN' '$ROOT/src/oscarbackend.cpp'"
check "5.1 tolerant OSCAR family negotiation" "grep -q 'clientImplementsFamily' '$ROOT/src/oscarbackend.cpp' && grep -q 'accepting HOST_VERSIONS with non-echoed request-id' '$ROOT/src/oscarbackend.cpp'"
check "Native OSCAR idle/presence" "grep -q '\[oscar-idle\]' '$ROOT/src/oscarbackend.cpp' && grep -q 'setIdleSeconds' '$ROOT/src/terminalui.cpp' && grep -q 'oscarBuddyPresence' '$ROOT/src/terminalui.cpp'"
check "OSCAR diagnostic modes" "grep -q 'oscarDebugMode' '$ROOT/src/backend.h' && grep -q 'off/login/full' '$ROOT/src/terminalui.cpp'"
check "OSCAR voice engine/commands" "test -f '$ROOT/src/oscarvoice.cpp' && grep -q 'src/oscarvoice.cpp' '$ROOT/CMakeLists.txt' && grep -q '/voicecall' '$ROOT/src/terminalui.cpp' && grep -q '/voiceaccept' '$ROOT/src/terminalui.cpp'"
check "IRC retained" "test -f '$ROOT/src/ircbackend.cpp' && grep -q '/nick' '$ROOT/src/terminalui.cpp'"
check "Secure rooms/transfers retained" "test -f '$ROOT/src/secureroom.cpp' && test -f '$ROOT/src/filetransfer.cpp' && test -f '$ROOT/src/directtransfer.cpp' && grep -q '/sendfile' '$ROOT/src/terminalui.cpp'"
check "Media/streams retained" "test -f '$ROOT/src/mediacontroller.cpp' && grep -q '/mstream' '$ROOT/src/terminalui.cpp' && grep -q '/mshoutcast' '$ROOT/src/terminalui.cpp'"

# 5.1 SIP behavior plus native-Termux runtime hardening retained from 0.9.
check "PJSIP 2.17 managed build" "grep -q 'branch 2.17' '$ROOT/build-termux.sh' && grep -q 'VERSION_EQUAL \"2.17\"' '$ROOT/CMakeLists.txt'"
check "PJSIP WaffleHouse account/call limits" "grep -q '#define PJSUA_MAX_ACC 32' '$ROOT/build-termux.sh' && grep -q '#define PJSUA_MAX_CALLS 64' '$ROOT/build-termux.sh' && grep -q '#define PJ_IOQUEUE_MAX_HANDLES 256' '$ROOT/build-termux.sh'"
check "Multi-SIP capacity gate" "grep -q 'PJSUA_MAX_ACC >= 32' '$ROOT/src/sipcore/SipEngine.cpp'"
check "Asterisk chan_sip compatibility" "grep -q 'asterisk-chan_sip' '$ROOT/src/sipcore/Profile.cpp' && grep -q 'SipCompatibility::AsteriskChanSip' '$ROOT/src/sipcore/SipEngine.cpp' && grep -q 'sipOutboundUse=false' '$ROOT/src/sipcore/SipEngine.cpp' && grep -q 'sipCompatibilityFromString' '$ROOT/src/sipbackend.cpp'"
check "SIP server type exposed in CLI" "grep -q 'SIP server type (auto/pjsip/chan_sip)' '$ROOT/src/terminalui.cpp'"
check "SIP blind/attended transfer controls" "grep -q '/transfer CALL-ID DESTINATION' '$ROOT/src/terminalui.cpp' && grep -q '/atransfer' '$ROOT/src/terminalui.cpp'"
check "5.1 remote SIP audio bridge" "test -f '$ROOT/src/sipcore/RemoteSipAudio.cpp' && grep -q 'src/sipcore/RemoteSipAudio.cpp' '$ROOT/CMakeLists.txt'"
check "Termux native GUID backend" "grep -q 'guid_android.o' '$ROOT/build-termux.sh' && grep -q 'guid_simple.o' '$ROOT/build-termux.sh' && grep -q 'verifyPjGuidBackend' '$ROOT/src/sipcore/SipEngine.cpp'"
check "Termux deferred-hardware RTP mode" "grep -q 'Termux deferred-hardware mode armed' '$ROOT/src/sipcore/SipEngine.cpp' && grep -q 'audio.setNullDev()' '$ROOT/src/sipcore/SipEngine.cpp' && grep -q 'ec.medConfig.hasIoqueue=true' '$ROOT/src/sipcore/SipEngine.cpp'"
check "Termux audio fallback keeps call alive" "grep -q 'trying speaker-only' '$ROOT/src/sipcore/CallSession.cpp' && grep -q 'PJSUA_SND_DEV_SPEAKER_ONLY' '$ROOT/src/sipcore/CallSession.cpp' && grep -q 'call remains active' '$ROOT/src/sipcore/CallSession.cpp'"
check "Termux runtime paths avoid hard /tmp" "grep -q 'absoluteEnvPath(\"TMPDIR\")' '$ROOT/src/sipcore/RuntimePaths.cpp' && grep -q 'absoluteEnvPath(\"PREFIX\")' '$ROOT/src/sipcore/RuntimePaths.cpp'"
check "SIP saved passwords survive network timeouts" "grep -q 'explicitAuthenticationFailure && !entry->settings.savePassword' '$ROOT/src/terminalui.cpp' && grep -q 'A transport/network failure is not evidence that the stored secret is' '$ROOT/src/terminalui.cpp'"
check "SIP startup failures visible in CLI" "grep -q 'initializationError() const' '$ROOT/src/sipcontroller.h' && grep -q '\[error\] SIP engine startup failed:' '$ROOT/src/terminalui.cpp'"
check "Timed-out SIP disconnect is idempotent" "grep -q 'e.status == PJ_EINVALIDOP' '$ROOT/src/sipcontroller.cpp' && grep -q 'SIP account already offline' '$ROOT/src/sipcontroller.cpp'"

# 5.1 exact BBS geometry adapted to a real Termux viewport.
check "Exact BBS profile geometry retained" "grep -q 'telnetColumns' '$ROOT/src/backend.h' && grep -q 'telnetRows' '$ROOT/src/backend.h' && grep -q 'telnetAutoFit' '$ROOT/src/backend.h'"
check "BBS exact outer resize request" "grep -Fq 'fprintf(stdout, \"\\033[8;%d;%dt\"' '$ROOT/src/terminalui.cpp'"
check "Live Android/Termux viewport detection" "grep -q 'TIOCGWINSZ' '$ROOT/src/terminalui.cpp' && grep -q 'resizeterm(rows, cols)' '$ROOT/src/terminalui.cpp' && grep -q 'm_nextGeometryCheckMs' '$ROOT/src/terminalui.h'"
check "BBS NAWS follows visible viewport fallback" "grep -q 'visibleCols' '$ROOT/src/terminalui.cpp' && grep -q 'visibleRows' '$ROOT/src/terminalui.cpp' && grep -q 'entry->backend->setTerminalSize(cols, rows)' '$ROOT/src/terminalui.cpp'"
check "BBS renderer no longer hard-capped at 80" "! grep -q 'std::min(80' '$ROOT/src/terminalui.cpp' && grep -q 'std::min(buffer->terminal->columns(), std::max(1, width - 2))' '$ROOT/src/terminalui.cpp'"
check "Live Telnet NAWS support" "grep -q 'CommandType::WindowSize' '$ROOT/src/telnetbackend.cpp' && grep -q 'm_nawsEnabled' '$ROOT/src/telnetbackend.cpp'"
check "Responsive phone shell" "grep -q 'responsiveLayout' '$ROOT/src/terminalui.cpp' && grep -q 'minimum 24x6' '$ROOT/src/terminalui.cpp' && grep -q 'adaptiveBoxWidth' '$ROOT/src/terminalui.cpp' && grep -q 'adaptiveBoxHeight' '$ROOT/src/terminalui.cpp'"

# Android integrations / installer.
check "Termux shared Downloads support" "grep -q 'storage/downloads' '$ROOT/src/terminalui.cpp'"
check "Android notification audio" "grep -q 'termux-media-player' '$ROOT/src/notificationmanager.cpp'"
check "Android browser handoff" "grep -q 'termux-open-url' '$ROOT/src/terminalui.cpp'"
check "Audio preflight installed" "test -x '$ROOT/scripts/termux-audio-preflight.sh' && grep -q 'wafflehouse-audio-preflight' '$ROOT/CMakeLists.txt'"
check "Builder supports upgrade" "grep -q -- '--upgrade' '$ROOT/build-termux.sh' && grep -q -- '--upgrade' '$ROOT/client-up.sh'"
check "Compatibility launcher retained" "grep -q 'ln -sf.*wafflehouse-termux.*wafflehouse-client' '$ROOT/build-termux.sh'"
check "No desktop package manager leakage" "! grep -q 'apt-get' '$ROOT/build-termux.sh' && grep -q 'pkg install' '$ROOT/build-termux.sh'"

printf '\nWaffleHouse-Termux 1.0r1 / Core-5.1 parity gate: %d passed, %d failed\n' "$pass" "$fail"
(( fail == 0 ))
