#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/src/terminalui.cpp"
S="$ROOT/src/sipcontroller.cpp"

# 408/network errors must not be classified as authentication failures.
grep -q 'explicitAuthenticationFailure' "$T"
grep -q 'explicitAuthenticationFailure && !entry->settings.savePassword' "$T"
grep -q 'A transport/network failure is not evidence that the stored secret is' "$T"
# Persisted credentials must not go through clearSessionPassword().
python - "$T" <<'PY'
import sys
text=open(sys.argv[1]).read()
start=text.index('void TerminalUi::onBackendError')
end=text.index('\nQString TerminalUi::takeArgument', start)
block=text[start:end]
assert 'explicitAuthenticationFailure && !entry->settings.savePassword' in block
assert 'sip->clearSessionPassword();' in block
# The clear must occur inside the guarded block, not an unconditional SIP path.
assert block.index('explicitAuthenticationFailure && !entry->settings.savePassword') < block.index('sip->clearSessionPassword();')
assert '408' not in block[block.index('const bool explicitAuthenticationFailure ='):block.index('if (entry->connecting')]
PY
# Timed-out accounts should be harmless to disconnect.
grep -q 'e.status == PJ_EINVALIDOP' "$S"
grep -q 'SIP account already offline' "$S"

echo 'Termux SIP saved-password + timeout disconnect regression: PASS'
