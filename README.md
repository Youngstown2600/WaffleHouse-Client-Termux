# WaffleHouse-Termux 1.0r1

**WaffleHouse-Termux 1.0r1** is the native Android/Termux CLI edition of WaffleHouse. This release moves the old Termux 0.9 branch onto the **WaffleHouse-Client 5.1 protocol/core baseline** while preserving the Android-specific runtime fixes that made the Termux build usable in the first place.

It is intentionally a terminal application: no Qt Widgets GUI and no X11 requirement. The executable is `wafflehouse-termux`; `wafflehouse-client` is installed as a compatibility alias.

## What moved to the 5.1 core

- AIM/OSCAR with the 5.1 NINA/NINAPatcher-compatible sign-on path, `auto` / `nina` / `custom` network profiles, native buddy presence/idle state, login/full-wire diagnostic modes, redirected AIM chat services, profiles, rooms, typing, secure messaging/file-transfer plumbing, and OSCAR voice support.
- IRC with the current 5.1 CLI behavior and commands.
- Telnet/BBS with saved **exact columns × rows**, NAWS updates, responsive rendering, and auto-fit behavior adapted for Termux.
- SIP/VoIP with concurrent account support, current call controls, blind/attended transfer, 5.1 diagnostics, remote-audio plumbing, and server compatibility profiles for generic SIP, Asterisk `chan_pjsip`, and legacy `chan_sip`.
- The unified 5.x contact, history, and capability stores.
- Media/stream commands, themes, secure rooms, encrypted transfers, notification handling, and the newer CLI command surface.

## Telnet/BBS auto-fit on Termux

Desktop WaffleHouse 5.1 can directly shrink the Qt terminal widget's font until the configured BBS grid fits. A native ncurses process inside Termux cannot directly control the Termux app's font size, so 1.0 implements the closest correct equivalent:

1. A BBS profile keeps its requested geometry (for example 80×24 or 132×24).
2. With **Auto-fit BBS geometry** enabled, WaffleHouse-Termux requests an outer terminal size large enough for that grid.
3. It reads the real Android/Termux terminal dimensions with `TIOCGWINSZ` and tracks live changes.
4. If Termux accepts the resize—or you pinch the Termux font smaller / rotate the device so the grid fits—the configured exact size is used and advertised through Telnet NAWS.
5. If the phone still cannot physically show the requested grid, WaffleHouse uses the largest grid that actually fits the visible pane instead of rendering off-screen.

The saved profile is **not** permanently reduced by a small phone screen. When more cells become available, the session automatically grows back toward the requested geometry.

## Build in Termux

From the extracted source directory:

```sh
chmod +x build.sh build-termux.sh client-up.sh
./build.sh
```

The builder installs the required Termux packages, builds/validates the managed **PJSIP 2.17** configuration, builds WaffleHouse-Termux, installs it under `$PREFIX/bin`, and creates the compatibility alias.

Run it with:

```sh
wafflehouse-termux
```

The old command also works after installation:

```sh
wafflehouse-client
```

To rebuild the current tree later:

```sh
./client-up.sh
```

Useful builder switches:

```text
./build.sh --clean       clean WaffleHouse build output
./build.sh --pjsip       force rebuild managed PJSIP 2.17
./build.sh --test        run the Termux source/parity checks
./build.sh --upgrade     rebuild/reinstall this source tree
./build.sh --uninstall   remove installed launchers/files
```

## SIP/Android runtime behavior retained from 0.9

The 5.1 engine merge deliberately keeps the native-Termux protections from the known-good 0.9 branch:

- PJSIP's Android JNI GUID backend is replaced by its native `guid_simple` backend; startup also performs a GUID sanity check.
- PortAudio is used instead of PJSIP's Android JNI/OpenSL/Oboe audio backends.
- SIP/RTP can start with a null audio device and attach hardware once a call needs it.
- If capture cannot open, playback/speaker-only mode is attempted without dropping the SIP/RTP session.
- Runtime scratch paths prefer Termux's `TMPDIR`, `PREFIX/tmp`, and user cache paths instead of assuming `/tmp` exists.
- Saved SIP passwords are not erased merely because a registration timed out or the network failed.

For the Android microphone/API permission check:

```sh
wafflehouse-audio-preflight
```

## File downloads and Android helpers

Incoming files default to `~/storage/downloads` when Termux shared storage is available. WaffleHouse-Termux also uses `termux-open-url` for browser handoff and `termux-media-player` when available for notification audio.

If shared storage has not been enabled yet, run:

```sh
termux-setup-storage
```

## Source validation

The release contains a static parity/regression gate:

```sh
./tests/run-termux-tests.sh
./tests/termux_sip_password_persistence_test.sh
./tests/termux_sip_runtime_test.sh
```

These checks verify the 5.1 engine markers, Termux-specific SIP fixes, BBS geometry logic, responsive TUI behavior, and installer wiring. A real Android/Termux compile and live network/audio test still needs to be performed on an actual Termux installation because the release-preparation environment is not an Android/Termux runtime.

## 1.0r1 Termux Qt dependency fix

If 1.0 stopped at CMake with `Qt6Multimedia_FOUND = FALSE` / `Qt6Gui could not be found`, use this revision. The native CLI no longer asks CMake for Qt Multimedia or Qt Gui. OSCAR Voice uses the same Termux-native PortAudio layer already installed for voice work, while media playback continues to use `mpv`/`ffmpeg` through the CLI controller.
