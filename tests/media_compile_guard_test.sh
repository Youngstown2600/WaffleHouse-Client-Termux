#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail(){ echo "WaffleHouse Media 3.2-Termux compile guard failed: $*" >&2; exit 1; }
grep -Fq 'QStringLiteral("Open this URL in a browser:\n%1")' "$ROOT/src/mediawindow.cpp" || fail 'SHOUTcast fallback must use escaped newline'
python3 - "$ROOT" <<'PY2'
from pathlib import Path
import sys
root=Path(sys.argv[1])
# Catch ordinary C/C++ string literals that cross a physical newline without a trailing backslash.
# Raw strings R"(...)" are intentionally skipped.
for path in list((root/'src').glob('*.cpp')) + list((root/'src').glob('*.h')):
    text=path.read_text(errors='replace')
    i=0; line=1; state='code'; start_line=None
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1 < len(text) else ''
        if state=='code':
            if c=='/' and n=='/': state='linecomment'; i+=2; continue
            if c=='/' and n=='*': state='blockcomment'; i+=2; continue
            if c=='R' and n=='"':
                # Skip a simple C++ raw string, enough for this source tree's use.
                j=text.find('(', i+2)
                if j!=-1:
                    delim=text[i+2:j]
                    end=')'+delim+'"'
                    k=text.find(end,j+1)
                    if k!=-1:
                        line += text[i:k+len(end)].count('\n'); i=k+len(end); continue
            if c=='"': state='string'; start_line=line; i+=1; continue
            if c=="'": state='char'; i+=1; continue
        elif state=='linecomment':
            if c=='\n': state='code'
        elif state=='blockcomment':
            if c=='*' and n=='/': state='code'; i+=2; continue
        elif state=='string':
            if c=='\\': i+=2; continue
            if c=='"': state='code'; start_line=None; i+=1; continue
            if c=='\n':
                raise SystemExit(f'{path}:{start_line}: ordinary string literal crosses physical newline')
        elif state=='char':
            if c=='\\': i+=2; continue
            if c=="'": state='code'; i+=1; continue
            if c=='\n': raise SystemExit(f'{path}:{line}: character literal crosses physical newline')
        if c=='\n': line+=1
        i+=1
print('C/C++ multiline ordinary-string scan: PASS')
PY2
echo 'WaffleHouse-Client 3.2-Termux SHOUTcast compile guard: PASS'
