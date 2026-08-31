# WaffleHouse-Termux 1.0 merge notes

This tree uses the former native Termux 0.9 branch as the Android runtime foundation and the WaffleHouse-Client 5.1 shared sources as the protocol/core authority.

## 5.1 components merged

- AIM/OSCAR backend/protocol including NINA compatibility and current redirects/presence/debug behavior
- OSCAR voice engine and CLI commands
- IRC backend/current CLI surface
- Telnet/BBS profile geometry and current ANSI/BBS engine
- SIP backend/controller and current sipcore, including Asterisk compatibility, multi-account behavior, transfer controls, diagnostics, and RemoteSipAudio
- unified capability/contact/history core
- media, secure channel/rooms, file transfer, notifications, user activity, themes, and current CLI command additions

## Termux-specific behavior intentionally preserved or re-applied

- CLI-only `main_termux.cpp` and ncurses shell
- responsive phone-sized layout and adaptive dialogs
- PJSIP 2.17 native GUID backend validation
- PortAudio-only PJSIP device build; Android JNI/OpenSL/Oboe disabled
- deferred hardware audio/null-device RTP start and speaker-only fallback
- Termux-safe TMPDIR/PREFIX runtime paths
- SIP saved-password protection on timeouts and idempotent disconnect after timeout
- startup error visibility in the TUI
- shared Android Downloads, `termux-open-url`, and `termux-media-player`

## BBS sizing adaptation

The desktop 5.1 Qt widget can change its own font size. Native Termux ncurses cannot directly change the host app's font. WaffleHouse-Termux therefore preserves the same goal—fit the configured grid—by requesting the exact outer terminal geometry, tracking the real cell grid through `TIOCGWINSZ`, resizing ncurses, and advertising the largest visible grid via NAWS when the requested geometry physically cannot fit. Live size changes trigger fresh NAWS updates without reconnecting.
