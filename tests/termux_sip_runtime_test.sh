#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build-termux.sh"
C="$ROOT/src/sipcontroller.cpp"
H="$ROOT/src/sipcontroller.h"
T="$ROOT/src/terminalui.cpp"

grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO[[:space:]]+1' "$B"
grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI[[:space:]]+0' "$B"
grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_OPENSL[[:space:]]+0' "$B"
grep -Eq '#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_OBOE[[:space:]]+0' "$B"
grep -q 'PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI.*0' "$B"
grep -q 'initializationError() const' "$H"
grep -q 'm_initializationError' "$C"
grep -q 'Softphone initialization failed:' "$C"
grep -q '\[error\] SIP engine startup failed:' "$T"
echo 'Termux native SIP runtime/JNI isolation regression: PASS'
