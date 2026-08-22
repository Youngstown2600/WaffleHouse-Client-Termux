#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail(){ echo "WaffleHouse Media 3.2-Termux crypto compatibility regression failed: $*" >&2; exit 1; }
need(){ grep -Fq "$2" "$ROOT/$1" || fail "$1 missing: $2"; }
hash_file(){
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
    else sha256 -q "$1"; fi
}
check_hash(){
    file=$1 expected=$2
    actual=$(hash_file "$ROOT/$file")
    [ "$actual" = "$expected" ] || fail "$file changed: expected $expected got $actual"
}

# These are exact hashes from the user-supplied WaffleHouse-Client 3.1 IRC-slash-command baseline.
# WaffleHouse Media is a media/rebrand delta and must not alter established encryption/transfer wire code.
check_hash src/securechannel.cpp c1277460b87cacf56dcbbf4512d414a6902e26335c84edc158dc2506d4f2d312
check_hash src/securechannel.h 0f17f579a713f236ab9259da15ce4dd04c43b06a00be275faf80567f4e730242
check_hash src/filetransfer.cpp b667733786025bda86d816ff9b4108c4e5fa74834092f7e11b60198406728dac
check_hash src/filetransfer.h 77256b6dce164d9ca0216ff38269caad151a457a2bb56a1493a85973e0e60cf3
check_hash src/directtransfer.cpp d3456a0ed0dbcefbd42086ebbca9cd3ae565e6f247b276f8172fbbf3797cb612
check_hash src/directtransfer.h d29287a62b5a011e0084084cd2edeae81e7b43c25a6e16f996978f5509a5135c
check_hash src/filetransport.h f4e424304a9028124fe9608ba6d4c0ee0fe754e1e8113c16722d36086c023792
check_hash src/secureroom.cpp 6fe543ea7851532d8d21941fe01bc18ab02c017add8f331425112dbf6563b693
check_hash src/secureroom.h 287d5cd53889bda3d1ac262a39b4c9b0fc8316b69149f483fad6137286a969a4

# Existing CPX3 frame grammar and capabilities stay intact. secure-room-v1 is additive.
need src/securechannel.cpp '[[CPX3:'
need src/securechannel.cpp '[[CPX3:HELLO:%1]]'
need src/securechannel.cpp '[[CPX3:CAPS:%1]]'
need src/securechannel.cpp '[[CPX3:MSG:%1:%2]]'
need src/securechannel.cpp 'QStringLiteral("secure-dm")'
need src/securechannel.cpp 'QStringLiteral("file-transfer")'
need src/securechannel.cpp 'QStringLiteral("file-resume")'
need src/securechannel.cpp 'QStringLiteral("file-ack")'
need src/securechannel.cpp 'QStringLiteral("file-direct-v1")'
need src/securechannel.cpp 'QStringLiteral("secure-room-v1")'

# Secure-room AAD intentionally omits local profile IDs for cross-client compatibility.
need src/secureroom.cpp 'return QByteArray("CPXROOM1|")'
need src/secureroom.cpp 'connectionId is deliberately NOT part of the authenticated wire data.'
python3 - "$ROOT/src/secureroom.cpp" <<'PY_AAD'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
a = s.index('QByteArray SecureRoomManager::associatedData')
b = s.index('bool SecureRoomManager::createOrRotate', a)
body = s[a:b]
assert '(void)connectionId;' in body
assert 'connectionId +' not in body
assert 'room.trimmed().toCaseFolded().toUtf8()' in body
PY_AAD

echo 'WaffleHouse-Client 3.2-Termux encrypted DM/file-transfer/Secure Room wire compatibility: PASS'
