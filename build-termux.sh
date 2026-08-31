#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

APP="WaffleHouse-Termux 1.0r1"
ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX_EXPECTED="/data/data/com.termux/files/usr"
BUILD="$ROOT/build-termux"
PJROOT="$ROOT/.termux/pjproject-2.17"
PJINSTALL="$PREFIX_EXPECTED/opt/wafflehouse-pjsip-2.17"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
TMPBASE="${TMPDIR:-$PREFIX_EXPECTED/tmp}"

say(){ printf '\n==> %s\n' "$*"; }
fail(){ printf '\nERROR: %s\n' "$*" >&2; exit 1; }

if [[ "${PREFIX:-}" != "$PREFIX_EXPECTED" ]] || [[ ! -d "$PREFIX_EXPECTED" ]]; then
  fail "$APP must be built inside Termux (expected PREFIX=$PREFIX_EXPECTED)."
fi

for arg in "$@"; do
  case "$arg" in --clean|--pjsip|--test|--upgrade|--uninstall|--help|-h) ;; *) fail "Unknown option: $arg";; esac
done
if [[ " ${*:-} " == *" --help "* || " ${*:-} " == *" -h "* ]]; then
  cat <<USAGE
$APP builder
  ./build.sh --clean       clean app build
  ./build.sh --pjsip       force rebuild managed PJSIP 2.17
  ./build.sh --test        run source/parity tests only
  ./build.sh --upgrade     rebuild/reinstall current source tree
  ./build.sh --uninstall   remove installed WaffleHouse files
USAGE
  exit 0
fi
if [[ " ${*:-} " == *" --uninstall "* ]]; then
  rm -f "$PREFIX/bin/wafflehouse-termux" "$PREFIX/bin/wafflehouse-client"
  rm -rf "$PREFIX/share/wafflehouse-termux" "$PREFIX/share/wafflehouse-client"
  printf '%s removed.\n' "$APP"
  exit 0
fi

repair_package_state(){
  say "Checking Termux package state"
  # Clean only obsolete WaffleHouse experimental shim packages from earlier builds.
  for pkgname in wafflehouse-xdg-utils-compat wafflehouse-termux-xdg-provider; do
    if dpkg-query -W -f='${Status}\n' "$pkgname" 2>/dev/null | grep -q 'install ok installed'; then
      dpkg --remove --force-remove-reinstreq "$pkgname" >/dev/null 2>&1 || true
    fi
  done
  # If CPAN-backed xdg-utils was interrupted, remove that broken package before configure.
  local xdg_status="$TMPBASE/wh-xdg-status.$$"
  mkdir -p "$TMPBASE"
  if dpkg-query -W -f='${Status}\n' xdg-utils >"$xdg_status" 2>/dev/null; then
    if ! grep -q 'install ok installed' "$xdg_status"; then
      dpkg --remove --force-remove-reinstreq xdg-utils >/dev/null 2>&1 || true
    fi
  fi
  rm -f "$xdg_status"
  dpkg --configure -a >/dev/null 2>&1 || true
}

install_xdg_provider(){
  # qt6-qtbase is distributed from the Termux X11 repository and declares
  # xdg-utils as a package dependency. The WaffleHouse CLI uses neither its
  # desktop MIME database nor its Perl/CPAN helper. If a healthy xdg-utils is
  # already installed we leave it alone. Otherwise install a metadata-only
  # provider. It contains NO files and therefore cannot overwrite termux-tools'
  # existing xdg-open command.
  if dpkg-query -W -f='${Status}\n' xdg-utils 2>/dev/null | grep -q 'install ok installed'; then
    say "Healthy xdg-utils already installed; using it unchanged"
    return
  fi
  if ! command -v xdg-open >/dev/null 2>&1; then
    say "xdg-open missing; installing its proper Termux owner (termux-tools)"
    pkg install -y termux-tools
  else
    say "Using existing Termux xdg-open: $(command -v xdg-open)"
  fi

  local stage="$ROOT/.termux/xdg-provider-stage"
  local deb="$ROOT/.termux/wafflehouse-termux-xdg-provider.deb"
  rm -rf "$stage"
  mkdir -p "$ROOT/.termux"
  install -d -m 0755 "$stage/DEBIAN"
  cat > "$stage/DEBIAN/control" <<CTRL
Package: wafflehouse-termux-xdg-provider
Version: 1.0.1
Architecture: all
Maintainer: WaffleHouse-Termux
Provides: xdg-utils
Conflicts: xdg-utils
Description: Metadata-only xdg-utils provider for WaffleHouse-Termux 1.0r1
 Contains no files. Termux's existing xdg-open command remains owned by termux-tools.
CTRL
  chmod 0644 "$stage/DEBIAN/control"
  dpkg-deb --build "$stage" "$deb" >/dev/null
  dpkg -i "$deb" >/dev/null
}

