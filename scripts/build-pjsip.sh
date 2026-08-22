#!/bin/sh
set -eu

SRC=${1:-}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DEFAULT_PJSIP_SOURCE="$ROOT_DIR/third_party/pjproject"
USER_HOME=${HOME:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}
TERMUX_APP_PREFIX=${PREFIX:-}
PREFIX=${2:-$USER_HOME/.local/wafflehouse-pjsip}

if [ -z "$SRC" ] || [ ! -d "$SRC" ]; then
  echo "Usage: $0 /path/to/pjproject [install-prefix]" >&2
  exit 2
fi

SRC=$(CDPATH= cd -- "$SRC" && pwd)
mkdir -p "$PREFIX"
# Invalidate any previous WaffleHouse-Client compatibility stamp before touching the
# installation. If configure/build/install fails, the next top-level build will
# treat the prefix as stale instead of trusting a partially updated install.
rm -f "$PREFIX/.wafflehouse-pjsip-build"

HOST_OS=$(uname -s)
if [ -n "${TERMUX_VERSION:-}" ] || [ "${TERMUX_APP_PREFIX:-}" = "/data/data/com.termux/files/usr" ]; then
  HOST_OS=Termux
fi
case "$HOST_OS" in
  Termux)
    MAKE=make
    TM_CC=${CC:-clang}
    TM_CXX=${CXX:-clang++}
    ;;
  Linux)
    MAKE=make
    TM_CC=${CC:-cc}
    TM_CXX=${CXX:-c++}
    ;;
  FreeBSD)
    MAKE=gmake
    TM_CC=${WAFFLEHOUSE_PJSIP_CC:-${TRUNKMONKEY_PJSIP_CC:-/usr/bin/cc}}
    TM_CXX=${WAFFLEHOUSE_PJSIP_CXX:-${TRUNKMONKEY_PJSIP_CXX:-/usr/bin/c++}}
    [ -x "$TM_CC" ] || { echo "FreeBSD base C compiler not found: $TM_CC" >&2; exit 1; }
    [ -x "$TM_CXX" ] || { echo "FreeBSD base C++ compiler not found: $TM_CXX" >&2; exit 1; }
    "$TM_CXX" --version 2>/dev/null | grep -qi 'clang' || {
      echo "FreeBSD PJSUA2 must be built with base Clang/libc++; got: $TM_CXX" >&2
      exit 1
    }
    ;;
  MINGW*|MSYS*)
    HOST_OS=Windows
    MAKE=make
    TM_CC=${CC:-gcc}
    TM_CXX=${CXX:-g++}
    ;;
  *) echo "Unsupported OS for this helper: $HOST_OS" >&2; exit 2 ;;
esac

if ! command -v "$MAKE" >/dev/null 2>&1; then
  echo "$MAKE is required" >&2
  exit 1
fi

if command -v nproc >/dev/null 2>&1; then
  JOBS=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
  JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 1)
else
  JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
fi
case "$JOBS" in ''|*[!0-9]*) JOBS=1 ;; esac
[ "$JOBS" -gt 0 ] 2>/dev/null || JOBS=1

cd "$SRC"

# PJSUA2 2.17 Call::~Call() touches the numeric call slot even after the
# DISCONNECTED callback has removed the Call association. A delayed wrapper
# destructor can therefore assert after PJSUA has returned to NULL state, or
# touch/hang up a newer call that reused the same numeric slot. Replace the
# upstream destructor with an ownership-aware guard:
#   * PJSUA must be RUNNING (never NULL/CREATED/INIT/STARTING/CLOSING)
#   * the slot user-data must still point to this exact C++ Call object
#   * only an owned live slot may be cleared or implicitly hung up
CALL_CPP="$SRC/pjsip/src/pjsua2/call.cpp"
if [ ! -f "$CALL_CPP" ]; then
  echo "PJSIP 2.17 PJSUA2 source not found: $CALL_CPP" >&2
  exit 1
fi

