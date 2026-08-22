#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CPP="$ROOT/src/terminalui.cpp"

# Restored QSettings profiles intentionally avoid saveConnections() while the
# array is being read, but their stable profileId must mark them persistent.
grep -q 'const bool restoredPersistentProfile = !profileId.trimmed().isEmpty();' "$CPP"
grep -q 'entry->persistent = persist || restoredPersistentProfile;' "$CPP"

# Loading still calls addConnectionEntry without a mid-read save.
grep -q 'addConnectionEntry(value, required, !value.password.isEmpty(), false, false, profileId);' "$CPP"

# Saved directory commands must filter to persistent profiles; /active is separate.
awk '/void TerminalUi::listConnections\(\)/{f=1} f{print} /switchToBuffer\(buffer\);/{if(f){exit}}' "$CPP" | grep -q '!entry->persistent'
grep -q 'void TerminalUi::listActiveConnections()' "$CPP"

# saveConnections must serialize persistent profiles with a compact write index.
awk '/void TerminalUi::saveConnections\(\) const/{f=1} f{print} /^}/{if(f){exit}}' "$CPP" | grep -q 'if (!entry || !entry->persistent)'
awk '/void TerminalUi::saveConnections\(\) const/{f=1} f{print} /^}/{if(f){exit}}' "$CPP" | grep -q 'settings.setArrayIndex(writeIndex++)'

echo "saved-profile persistence source regression: PASS"
