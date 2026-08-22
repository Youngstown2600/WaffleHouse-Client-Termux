#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build-termux.sh"
C="$ROOT/src/sipcontroller.cpp"
H="$ROOT/src/sipcontroller.h"
T="$ROOT/src/terminalui.cpp"
R="$ROOT/src/sipcore/RuntimePaths.cpp"

grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO[[:space:]]+1' "$B"
grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI[[:space:]]+0' "$B"
grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_OPENSL[[:space:]]+0' "$B"
grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_OBOE[[:space:]]+0' "$B"
grep -q 'PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI.*0' "$B"
grep -q 'initializationError() const' "$H"
grep -q 'm_initializationError' "$C"
grep -q 'Softphone initialization failed:' "$C"
grep -q '\[error\] SIP engine startup failed:' "$T"
grep -q 'absoluteEnvPath("TMPDIR")' "$R"
grep -q 'absoluteEnvPath("PREFIX")' "$R"
! grep -q 'return std::filesystem::path("/tmp") / ("wafflehouse-client-' "$R"
grep -q 'TMPBASE="${TMPDIR:-$PREFIX_EXPECTED/tmp}"' "$B"
! grep -Eq '>/tmp/wh-xdg-status|/tmp/wh-xdg-status' "$B"
echo 'Termux native SIP runtime/JNI isolation regression: PASS'