tmp_call="$CALL_CPP.wafflehouse.$$"
awk '
  BEGIN { replacing=0; replaced=0; depth=0 }
  !replacing && $0 == "Call::~Call()" {
    print "Call::~Call()"
    if ((getline line) <= 0 || line != "{") {
      exit 41
    }
    print "{"
    print "    /* WaffleHouse-Client compatibility: only touch a PJSUA call slot while"
    print "     * the library is initialized and this wrapper still owns it. */"
    print "    bool owns_call = false;"
    print "    const pjsua_state state = pjsua_get_state();"
    print ""
    print "    if (id != PJSUA_INVALID_ID &&"
    print "        state == PJSUA_STATE_RUNNING &&"
    print "        pjsua_call_get_user_data(id) == this)"
    print "    {"
    print "        owns_call = true;"
    print "        pjsua_call_set_user_data(id, NULL);"
    print "    }"
    print ""
    print "    if (owns_call && isActive()) {"
    print "        try {"
    print "            CallOpParam prm;"
    print "            hangup(prm);"
    print "        } catch (Error &err) {"
    print "            PJ_UNUSED_ARG(err);"
    print "        }"
    print "    }"
    print "}"
    replacing=1
    replaced=1
    depth=1
    next
  }
  replacing {
    line=$0
    tmp=line
    opens=gsub(/\{/, "", tmp)
    tmp=line
    closes=gsub(/\}/, "", tmp)
    depth += opens - closes
    if (depth <= 0) {
      replacing=0
    }
    next
  }
  { print }
  END {
    if (!replaced || replacing) exit 42
  }
' "$CALL_CPP" > "$tmp_call" || {
  rm -f "$tmp_call"
  echo "Unable to replace PJSIP 2.17 Call destructor with WaffleHouse-Client lifetime guard." >&2
  exit 1
}
mv "$tmp_call" "$CALL_CPP"

if ! grep -q 'state == PJSUA_STATE_RUNNING' "$CALL_CPP" 2>/dev/null || \
   ! grep -q 'owns_call && isActive()' "$CALL_CPP" 2>/dev/null || \
   ! grep -q 'pjsua_call_get_user_data(id) == this' "$CALL_CPP" 2>/dev/null; then
  echo "Unable to verify WaffleHouse-Client PJSUA2 Call destructor lifetime fix." >&2
  exit 1
fi

# A forced PJSIP rebuild may change platform/backend options. PJSIP's GNU
# configure output embeds absolute source paths in generated files such as
# build.mak. If the WaffleHouse-Client directory is renamed or moved, "make distclean"
# may be unable to parse that stale build.mak at all. The default WaffleHouse-Client
# pjproject checkout is a disposable build cache, so clean all untracked and
# ignored generated state directly with git before reconfiguring. Tracked source
# files (and any deliberate tracked edits) are left untouched.
if [ "$SRC" = "$DEFAULT_PJSIP_SOURCE" ] && [ -d "$SRC/.git" ]; then
  echo "Resetting cached PJSIP generated state..."
  git -C "$SRC" clean -ffdx
elif [ -f build.mak ]; then
  echo "Cleaning previous PJSIP configure/build state..."
  if ! "$MAKE" distclean; then
    echo >&2
    echo "PJSIP distclean could not parse the existing generated state." >&2
    echo "This commonly happens after the PJSIP source directory was moved or renamed." >&2
    if [ -d "$SRC/.git" ]; then
      echo "Removing ignored generated files from the supplied git checkout and retrying cleanly..." >&2
      git -C "$SRC" clean -fdX
    else
      echo "Refusing to delete generated files from a non-git custom PJSIP source tree." >&2
      echo "Clean that source tree manually, or use WaffleHouse-Client's default third_party/pjproject checkout." >&2
      exit 1
    fi
  fi
fi

cat > pjlib/include/pj/config_site.h <<'CONFIG'
#pragma once
/* WaffleHouse-Client supports multiple simultaneous saved SIP identities. */
#define PJSUA_MAX_ACC 32
/* WaffleHouse-Client requires up to 50 simultaneous independent calls. */
#define PJSUA_MAX_CALLS 64
/* PJSIP recommends roughly three IO-queue handles per PJSUA call. */
#define PJ_IOQUEUE_MAX_HANDLES 256
#define PJMEDIA_HAS_VIDEO 0
CONFIG

if [ "$HOST_OS" = Termux ]; then
  cat >> pjlib/include/pj/config_site.h <<'CONFIG'
/* Termux: audio is provided by the packaged PortAudio OpenSL ES backend. */
#undef PJMEDIA_HAS_WEBRTC_AEC
#define PJMEDIA_HAS_WEBRTC_AEC 0
#undef PJMEDIA_HAS_WEBRTC_AEC3
#define PJMEDIA_HAS_WEBRTC_AEC3 0
CONFIG
fi

if [ "$HOST_OS" = FreeBSD ]; then
  cat >> pjlib/include/pj/config_site.h <<'CONFIG'