install_dependencies(){
  say "Refreshing Termux repositories"
  pkg update -y
  pkg install -y x11-repo
  pkg update -y
  repair_package_state
  install_xdg_provider
  say "Installing Termux-native dependencies"
  pkg install -y \
    clang cmake make ninja pkg-config git curl ca-certificates python \
    qt6-qtbase libsodium ncurses openssl libuuid portaudio libopus \
    mpv ffmpeg termux-api
}

pjsip_limits_ok(){
  local cfg="$PJINSTALL/include/pj/config_site.h"
  [[ -f "$PJINSTALL/lib/pkgconfig/libpjproject.pc" ]] || return 1
  [[ -f "$cfg" ]] || return 1
  PKG_CONFIG_PATH="$PJINSTALL/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
    pkg-config --exact-version=2.17 libpjproject >/dev/null 2>&1 || return 1
  grep -Eq '^[[:space:]]*#define[[:space:]]+PJSUA_MAX_ACC[[:space:]]+32([[:space:]]|$)' "$cfg" || return 1
  grep -Eq '^[[:space:]]*#define[[:space:]]+PJSUA_MAX_CALLS[[:space:]]+64([[:space:]]|$)' "$cfg" || return 1
  grep -Eq '^[[:space:]]*#define[[:space:]]+PJ_IOQUEUE_MAX_HANDLES[[:space:]]+256([[:space:]]|$)' "$cfg" || return 1
  # A native Termux process has no Java VM. PJSIP's Android target enables
  # its JNI audio device by default, so require our explicit PortAudio-only
  # configuration before trusting a cached managed PJSIP install.
  grep -Eq '^[[:space:]]*#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO[[:space:]]+1([[:space:]]|$)' "$cfg" || return 1
  grep -Eq '^[[:space:]]*#define[[:space:]]+PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI[[:space:]]+0([[:space:]]|$)' "$cfg" || return 1
  # Native Termux has no Java VM. WaffleHouse-Termux 1.0r1 replaces PJSIP's Android/JNI
  # UUID backend with upstream guid_simple.o so SIP branch/Call-ID/tag values
  # are valid in a standalone terminal process.
  [[ -f "$PJINSTALL/.wafflehouse-termux-guid-backend" ]] || return 1
  grep -qx 'guid_simple' "$PJINSTALL/.wafflehouse-termux-guid-backend" || return 1
}

