#!/data/data/com.termux/files/usr/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT_DIR"

APP_VERSION=3.2-Termux
BUILD_DIR="$ROOT_DIR/build-termux"
TERMUX_PREFIX=${PREFIX:-/data/data/com.termux/files/usr}
PJSIP_PREFIX=${PJSIP_PREFIX:-$HOME/.local/wafflehouse-pjsip-termux}
JOBS=
CLEAN=0
INSTALL=1
AUTO_DEPS=1
REBUILD_PJSIP=0
DRY_RUN=0
UNINSTALL=0

usage() {
  cat <<USAGE
WaffleHouse-Client 3.2-Termux builder

Usage: ./build.sh [options]
  --clean          remove the Termux build directory first
  --upgrade        clean rebuild and reinstall (compatibility alias)
  --uninstall      remove the installed Termux binary and sound assets
  --remove-only    alias for --uninstall
  --pjsip          force rebuild of managed PJSIP 2.17
  --no-auto-deps   do not install missing Termux packages
  --no-install     build but do not install to \$PREFIX/bin
  --dry-run        print actions without changing the device
  --jobs N         parallel build jobs
  -h, --help       show this help

The Termux build is CLI-only but retains the full source CLI feature set: AIM/OSCAR,
IRC, Telnet/BBS, encrypted DM/Secure Rooms, file transfer, multi-account SIP,
SIP call controls/log/ladder/audio controls, themes, notifications, and media.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --clean) CLEAN=1 ;;
    --upgrade) CLEAN=1 ;;
    --uninstall|--remove-only) UNINSTALL=1 ;;
    --pjsip) REBUILD_PJSIP=1 ;;
    --no-auto-deps) AUTO_DEPS=0 ;;
    --no-install) INSTALL=0 ;;
    --dry-run) DRY_RUN=1 ;;
    --jobs)
      shift
      [ "$#" -gt 0 ] || { echo "--jobs requires a value" >&2; exit 2; }
      JOBS=$1
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

if [ -z "${TERMUX_VERSION:-}" ] && [ "$TERMUX_PREFIX" != "/data/data/com.termux/files/usr" ]; then
  echo "This builder must be run inside Termux on Android." >&2
  echo "Detected PREFIX: $TERMUX_PREFIX" >&2
  exit 2
fi

if [ -z "$JOBS" ]; then
  JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
fi
case "$JOBS" in ''|*[!0-9]*|0) JOBS=2 ;; esac

run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '  [dry-run]'
    printf ' %s' "$@"
    printf '\n'
  else
    "$@"
  fi
}

need_cmd() { command -v "$1" >/dev/null 2>&1; }

if [ "$UNINSTALL" -eq 1 ]; then
  echo "==> Removing WaffleHouse-Client $APP_VERSION from Termux"
  run rm -f "$TERMUX_PREFIX/bin/wafflehouse-client"
  run rm -rf "$TERMUX_PREFIX/share/wafflehouse-client"
  echo "WaffleHouse-Client Termux install removed. User settings were preserved."
  exit 0
fi

