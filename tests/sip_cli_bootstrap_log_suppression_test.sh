#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/sipcore/SipEngine.cpp"
fail(){ echo "FAIL: $*" >&2; exit 1; }

grep -q '#include <pj/log.h>' "$SRC" || fail 'pj/log.h not included'
grep -q 'PjBootstrapLogSilencer bootstrapLogSilencer' "$SRC" || fail 'bootstrap silencer not created before PJSIP startup'
grep -q 'pj_log_set_level(0)' "$SRC" || fail 'pre-libInit PJSIP console level is not silenced'
grep -q 'ec.logConfig.consoleLevel=0' "$SRC" || fail 'configured PJSIP console logging is not disabled'
grep -q 'ec.logConfig.filename=runtime::pjsipLogPath().string()' "$SRC" || fail 'PJSIP file log destination was lost'
grep -q 'bootstrapLogSilencer.release()' "$SRC" || fail 'bootstrap silencer is not released after libInit'

create_line=$(grep -n 'endpoint_->libCreate();' "$SRC" | head -1 | cut -d: -f1)
silence_line=$(grep -n 'PjBootstrapLogSilencer bootstrapLogSilencer' "$SRC" | head -1 | cut -d: -f1)
init_line=$(grep -n 'endpoint_->libInit(ec);' "$SRC" | head -1 | cut -d: -f1)
release_line=$(grep -n 'bootstrapLogSilencer.release()' "$SRC" | head -1 | cut -d: -f1)
[ "$silence_line" -lt "$create_line" ] || fail 'bootstrap suppression must start before libCreate'
[ "$create_line" -lt "$init_line" ] || fail 'libCreate/libInit ordering changed unexpectedly'
[ "$init_line" -lt "$release_line" ] || fail 'bootstrap suppression must remain active through libInit'

echo 'WaffleHouse CLI PJSIP bootstrap-console suppression: PASS'