/* PJSIP 2.17's legacy WebRTC AEC archive is not reliably linkable on FreeBSD/x86_64
 * (missing WebRtc_GetCPUInfo/SSE2 glue in pjmedia-test). WaffleHouse-Client does not
 * require this optional AEC; PortAudio + Speex AEC remain available. */
#undef PJMEDIA_HAS_WEBRTC_AEC
#define PJMEDIA_HAS_WEBRTC_AEC 0
#undef PJMEDIA_HAS_WEBRTC_AEC3
#define PJMEDIA_HAS_WEBRTC_AEC3 0
CONFIG
fi

# Build static PJSIP objects as position-independent code. WaffleHouse-Client is
# normally linked as a PIE on modern Linux/FreeBSD toolchains; PIC avoids text
# relocation warnings and makes the static archives safer to consume.
PIC_CFLAGS="${CFLAGS:+$CFLAGS }-fPIC"
PIC_CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }-fPIC"

if [ "$HOST_OS" = Termux ]; then
  LOCALBASE=${TERMUX_APP_PREFIX:-/data/data/com.termux/files/usr}
  PC=${PKG_CONFIG:-pkg-config}
  for spec in "portaudio-2.0:portaudio:Android OpenSL ES audio" "opus:libopus:Opus codec" "openssl:openssl:TLS/SRTP crypto" "uuid:libuuid:SIP GUID generation"; do
    module=${spec%%:*}; rest=${spec#*:}; package=${rest%%:*}; purpose=${rest#*:}
    if ! "$PC" --exists "$module" 2>/dev/null; then
      echo "Termux PJSIP requires $package ($purpose). Install it with: pkg install $package" >&2
      exit 1
    fi
  done

  echo "Configuring PJSIP 2.17 for Termux with external PortAudio/OpenSL ES..."
  CFLAGS="-fPIC -I$LOCALBASE/include" \
  CXXFLAGS="-fPIC -I$LOCALBASE/include" \
  CPPFLAGS="-I$LOCALBASE/include" \
  LDFLAGS="-L$LOCALBASE/lib" \
  CC="$TM_CC" CXX="$TM_CXX" \
    ./configure --prefix="$PREFIX" --disable-video --with-external-pa \
      --with-opus="$LOCALBASE" --disable-libwebrtc --disable-upnp \
      --disable-opencore-amr --disable-silk --disable-libyuv --disable-sdl \
      --disable-ffmpeg --disable-openh264 --disable-vpx

  # PJSIP's generic Unix metadata may advertise libstdc++; Termux uses libc++.
  if grep -q -- '-lstdc++' build.mak 2>/dev/null; then
    tmp_mak="build.mak.wafflehouse.$$"
    sed 's/-lstdc++/-lc++/g' build.mak > "$tmp_mak"
    mv "$tmp_mak" build.mak
  fi
elif [ "$HOST_OS" = FreeBSD ]; then
  LOCALBASE=${LOCALBASE:-/usr/local}
  if command -v pkg-config >/dev/null 2>&1; then
    PC=pkg-config
  elif command -v pkgconf >/dev/null 2>&1; then
    PC=pkgconf
  else
    echo "pkgconf/pkg-config is required on FreeBSD" >&2
    exit 1
  fi
  for spec in "portaudio-2.0:portaudio:audio backend" "opus:opus:Opus codec" "libbcg729:bcg729:G.729 codec" "uuid:libuuid:robust SIP GUID generation"; do
    module=${spec%%:*}; rest=${spec#*:}; package=${rest%%:*}; purpose=${rest#*:}
    if ! "$PC" --exists "$module" 2>/dev/null; then
      echo "FreeBSD PJSIP requires $package ($purpose). Install package: $package" >&2
      exit 1
    fi
  done

  echo "Configuring PJSIP for FreeBSD with base Clang/libc++ + external PortAudio..."
  echo "  CC=$TM_CC"
  echo "  CXX=$TM_CXX"
  echo "  Deterministic features: PortAudio/Opus/bcg729/libuuid enabled; WebRTC/UPnP/AMR/SILK/video helpers disabled"
  echo "  IO queue: PJSIP default backend (experimental kqueue is intentionally not forced)"

  # Keep the managed FreeBSD dependency build deterministic. Do not inherit
  # host C/C++/link flags that could silently request libstdc++ or another ABI.
  CFLAGS="-fPIC -I$LOCALBASE/include" \
  CXXFLAGS="-fPIC -stdlib=libc++ -I$LOCALBASE/include" \
  CPPFLAGS="-I$LOCALBASE/include" \
  LDFLAGS="-L$LOCALBASE/lib" \
  CC="$TM_CC" CXX="$TM_CXX" \
    ./configure --prefix="$PREFIX" --disable-video --with-external-pa \
      --with-opus="$LOCALBASE" --with-bcg729="$LOCALBASE" \
      --disable-libwebrtc --disable-upnp \
      --disable-opencore-amr --disable-silk \
      --disable-libyuv --disable-sdl --disable-ffmpeg \
      --disable-openh264 --disable-vpx

  # PJSIP build.mak records the compilers applications are expected to use.
  # Verify configure actually honored the pinned FreeBSD base Clang toolchain
  # before spending time compiling static archives.
  grep -Fq "export APP_CC := $TM_CC" build.mak 2>/dev/null || {
    echo "FreeBSD PJSIP configure did not preserve CC=$TM_CC in build.mak." >&2
    exit 1
  }
  grep -Fq "export APP_CXX := $TM_CXX" build.mak 2>/dev/null || {
    echo "FreeBSD PJSIP configure did not preserve CXX=$TM_CXX in build.mak." >&2
    exit 1
  }
  grep -q -- '-stdlib=libc++' build.mak 2>/dev/null || {
    echo "FreeBSD PJSIP generated build.mak is missing -stdlib=libc++; refusing ABI-ambiguous build." >&2
    exit 1
  }

  # PJSIP 2.17 GNU build.mak.in hardcodes -lstdc++ for PJSUA2 on non-Darwin
  # targets. FreeBSD base C++ and packaged Qt use libc++, so keeping that flag
  # produces an ABI-mixed process and unresolved std::__cxx11 symbols. Rewrite
  # only the generated build.mak after configure; upstream source stays intact.
  if grep -q -- '-lstdc++' build.mak 2>/dev/null; then
    tmp_mak="build.mak.wafflehouse.$$"
    sed 's/-lstdc++/-lc++/g' build.mak > "$tmp_mak"
    mv "$tmp_mak" build.mak
  fi
  if grep -q -- '-lstdc++' build.mak 2>/dev/null; then
    echo "Unable to remove libstdc++ from FreeBSD PJSIP generated link metadata." >&2
    exit 1
  fi
  grep -q -- '-lc++' build.mak 2>/dev/null || {
    echo "FreeBSD PJSIP generated build.mak does not link libc++; refusing ABI-ambiguous build." >&2
    exit 1
  }

elif [ "$HOST_OS" = Windows ]; then
  echo "Configuring PJSIP for Windows/MinGW-w64..."
  CFLAGS="${CFLAGS:-}-O2" CXXFLAGS="${CXXFLAGS:-}-O2" CC="$TM_CC" CXX="$TM_CXX" \
    ./configure --prefix="$PREFIX" --disable-video
else
  CFLAGS="$PIC_CFLAGS" CXXFLAGS="$PIC_CXXFLAGS" CC="$TM_CC" CXX="$TM_CXX" \
    ./configure --prefix="$PREFIX" --disable-video
fi

"$MAKE" dep
# WaffleHouse-Client consumes the libraries only. PJSIP's default `make all` also
# links sample/test executables, which can fail for optional components that
# WaffleHouse-Client never uses. Upstream documents `make lib` for library-only builds.
"$MAKE" -j"$JOBS" lib

if [ "$HOST_OS" = FreeBSD ]; then
  # Prove the PJSUA2 archive itself was compiled against libc++, not merely
  # that the generated pkg-config metadata was rewritten. The failure we saw
  # on FreeBSD presented as hundreds of unresolved std::__cxx11 symbols.
  pjsua2_archive=
  for candidate in "$SRC"/pjsip/lib/libpjsua2-*.a; do
    if [ -f "$candidate" ]; then
      pjsua2_archive=$candidate
      break
    fi
  done
  [ -n "$pjsua2_archive" ] || {
    echo "FreeBSD PJSIP build did not produce a libpjsua2 static archive." >&2
    exit 1
  }
  if command -v nm >/dev/null 2>&1; then
    if nm -C "$pjsua2_archive" 2>/dev/null | grep -q 'std::__cxx11::'; then
      echo "FreeBSD PJSUA2 archive contains libstdc++ ABI symbols (std::__cxx11); refusing mixed-ABI install." >&2
      exit 1
    fi
    nm -C "$pjsua2_archive" 2>/dev/null | grep -q 'std::__1::' || {
      echo "Unable to verify libc++ ABI symbols (std::__1) in FreeBSD PJSUA2 archive." >&2
      exit 1
    }
  else
    echo "nm is required to validate the FreeBSD PJSUA2 C++ ABI." >&2
    exit 1
  fi
fi

# The compile completed. Remove only previously installed PJSIP headers/libs
# now, so stale archives from an older ABI/configuration cannot survive install.
rm -rf "$PREFIX/include" "$PREFIX/lib"
mkdir -p "$PREFIX"
"$MAKE" install

case "$HOST_OS" in
  Termux)
    TM_OS=termux
    pc="$PREFIX/lib/pkgconfig/libpjproject.pc"
    [ -f "$pc" ] || { echo "PJSIP install did not create $pc" >&2; exit 1; }
    if grep -q -- '-lstdc++' "$pc"; then
      sed 's/-lstdc++/-lc++/g' "$pc" > "$pc.tmp" && mv "$pc.tmp" "$pc"
    fi
    BUILD_ID="2.17-wafflehouse-acc32-call64-pic-termux-$(uname -m 2>/dev/null || echo unknown)-v1"
    ;;
  Linux)
    TM_OS=linux
    BUILD_ID="2.17-wafflehouse-acc32-call64-pic-linux-$(uname -m 2>/dev/null || echo unknown)-v10"
    ;;
  Windows)
    TM_OS=windows
    BUILD_ID="2.17-sak64-windows-mingw-$(uname -m 2>/dev/null || echo x86_64)-v1"
    ;;
  FreeBSD)
    TM_OS=freebsd
    pc="$PREFIX/lib/pkgconfig/libpjproject.pc"
    [ -f "$pc" ] || { echo "PJSIP install did not create $pc" >&2; exit 1; }
    if grep -q -- '-lstdc++' "$pc"; then
      echo "FreeBSD PJSIP install still advertises libstdc++; refusing mixed C++ ABI." >&2
      exit 1
    fi
    grep -q -- '-lc++' "$pc" || { echo "FreeBSD PJSIP install does not advertise libc++." >&2; exit 1; }
    for forbidden in -lwebrtc -lupnp -lixml; do
      if grep -q -- "$forbidden" "$pc"; then
        echo "FreeBSD deterministic PJSIP build unexpectedly advertises $forbidden." >&2
        exit 1
      fi
    done
    for required in -lportaudio -lopus -lbcg729 -luuid -lc++; do
      grep -q -- "$required" "$pc" || { echo "FreeBSD PJSIP install is missing required linkage: $required" >&2; exit 1; }
    done
    abi=$(uname -K 2>/dev/null || uname -r 2>/dev/null || echo unknown)
    cxxver=$($TM_CXX --version 2>/dev/null | sed -n '1s/.*clang version \([^ ]*\).*/\1/p')
    [ -n "$cxxver" ] || cxxver=unknown
    cxxver=$(printf '%s' "$cxxver" | tr -c 'A-Za-z0-9._-' '_')
    BUILD_ID="2.17-wafflehouse-acc32-call64-pic-freebsd-$(uname -m 2>/dev/null || echo unknown)-libcxx-${abi}-${cxxver}-v10"
    ;;
esac
printf '%s\n' "$BUILD_ID" > "$PREFIX/.wafflehouse-pjsip-build"

echo
echo "PJSIP installed to $PREFIX"
echo "PJSUA_MAX_ACC is configured for 32; PJSUA_MAX_CALLS is configured for 64; PJ_IOQUEUE_MAX_HANDLES is configured for 256."
if [ "$HOST_OS" = Termux ]; then
  echo "Termux audio backend: external PortAudio using the Termux OpenSL ES host API."
  echo "Grant microphone permission to the matching Termux:API app for SIP capture."
elif [ "$HOST_OS" = Windows ]; then
  echo "Windows audio backend: PJSIP native Windows audio backend."
  echo "Windows CLI/GUI use the same PJSUA2 core as Linux/FreeBSD."
elif [ "$HOST_OS" = FreeBSD ]; then
  echo "FreeBSD audio backend: external PortAudio."
  echo "FreeBSD C++ ABI: base Clang/libc++."
  echo "FreeBSD optional PJSIP deps: deterministic/minimal (PortAudio/Opus/G.729/libuuid enabled; no WebRTC AEC, UPnP, AMR, SILK, or video extras)."
  echo "PJSUA2 Call lifetime compatibility guard: enabled."
fi
