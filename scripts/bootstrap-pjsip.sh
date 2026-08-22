#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
USER_HOME=${HOME:-$ROOT_DIR}
VERSION=${PJSIP_VERSION:-2.17}
DEST=${PJSIP_SOURCE_DIR:-$ROOT_DIR/third_party/pjproject}
PREFIX=${PJSIP_PREFIX:-$USER_HOME/.local/wafflehouse-pjsip}

source_version()
{
  src=$1
  vm="$src/version.mak"
  [ -f "$vm" ] || return 1

  major=$(sed -n 's/^[[:space:]]*export[[:space:]]*PJ_VERSION_MAJOR[[:space:]]*:=[[:space:]]*//p' "$vm" | head -n 1 | tr -d '[:space:]')
  minor=$(sed -n 's/^[[:space:]]*export[[:space:]]*PJ_VERSION_MINOR[[:space:]]*:=[[:space:]]*//p' "$vm" | head -n 1 | tr -d '[:space:]')
  rev=$(sed -n 's/^[[:space:]]*export[[:space:]]*PJ_VERSION_REV[[:space:]]*:=[[:space:]]*//p' "$vm" | head -n 1 | tr -d '[:space:]')

  [ -n "$major" ] && [ -n "$minor" ] || return 1
  if [ -n "$rev" ]; then
    printf '%s.%s.%s\n' "$major" "$minor" "$rev"
  else
    printf '%s.%s\n' "$major" "$minor"
  fi
}

looks_like_pjproject()
{
  src=$1
  [ -f "$src/version.mak" ] &&   [ -x "$src/configure" ] &&   [ -d "$src/pjlib" ] &&   [ -d "$src/pjsip" ] &&   [ -d "$src/pjmedia" ]
}

clone_release()
{
  target=$1
  if ! command -v git >/dev/null 2>&1; then
    echo "git is required to download PJSIP $VERSION" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$target")"
  git clone --depth 1 --branch "$VERSION" https://github.com/pjsip/pjproject.git "$target"
}

if [ -e "$DEST" ] && [ ! -d "$DEST" ]; then
  echo "PJSIP source destination exists but is not a directory: $DEST" >&2
  exit 1
fi

if [ ! -d "$DEST" ] || [ -z "$(ls -A "$DEST" 2>/dev/null || true)" ]; then
  rm -rf "$DEST"
  echo "PJSIP source is not present; downloading release $VERSION..."
  clone_release "$DEST"
elif [ -d "$DEST/.git" ]; then
  current_tag=$(git -C "$DEST" describe --tags --exact-match 2>/dev/null || true)
  if [ "$current_tag" != "$VERSION" ]; then
    echo "Existing PJSIP checkout is '$current_tag' but WaffleHouse-Client requires tag '$VERSION'." >&2
    echo "Remove $DEST or set PJSIP_SOURCE_DIR to a clean PJSIP $VERSION source tree." >&2
    exit 1
  fi
  if [ "$DEST" = "$ROOT_DIR/third_party/pjproject" ]; then
    # This directory is WaffleHouse-Client-managed. Restore exact upstream 2.17 source
    # before the build helper reapplies its small compatibility patch.
    git -C "$DEST" reset --hard "$VERSION" >/dev/null
    git -C "$DEST" clean -ffdx >/dev/null
  fi
else
  # Release archives and some package/extraction workflows intentionally do not
  # preserve .git. A valid PJSIP source tree does not need Git in order to build.
  if ! looks_like_pjproject "$DEST"; then
    echo "PJSIP source destination exists but is not a recognizable PJSIP source tree: $DEST" >&2
    exit 1
  fi
  detected=$(source_version "$DEST" || true)
  if [ "$detected" != "$VERSION" ]; then
    echo "Existing non-Git PJSIP source reports version '$detected'; WaffleHouse-Client requires '$VERSION'." >&2
    echo "Remove $DEST or set PJSIP_SOURCE_DIR to a clean PJSIP $VERSION source tree." >&2
    exit 1
  fi

  echo "Using existing non-Git PJSIP $detected source tree: $DEST"

  # A clean release tarball is safe to build directly. If this managed default
  # source tree contains generated configure state, however, it may embed the
  # absolute path of an older WaffleHouse-Client extraction. Replace it atomically with
  # a clean checkout rather than trusting stale build.mak/config.status files.
  if [ "$DEST" = "$ROOT_DIR/third_party/pjproject" ] &&      { [ -f "$DEST/build.mak" ] || [ -f "$DEST/config.status" ]; }; then
    if command -v git >/dev/null 2>&1; then
      backup="$DEST.wafflehouse-stale.$$"
      echo "Managed non-Git PJSIP tree contains generated build state; refreshing it safely..."
      rm -rf "$backup"
      mv "$DEST" "$backup"
      if clone_release "$DEST"; then
        rm -rf "$backup"
      else
        echo "Unable to download a clean PJSIP $VERSION checkout; restoring the previous source tree." >&2
        rm -rf "$DEST"
        mv "$backup" "$DEST"
        exit 1
      fi
    else
      echo "The managed non-Git PJSIP tree contains stale generated build state." >&2
      echo "Install git so WaffleHouse-Client can refresh it safely, or replace $DEST with a clean PJSIP $VERSION source tree." >&2
      exit 1
    fi
  fi
fi

sh "$ROOT_DIR/scripts/build-pjsip.sh" "$DEST" "$PREFIX"

echo
echo "PJSIP is ready for WaffleHouse-Client:"
echo "  $PREFIX"
echo "The top-level ./build.sh detects this prefix automatically."