install_xdg_utils_compat() {
  # qt6-qtbase depends on xdg-utils, but the Termux xdg-utils package currently
  # runs a CPAN install hook.  The WaffleHouse Termux CLI does not need the
  # desktop xdg-utils implementation, so satisfy that dependency with a tiny
  # local provider instead.  Its xdg-open wrapper delegates to native Termux
  # helpers when another program happens to call it.
  STUB_NAME=wafflehouse-xdg-utils-compat
  STUB_VERSION=1.0.0
  STUB_ROOT="$ROOT_DIR/.termux-xdg-utils-compat"
  STUB_DEB="$ROOT_DIR/.termux-xdg-utils-compat.deb"
  REL_PREFIX=${TERMUX_PREFIX#/}

  if [ "$DRY_RUN" -eq 1 ]; then
    echo "  [dry-run] install local $STUB_NAME provider for xdg-utils (no Perl/CPAN)"
    return 0
  fi

  need_cmd dpkg || { echo "Termux dpkg command not found." >&2; exit 1; }
  need_cmd dpkg-deb || { echo "Termux dpkg-deb command not found." >&2; exit 1; }

  if dpkg-query -W -f='${Status}' xdg-utils 2>/dev/null | grep -q '^install ok installed$'; then
    echo "==> Existing xdg-utils is already configured; leaving it untouched"
    return 0
  fi

  if dpkg-query -W -f='${Status}' "$STUB_NAME" 2>/dev/null | grep -q '^install ok installed$'; then
    echo "==> WaffleHouse xdg-utils compatibility provider is already installed"
    return 0
  fi

  # A previous qt6-qtbase attempt may have left the official xdg-utils package
  # unpacked but unconfigured. Remove that broken package *before* running
  # dpkg --configure -a so its CPAN postinst is never retried.
  if dpkg-query -W -f='${Status}' xdg-utils >/dev/null 2>&1; then
    echo "==> Removing incomplete Termux xdg-utils package"
    dpkg --remove --force-remove-reinstreq xdg-utils >/dev/null 2>&1 || \
      dpkg --purge --force-all xdg-utils >/dev/null 2>&1 || true
  fi

  rm -rf "$STUB_ROOT" "$STUB_DEB"
  mkdir -p "$STUB_ROOT/DEBIAN" "$STUB_ROOT/$REL_PREFIX/bin"
  cat > "$STUB_ROOT/DEBIAN/control" <<EOF
Package: $STUB_NAME
Version: $STUB_VERSION
Architecture: all
Maintainer: WaffleHouse-Client
Section: misc
Priority: optional
Provides: xdg-utils
Conflicts: xdg-utils
Replaces: xdg-utils
Description: Minimal xdg-utils compatibility provider for WaffleHouse-Client Termux
 Satisfies Qt's xdg-utils dependency without the desktop package's Perl/CPAN
 install hook.  xdg-open delegates to Termux/Android helpers.
EOF

  cat > "$STUB_ROOT/$REL_PREFIX/bin/xdg-open" <<'EOF'
#!/data/data/com.termux/files/usr/bin/sh
set -eu
[ "$#" -gt 0 ] || { echo "usage: xdg-open URL|FILE" >&2; exit 2; }
target=$1
case "$target" in
  http://*|https://*)
    if command -v termux-open-url >/dev/null 2>&1; then
      exec termux-open-url "$target"
    fi
    ;;
esac
if command -v termux-open >/dev/null 2>&1; then
  exec termux-open "$target"
fi
if command -v am >/dev/null 2>&1; then
  exec am start -a android.intent.action.VIEW -d "$target"
fi
echo "No Android opener is available for: $target" >&2
exit 1
EOF
  chmod 0755 "$STUB_ROOT/$REL_PREFIX/bin/xdg-open"

  dpkg-deb --build "$STUB_ROOT" "$STUB_DEB" >/dev/null
  dpkg -i "$STUB_DEB" >/dev/null
  rm -rf "$STUB_ROOT" "$STUB_DEB"
}

recover_termux_dpkg() {
  [ "$DRY_RUN" -eq 0 ] || return 0
  need_cmd dpkg || return 0
  if dpkg --audit 2>/dev/null | grep -q .; then
    echo "==> Recovering interrupted Termux package configuration without CPAN"
    install_xdg_utils_compat
    dpkg --configure -a
  fi
}

install_packages() {
  [ "$AUTO_DEPS" -eq 1 ] || return 0
  if [ "$DRY_RUN" -eq 0 ]; then
    need_cmd pkg || { echo "Termux 'pkg' command not found." >&2; exit 1; }
  fi

  # Repair a previous interrupted Qt/xdg-utils transaction first, but never
  # invoke the official xdg-utils CPAN post-install path.
  install_xdg_utils_compat
  recover_termux_dpkg

  echo "==> Refreshing Termux package metadata"
  run pkg update -y

  # Qt 6 is currently supplied from the official Termux X11 repository.  The
  # executable does not launch Qt Widgets/X11; only Qt Core/Network are
  # linked for the existing WaffleHouse shared protocol/TUI code.
  echo "==> Enabling official Termux X11 repository for Qt 6 libraries"
  run pkg install -y x11-repo
  run pkg update -y

  # Reassert the local xdg-utils provider after repository setup so apt sees
  # the Qt dependency as already satisfied without pulling Perl/CPAN.
  install_xdg_utils_compat

  # Keep Termux package names separate from pkg-config module names.  In
  # particular, the Opus package is named libopus while it exports opus.pc.
  TERMUX_DEPS="clang cmake make pkg-config git curl ca-certificates qt6-qtbase libsodium ncurses openssl libuuid portaudio libopus mpv ffmpeg termux-api"

  echo "==> Verifying Termux dependency package names"
  if [ "$DRY_RUN" -eq 0 ]; then
    for package in $TERMUX_DEPS; do
      if ! pkg show "$package" >/dev/null 2>&1; then
        echo "Required Termux package is unavailable from enabled repositories: $package" >&2
        echo "Try: pkg update && pkg upgrade" >&2
        exit 1
      fi
    done
  else
    echo "  [dry-run] pkg show: $TERMUX_DEPS"
  fi

  echo "==> Installing WaffleHouse build/runtime dependencies"
  # shellcheck disable=SC2086 -- intentional word splitting of package list.
  run pkg install -y $TERMUX_DEPS
}

install_packages

if [ "$DRY_RUN" -eq 0 ]; then
  missing=""
  for cmd in clang clang++ cmake make pkg-config git; do
    if ! need_cmd "$cmd"; then missing="$missing $cmd"; fi
  done
  if [ -n "$missing" ]; then
    echo "Missing required commands:$missing" >&2
    echo "Run ./build.sh with automatic dependencies enabled." >&2
    exit 1
  fi
fi

export PKG_CONFIG_PATH="$PJSIP_PREFIX/lib/pkgconfig:$PJSIP_PREFIX/libdata/pkgconfig:$TERMUX_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$TERMUX_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

if [ "$DRY_RUN" -eq 0 ]; then
  for module in libsodium ncursesw portaudio-2.0 opus openssl uuid; do
    if ! pkg-config --exists "$module" 2>/dev/null; then
      echo "Required Termux pkg-config module is missing: $module" >&2
      exit 1
    fi
  done
else
  echo "  [dry-run] verify pkg-config modules: libsodium ncursesw portaudio-2.0 opus openssl uuid"
fi

have_pjsip=0
if [ "$DRY_RUN" -eq 0 ] && [ "$REBUILD_PJSIP" -eq 0 ] && pkg-config --exists libpjproject 2>/dev/null; then
  if [ "$(pkg-config --modversion libpjproject 2>/dev/null || true)" = "2.17" ]; then
    have_pjsip=1
  fi
fi
if [ "$have_pjsip" -eq 0 ]; then
  echo "==> Building managed PJSIP 2.17 for Termux/PortAudio/OpenSL ES"
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "  [dry-run] PJSIP_PREFIX=$PJSIP_PREFIX scripts/bootstrap-pjsip.sh"
  else
    PJSIP_PREFIX="$PJSIP_PREFIX" sh scripts/bootstrap-pjsip.sh
  fi
fi

if [ "$DRY_RUN" -eq 0 ]; then
  [ "$(pkg-config --modversion libpjproject 2>/dev/null || true)" = "2.17" ] || {
    echo "Managed PJSIP 2.17 did not validate after build." >&2
    exit 1
  }
fi

if [ "$CLEAN" -eq 1 ]; then run rm -rf "$BUILD_DIR"; fi

# Keep a Termux-owned temporary directory; mpv IPC and transfer temp files must
# never assume /tmp exists on Android.
export TMPDIR=${TMPDIR:-$TERMUX_PREFIX/tmp}
run mkdir -p "$TMPDIR"

echo "==> Configuring WaffleHouse-Client $APP_VERSION"
run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$TERMUX_PREFIX" \
  -DAPP_VERSION_STRING="$APP_VERSION"

echo "==> Building WaffleHouse-Client $APP_VERSION"
run cmake --build "$BUILD_DIR" -j "$JOBS"

if [ "$INSTALL" -eq 1 ]; then
  echo "==> Installing to $TERMUX_PREFIX/bin/wafflehouse-client"
  run cmake --install "$BUILD_DIR"
fi

cat <<DONE

WaffleHouse-Client $APP_VERSION build complete.

Run:
  wafflehouse-client

Useful Termux setup:
  termux-setup-storage       # optional shared-storage access for file/media paths
  /menu                      # compact WaffleHouse command map inside the client
  /audio-devices             # verify PJSIP sees Android/OpenSL audio devices

SIP microphone note:
  Install the matching Termux:API Android app and grant it Microphone permission.
  The 'termux-api' package installed above supplies the command-side integration.
DONE
