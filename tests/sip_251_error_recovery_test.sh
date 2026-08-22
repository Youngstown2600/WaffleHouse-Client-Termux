#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

fail(){ echo "FAIL: $*" >&2; exit 1; }

# Regression for 2.5.1 CLI Add-SIP stack overflow: a failed account create emitted
# backendError; the CLI error handler called setConnectionSettings(); that method
# retried initializeAccount(); failure emitted backendError again, recursively.
grep -q 'void SipBackend::clearSessionPassword()' src/sipbackend.cpp || fail 'missing non-reentrant SIP password clear'
grep -q 'if (!m_accountInitialized)' src/sipbackend.cpp || fail 'setConnectionSettings still lacks uninitialized-account guard'
block=$(sed -n '/void SipBackend::setConnectionSettings/,/void SipBackend::clearSessionPassword/p' src/sipbackend.cpp)
if printf '%s\n' "$block" | sed -n '/if (!m_accountInitialized)/,/^    }/p' | grep -q 'initializeAccount'; then
  fail 'uninitialized setConnectionSettings still retries initializeAccount'
fi
grep -q 'sip->clearSessionPassword();' src/terminalui.cpp || fail 'CLI error recovery still re-enters SIP settings update'
grep -q 'sip->clearSessionPassword();' src/mainwindow.cpp || fail 'GUI error recovery still re-enters SIP settings update'

# PJSUA2 reports API failures as pj::Error. Verify the account path surfaces its
# structured diagnostic instead of falling through to the generic catch-all.
grep -q 'catch (const pj::Error &e)' src/sipcontroller.cpp || fail 'pj::Error diagnostics missing'
grep -q 'pjsipErrorText(e)' src/sipcontroller.cpp || fail 'pj::Error text is not surfaced'

# Human-friendly SIP URI normalization for optional registrar/proxy fields.
grep -q 'ensureSipUri(v.sipRegistrar)' src/sipbackend.cpp || fail 'registrar normalization missing'
grep -q 'ensureSipUri(v.sipOutboundProxy)' src/sipbackend.cpp || fail 'outbound proxy normalization missing'

echo 'PASS: SIP 2.5.1 account-error recovery regression checks'
