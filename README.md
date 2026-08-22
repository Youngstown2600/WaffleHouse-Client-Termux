# WaffleHouse-Client 3.2-Termux

A dedicated Android/Termux CLI port of WaffleHouse-Client. This package is based on the supplied 3.1r8 source and keeps the CLI feature set while replacing desktop-only launch/build assumptions with Termux-native behavior.

## Included CLI features

- AIM/OSCAR accounts, buddy list, IM, presence/AFK, private rooms and version query
- IRC accounts, channels, private messages and IRC slash commands
- Telnet/BBS terminal support and ANSI rendering
- Secure direct messaging and Secure Rooms
- Secure and unsecured file transfer, including direct-transfer support
- Multi-account SIP/PJSIP softphone
- SIP calls, answer/reject/hangup/hold/resume/mute/DTMF, activity, SIP log and ladder
- SIP audio device selection and automatic audio selection
- Themes, notifications, saved profiles and multi-connection status UI
- Local media, HTTP/HLS streams, SHOUTcast/Icecast, queue/playlist controls, seek, volume, shuffle/repeat and EQ

The old edition branding has been removed. Media/radio functionality remains part of WaffleHouse-Client itself.

## Build in Termux

Use a current Termux installation from F-Droid or GitHub rather than the obsolete Play Store build.

```sh
pkg update
pkg upgrade
termux-setup-storage   # optional, enables ~/storage paths
unzip WaffleHouse-Client-3.2-Termux.zip
cd WaffleHouse-Client-3.2-Termux
chmod +x build.sh
./build.sh --clean
```

The builder installs the required Termux packages, builds the managed PJSIP 2.17 dependency when needed, compiles the CLI-only executable, and installs it as:

```text
$PREFIX/bin/wafflehouse-client
```

Run it with:

```sh
wafflehouse-client
```

## SIP audio on Android

The Termux build uses PJSIP/PJMEDIA with external PortAudio. Termux's PortAudio package supplies an Android OpenSL ES host backend. Install the matching **Termux:API Android app**, grant it microphone permission, and keep the `termux-api` package installed inside Termux.

Inside WaffleHouse, verify devices with:

```text
/audio-devices
/audio-auto
```

You can choose a specific input/output device with `/audio-use`.

## Phone-sized terminal UI

The CLI automatically uses a compact startup banner on narrow terminals and accepts widths down to 30 columns. `/menu` opens a compact command map so the client remains practical without function keys or an external keyboard. All existing slash commands remain available through the normal input line.

## File transfers and storage

After `termux-setup-storage`, incoming transfers without an explicit destination prefer:

```text
~/storage/downloads
```

Otherwise WaffleHouse falls back to Qt's download location or the Termux home directory.

## Media

`mpv` is used for local media and stream playback. Media support includes local paths, HTTP/HLS streams, SHOUTcast/Icecast URLs, queue/playlist controls and EQ. The supplied 3.1r8 baseline had already removed YouTube-specific playback; 3.2-Termux preserves that baseline behavior.

## Useful commands

```text
/menu
/help
/connections
/add
/buddies
/join
/msg
/secure
/sendfile
/transfers
/phone
/dial
/calls
/siplog
/ladder
/audio-devices
/media
/mplaylist
/themes
/options
```

## Build options

```text
./build.sh --clean
./build.sh --upgrade
./build.sh --uninstall
./build.sh --pjsip
./build.sh --no-auto-deps
./build.sh --no-install
./build.sh --dry-run
./build.sh --jobs N
```

## Platform scope

This package is the **Termux CLI build**. It intentionally does not build or launch the Qt Widgets desktop GUI. The shared protocol, security, file-transfer, SIP and media code remains in the source tree.
