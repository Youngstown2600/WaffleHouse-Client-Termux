# Building WaffleHouse-Client 3.2-Termux

## 1. Install Termux

Use a current Termux release from F-Droid or the official Termux GitHub releases. Do not use the obsolete Google Play build.

## 2. Extract the source

```sh
pkg install unzip
unzip WaffleHouse-Client-3.2-Termux.zip
cd WaffleHouse-Client-3.2-Termux
```

## 3. Build and install

```sh
chmod +x build.sh scripts/*.sh
./build.sh --clean
```

The build script enables the Termux X11 package repository because Qt 6 libraries are supplied there, but WaffleHouse itself remains a terminal application and does not require an X11 server to run.

The builder installs Clang, CMake, Qt 6 Core/Gui/Network libraries, libsodium, ncurses, OpenSSL, libuuid, PortAudio, Opus, mpv, FFmpeg and Termux:API support. It also builds a managed PJSIP 2.17 copy configured for Termux.

Qt 6 currently pulls Termux `xdg-utils`, whose post-install hook retrieves Perl `File::MimeInfo` from CPAN. The builder uses a temporary CPAN configuration that prefers `https://cpan.metacpan.org/`, so a timeout at `www.cpan.org` does not block WaffleHouse installation or overwrite the user's personal CPAN settings.

## 4. Android permissions

For SIP microphone capture, install the matching Termux:API Android app and grant its Microphone permission. Inside Termux the `termux-api` package is installed by the builder.

For convenient access to shared Downloads/media files:

```sh
termux-setup-storage
```

## 5. Run

```sh
wafflehouse-client
```

For a source-tree-only build without installing:

```sh
./build.sh --clean --no-install
./build-termux/wafflehouse-client
```

## 6. First checks

Inside WaffleHouse:

```text
/menu
/audio-devices
/audio-auto
/media
```

Then restore/add your AIM, IRC, Telnet/BBS and SIP profiles through the normal CLI account commands.
