#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ENGINE="$ROOT/src/sipcore/SipEngine.cpp"
CLI="$ROOT/src/terminalui.cpp"

fail() { echo "FAIL: $*" >&2; exit 1; }

# PJSIP 2.17 pjsua_update_stun_servers() asserts if count/srv are zero/null.
# The runtime refresh must explicitly short-circuit an empty collected list.
grep -F 'if(servers.empty()) {' "$ENGINE" >/dev/null || fail 'empty STUN list is not guarded'
grep -F 'endpoint_->natUpdateStunServers(servers,false);' "$ENGINE" >/dev/null || fail 'runtime STUN update call missing'

# The guard must occur before the call within refreshStunServers().
awk '
  /void SipEngine::refreshStunServers\(\)/ {infn=1}
  infn && /if\(servers\.empty\(\)\)/ {guard=NR}
  infn && /natUpdateStunServers\(servers,false\)/ {call=NR}
  infn && /^}/ {if (guard && call) exit}
  END { if (!guard || !call || guard >= call) exit 1 }
' "$ENGINE" || fail 'STUN empty guard does not precede update call'

# CLI connection creation must initialize a SIP backend after insertion into the
# CLI state model, so restored accounts populate SipController before start().
awk '
  /m_connectionById\.insert\(entry->id, entry\)/ {insert=NR}
  /backendReady = sip->initializeAccount\(&sipError\)/ {init=NR}
  END { if (!insert || !init || insert >= init) exit 1 }
' "$CLI" || fail 'CLI SIP initialization ordering is wrong'

echo 'PASS: empty-STUN abort and CLI SIP-account restore regression checks'