build_pjsip(){
  local force="${1:-0}"
  if [[ "$force" != 1 ]] && pjsip_limits_ok; then
    say "Managed PJSIP 2.17 already installed with WaffleHouse SIP limits"
    return
  fi
  if [[ "$force" != 1 && -f "$PJINSTALL/lib/pkgconfig/libpjproject.pc" ]]; then
    say "Existing PJSIP 2.17 has stale/default limits; rebuilding automatically"
  fi

  say "Building PJSIP 2.17 for native Termux"
  mkdir -p "$ROOT/.termux"
  rm -rf "$PJROOT"
  # This prefix is owned by the WaffleHouse builder. Remove stale headers and
  # archives so a previous default-limit PJSIP install cannot leak into the
  # rebuilt dependency.
  rm -rf "$PJINSTALL"
  git clone --depth 1 --branch 2.17 https://github.com/pjsip/pjproject.git "$PJROOT"

  # PJSIP deliberately selects guid_android.o whenever the compiler target is
  # Android. That implementation calls java.util.UUID through JNI. A native
  # Termux executable has no Java VM, so pj_generate_unique_string() fails and
  # REGISTER messages end up with empty Call-ID/From tags and NUL-filled Via
  # branches. Keep the Android platform target, but use PJSIP's own native
  # non-JNI GUID backend for this target.
  python3 - "$PJROOT/aconfigure" <<'PYGUID'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
old = 'ac_os_objs="$ac_os_objs guid_android.o"'
new = 'ac_os_objs="$ac_os_objs guid_simple.o"'
if old not in s:
    raise SystemExit("Unable to locate PJSIP Android GUID backend selection")
p.write_text(s.replace(old, new, 1))
PYGUID
  mkdir -p "$PJROOT/pjlib/include/pj"
  cat > "$PJROOT/pjlib/include/pj/config_site.h" <<'CFG'
#pragma once
/* Termux/Android platform settings. */
#define PJ_CONFIG_ANDROID 1
#define PJ_HAS_IPV6 1
#define PJMEDIA_HAS_VIDEO 0
#define PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO 1
/* Termux is a native executable, not a Java/JNI Android application.
 * PJSIP defaults this backend to PJ_ANDROID (enabled), which requires a JVM.
 * Audio is provided by Termux PortAudio/OpenSL ES instead. */
#define PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI 0
#define PJMEDIA_AUDIO_DEV_HAS_OPENSL 0
#define PJMEDIA_AUDIO_DEV_HAS_OBOE 0
/* Avoid Android MediaCodec/JNI codec factories in the native Termux process. */
#define PJMEDIA_HAS_AND_MEDIA_AMRNB 0
#define PJMEDIA_HAS_AND_MEDIA_AMRWB 0
#define PJMEDIA_HAS_AND_MEDIA_H264 0
#define PJMEDIA_HAS_AND_MEDIA_VP8 0
#define PJMEDIA_HAS_AND_MEDIA_VP9 0

/* WaffleHouse-Client SIP capacity requirements. Keep these in the
 * PJSIP build itself so the libraries and application headers agree. */
#define PJSUA_MAX_ACC 32
#define PJSUA_MAX_CALLS 64
#define PJ_IOQUEUE_MAX_HANDLES 256
CFG

  cd "$PJROOT"
  export CFLAGS="-I$PREFIX/include"
  export CXXFLAGS="-I$PREFIX/include"
  export LDFLAGS="-L$PREFIX/lib"
  export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  ./configure \
    --prefix="$PJINSTALL" \
    --with-external-pa \
    --with-opus="$PREFIX" \
    --with-ssl="$PREFIX" \
    --disable-video

  # Fail before compilation if upstream configure logic changes and the JNI
  # GUID object slips back in. os-auto.mak is the authoritative PJLIB object
  # list produced by aconfigure.
  if ! grep -q 'guid_simple.o' "$PJROOT/pjlib/build/os-auto.mak" || \
     grep -q 'guid_android.o' "$PJROOT/pjlib/build/os-auto.mak"; then
    echo "PJSIP Termux GUID configuration failed: expected guid_simple.o and no guid_android.o." >&2
    exit 1
  fi
  make dep
  make -j"$JOBS"
  make install
  printf '%s\n' 'guid_simple' > "$PJINSTALL/.wafflehouse-termux-guid-backend"

  # PJSIP installs public headers used by WaffleHouse. Refuse to continue if
  # the installed headers do not expose the exact limits used to build the
  # libraries; otherwise the C++ static assertions fail later with defaults.
  if ! pjsip_limits_ok; then
    echo "Installed PJSIP 2.17 did not preserve WaffleHouse SIP limits." >&2
    echo "Expected PJSUA_MAX_ACC=32, PJSUA_MAX_CALLS=64, PJ_IOQUEUE_MAX_HANDLES=256." >&2
    exit 1
  fi
  say "Verified PJSIP: accounts=32 calls=64 ioqueue=256 audio=PortAudio JNI=off GUID=guid_simple"
}

run_tests(){
  say "Running WaffleHouse-Termux 1.0r1 regression gates"
  bash "$ROOT/tests/run-termux-tests.sh"
  bash "$ROOT/tests/termux_sip_password_persistence_test.sh"
  bash "$ROOT/tests/termux_sip_runtime_test.sh"
}

if [[ " ${*:-} " == *" --test "* ]]; then run_tests; exit 0; fi
install_dependencies
force_pj=0; [[ " ${*:-} " == *" --pjsip "* ]] && force_pj=1
build_pjsip "$force_pj"
run_tests
[[ " ${*:-} " == *" --clean "* ]] && rm -rf "$BUILD"
mkdir -p "$BUILD"

say "Configuring $APP"
export PKG_CONFIG_PATH="$PJINSTALL/lib/pkgconfig:$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"

say "Compiling $APP"
cmake --build "$BUILD" -j "$JOBS"
say "Installing $APP"
cmake --install "$BUILD"

# New canonical command plus backward-compatible WaffleHouse command.
ln -sf "$PREFIX/bin/wafflehouse-termux" "$PREFIX/bin/wafflehouse-client"

cat <<DONE

$APP installed successfully.
Run: wafflehouse-termux
Compatibility alias: wafflehouse-client
Optional shared Android storage: termux-setup-storage
SIP audio inspection: /audio-devices
Android microphone permission test: wafflehouse-audio-preflight
DONE
