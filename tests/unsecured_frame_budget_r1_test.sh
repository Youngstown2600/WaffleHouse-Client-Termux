#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail(){ echo "WaffleHouse 3.0r1 unsecured frame-budget regression failed: $*" >&2; exit 1; }

grep -F 'const int rawChunk = irc ? (secureTransfer ? 120 : 96) : 768;' "$ROOT/src/mainwindow.cpp" >/dev/null || fail 'GUI unsecured chunk budget is not 96 bytes'
grep -F 'const int chunkBytes = irc ? (secureTransfer ? 120 : 96) : 768;' "$ROOT/src/terminalui.cpp" >/dev/null || fail 'CLI unsecured chunk budget is not 96 bytes'
grep -F 'frame.toUtf8().size() > 400' "$ROOT/src/mainwindow.cpp" >/dev/null || fail 'GUI IRC safety ceiling missing'
grep -F 'frame.toUtf8().size() > 400' "$ROOT/src/terminalui.cpp" >/dev/null || fail 'CLI IRC safety ceiling missing'

python3 - <<'PY'
import base64
transfer_id='12345678-1234-1234-1234-123456789abc'
raw=b'x'*96
inner='\x1eCPXFILE1|DATA|%s|1073741824|%s' % (
    transfer_id, base64.urlsafe_b64encode(raw).decode().rstrip('='))
outer='\x1eWHFILE1|' + base64.urlsafe_b64encode(inner.encode()).decode().rstrip('=')
if len(outer.encode()) > 400:
    raise SystemExit('96-byte unsecured DATA frame exceeds 400-byte IRC budget: %d' % len(outer.encode()))
# A reasonably long 96-byte filename offer should also fit; longer metadata is
# guarded at runtime by the explicit 400-byte rejection path.
name='n'*96
name64=base64.urlsafe_b64encode(name.encode()).decode().rstrip('=')
offer='\x1eCPXFILE1|OFFER2|%s|%s|1073741824|%s' % (transfer_id,name64,'a'*64)
wire='\x1eWHFILE1|' + base64.urlsafe_b64encode(offer.encode()).decode().rstrip('=')
if len(wire.encode()) > 400:
    raise SystemExit('96-byte filename offer unexpectedly exceeds IRC budget: %d' % len(wire.encode()))
print('unsecured frame budget: DATA=%d bytes OFFER=%d bytes' % (len(outer.encode()),len(wire.encode())))
PY

echo 'WaffleHouse 3.0r1 unsecured IRC frame-budget regression: PASS'
