# WaffleHouse-Client-Termux Build 0.8

A dedicated Android/Termux CLI build of WaffleHouse-Client. This is **not** the Linux desktop binary with `--cli` forced on: the Termux target has its own entry point and compiles no Qt Widgets/GUI frontend code.

## Preserved features

- AIM/OSCAR accounts, buddy presence, IM and private rooms
- IRC accounts, channels and IRC `/commands`
- Telnet/BBS sessions with ANSI terminal emulation
- SIP/PJSIP 2.17 softphone, multiple accounts, calls, DTMF, hold/mute and SIP diagnostics
- Secure rooms and encrypted direct messaging
- Secure/unsecured file transfer and direct transfer
- Saved profiles/accounts and CLI themes
- Media playback, streams and playlists retained from the supplied 3.1r8 baseline

## Termux-native differences

- CLI only; no desktop Qt Widgets or X11 window is compiled into WaffleHouse
- Qt Core + Network remain because the existing protocol engines are built on them
- `pkg` is used for Termux packages
- no CPAN or Perl command is used by the WaffleHouse builder
- Termux's own `xdg-open`/`termux-open-url` is reused
- shared Downloads storage is used when `termux-setup-storage` has been granted
- PJSIP is built specifically for Termux using the Termux PortAudio/OpenSL ES and libopus packages

## Build

```sh
pkg install unzip
unzip WaffleHouse-Client-Termux-Build-0.8.zip
cd WaffleHouse-Client-Termux-Build-0.8
chmod +x build.sh
./build.sh --clean
```

Then run:

```sh
wafflehouse-client
```

For Android shared storage:

```sh
termux-setup-storage
```

## Builder options

```text
./build.sh --clean       clean application build
./build.sh --pjsip       force rebuild of managed PJSIP 2.17
./build.sh --test        run source/parity tests only
./build.sh --uninstall   uninstall WaffleHouse-Client files
```

## Termux runtime paths

WaffleHouse-Client uses Termux's writable `$TMPDIR` (normally `$PREFIX/tmp`) for SIP logs, sockets, and temporary runtime data. It does not assume Android exposes a writable `/tmp`. If Termux temp variables are unavailable, Android falls back to `$HOME/.cache/wafflehouse-client/tmp`.

## Termux SIP audio / RTP notes (Build 0.8)

Build 0.8 deliberately decouples SIP/RTP from Android hardware audio. PJSIP starts
with a null sound device, so a missing microphone permission can no longer prevent
an INVITE from being sent or RTP/RTCP sockets from being created. Once call media
is negotiated, WaffleHouse attempts full-duplex audio. If microphone capture cannot
open, the call remains alive and WaffleHouse attempts playback-only audio.

For microphone capture, install the **Termux:API Android add-on** from the same
source/signing family as the main Termux app, then grant its Microphone permission.
The `pkg install termux-api` package supplies only the command-line client; it does
not install the Android add-on APK.

After installation, test the Android microphone permission path with:

```sh
wafflehouse-audio-preflight
```

Then use `/audio-devices` and `/audio-reopen` in WaffleHouse. RTP diagnostics remain
available through the Softphone call report/media views and include local/remote
RTP/RTCP addresses, codec, packet counts, loss, jitter, discard, RTT and jitter
buffer delay.
