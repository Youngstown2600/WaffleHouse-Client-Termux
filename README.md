# WaffleHouse-Client 3.2-Termux

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
unzip WaffleHouse-Client-3.2-Termux.zip
cd WaffleHouse-Client-3.2-Termux
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
