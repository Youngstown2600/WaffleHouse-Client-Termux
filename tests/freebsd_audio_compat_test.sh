#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DETECTOR="$ROOT_DIR/scripts/freebsd-hda-output-detect.awk"
[ -r "$DETECTOR" ] || { echo "missing detector: $DETECTOR" >&2; exit 1; }

TMPDIR_TEST=$(mktemp -d "${TMPDIR:-/tmp}/sipher-freebsd-audio-test.XXXXXX")
trap 'rm -rf "$TMPDIR_TEST"' EXIT INT TERM

cat > "$TMPDIR_TEST/project2501-broken.txt" <<'EOF'
dev.hdaa.0.nid33_original: 0x04211010 as=1 seq=0 device=Headphones conn=Jack ctype=1/8 loc=Right color=Black misc=0
dev.hdaa.0.nid33_config: 0x04211010 as=1 seq=0 device=Headphones conn=Jack ctype=1/8 loc=Right color=Black misc=0
dev.hdaa.0.nid20_original: 0x90170120 as=2 seq=0 device=Speaker conn=Fixed ctype=Analog loc=Internal color=Unknown misc=1
dev.hdaa.0.nid20_config: 0x90170120 as=2 seq=0 device=Speaker conn=Fixed ctype=Analog loc=Internal color=Unknown misc=1
dev.hdaa.0.nid18_original: 0x90a60130 as=3 seq=0 device=Mic conn=Fixed ctype=Digital loc=Internal color=Unknown misc=1
dev.hdaa.0.nid18_config: 0x90a60130 as=3 seq=0 device=Mic conn=Fixed ctype=Digital loc=Internal color=Unknown misc=1
EOF

broken=$(awk -f "$DETECTOR" "$TMPDIR_TEST/project2501-broken.txt")
expected=$(printf '0\t20\t2\t2\t0\t33\t1\t0\t2\t0\t1\t0')
[ "$broken" = "$expected" ] || {
  echo "broken-laptop detection mismatch" >&2
  echo "expected: $expected" >&2
  echo "actual:   $broken" >&2
  exit 1
}

cat > "$TMPDIR_TEST/project2501-runtime-fixed.txt" <<'EOF'
dev.hdaa.0.nid33_original: 0x04211010 as=1 seq=0 device=Headphones conn=Jack ctype=1/8 loc=Right color=Black misc=0
dev.hdaa.0.nid33_config: 0x0421101f as=1 seq=15 device=Headphones conn=Jack ctype=1/8 loc=Right color=Black misc=0
dev.hdaa.0.nid20_original: 0x90170120 as=2 seq=0 device=Speaker conn=Fixed ctype=Analog loc=Internal color=Unknown misc=1
dev.hdaa.0.nid20_config: 0x90170110 as=1 seq=0 device=Speaker conn=Fixed ctype=Analog loc=Internal color=Unknown misc=1
EOF

fixed=$(awk -f "$DETECTOR" "$TMPDIR_TEST/project2501-runtime-fixed.txt")
# The target deliberately remains the firmware speaker association (2), so an
# ephemeral manual remap is normalized to Speaker as=2 + Headphones as=2/seq15
# before the headphone hint is persisted.
expected_fixed=$(printf '0\t20\t2\t1\t0\t33\t1\t15\t2\t0\t1\t0')
[ "$fixed" = "$expected_fixed" ] || {
  echo "runtime-fixed normalization mismatch" >&2
  echo "expected: $expected_fixed" >&2
  echo "actual:   $fixed" >&2
  exit 1
}

cat > "$TMPDIR_TEST/desktop-complex.txt" <<'EOF'
dev.hdaa.0.nid20_original: 0x01014010 as=1 seq=0 device=Line-out conn=Jack ctype=1/8 loc=Rear color=Green misc=0
dev.hdaa.0.nid20_config: 0x01014010 as=1 seq=0 device=Line-out conn=Jack ctype=1/8 loc=Rear color=Green misc=0
dev.hdaa.0.nid21_original: 0x90170120 as=2 seq=0 device=Speaker conn=Fixed ctype=Analog loc=Internal color=Unknown misc=1
dev.hdaa.0.nid21_config: 0x90170120 as=2 seq=0 device=Speaker conn=Fixed ctype=Analog loc=Internal color=Unknown misc=1
dev.hdaa.0.nid22_original: 0x02211030 as=3 seq=0 device=Headphones conn=Jack ctype=1/8 loc=Front color=Black misc=0
dev.hdaa.0.nid22_config: 0x02211030 as=3 seq=0 device=Headphones conn=Jack ctype=1/8 loc=Front color=Black misc=0
EOF

complex=$(awk -f "$DETECTOR" "$TMPDIR_TEST/desktop-complex.txt")
[ -z "$complex" ] || {
  echo "complex desktop layout must not be auto-repaired" >&2
  echo "actual: $complex" >&2
  exit 1
}

echo "freebsd audio compatibility detector tests passed"
