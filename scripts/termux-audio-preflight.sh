#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

PREFIX=${PREFIX:-/data/data/com.termux/files/usr}
TMPDIR=${TMPDIR:-$PREFIX/tmp}
probe="$TMPDIR/wafflehouse-mic-probe-$$.m4a"
cleanup(){ rm -f "$probe" 2>/dev/null || true; }
trap cleanup EXIT

if ! command -v termux-microphone-record >/dev/null 2>&1; then
  echo "WaffleHouse audio preflight: termux-api command package is missing." >&2
  echo "Run: pkg install termux-api" >&2
  exit 2
fi

cat <<'MSG'
WaffleHouse-Termux 1.0r1 audio preflight
----------------------------------------
This test asks Android for one second of microphone access. If Android displays
an audio permission dialog, choose Allow while using the app.

IMPORTANT: The Termux:API Android add-on must be installed from the SAME source
and signing family as the main Termux app (for example F-Droid + F-Droid).
MSG

set +e
out=$(termux-microphone-record -f "$probe" -l 1 -e aac -r 16000 -c 1 2>&1)
rc=$?
set -e
printf '%s\n' "$out"
if [[ $rc -ne 0 ]]; then
  echo >&2
  echo "Microphone preflight failed before recording started." >&2
  echo "Install/enable the matching Termux:API Android add-on and grant Microphone permission." >&2
  exit $rc
fi

sleep 2
# The API service closes the file when its duration limit expires. A nonempty
# recording proves the shared Termux UID can actually capture Android audio.
if [[ -s "$probe" ]]; then
  bytes=$(wc -c < "$probe" | tr -d ' ')
  echo "Microphone preflight PASS: Android capture produced $bytes bytes."
  exit 0
fi

echo "Microphone preflight did not produce audio data." >&2
echo "Open Android Settings -> Apps -> Termux:API -> Permissions -> Microphone and allow it, then rerun:" >&2
echo "  wafflehouse-audio-preflight" >&2
exit 1
