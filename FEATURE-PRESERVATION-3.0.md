# WaffleHouse-Client 3.0 — Feature Preservation Gate

3.0 is a modern interface rebuild, not a protocol rewrite. The rebuild starts from the known-working **WaffleHouse-Client 2.5.4-r6** tree and treats that release as the compatibility baseline.

## What is allowed to change

- `src/mainwindow.cpp`: GUI composition and navigation shell.
- `src/terminalui.cpp`: TUI rendering/chrome only; command/protocol behavior remains covered by inherited tests.
- `src/modernstyle.{h,cpp}`: new shared Qt visual system.
- Version/build/documentation metadata.

## What is pinned to the 2.5.4-r6 baseline

The checksum gate in `tests/core_300_preservation_test.sh` verifies that the protocol and service implementation files remain byte-for-byte identical to the 2.5.4-r6 source baseline. That includes AIM/OSCAR, IRC, Telnet/BBS, ANSI terminal, CPX secure messaging, secure/direct file transfer, SIP backend/controller/core, platform detection, profile handling, and the existing GUI support windows. Launcher/version-branding files such as `src/main.cpp` are intentionally outside this checksum set so release branding can advance without weakening protocol-core verification.

## Functional regression gate

The inherited test suite covers:

- ANSI/BBS terminal behavior and BBS theme visibility.
- build/install lifecycle behavior.
- CLI active sessions and preserved BBS screens.
- AIM `/join` private-room semantics and `/j` alias behavior.
- saved profile persistence.
- CLI shortcut footer/status/input row placement and status context.
- FreeBSD audio compatibility detector behavior.
- GUI 2.5.1 account/menu/SIP workflow contracts.
- OSCAR Away/AFK/Idle presence behavior.
- SIP account error recovery and empty-STUN regression behavior.
- Telnet disconnect/curses repaint behavior.

3.0 adds two more gates:

- `tests/ui_300_modern_shell_test.sh` verifies the modern GUI/TUI structure and 3.0 branding.
- `tests/core_300_preservation_test.sh` verifies the untouched core against the 2.5.4-r6 SHA-256 manifest.

## Validation result in the build workspace

- 2.5.4-r6 baseline before rebuild: **15/15 inherited source regression tests passed**.
- 3.0 after the UI/TUI rebuild: **17/17 total regression gates passed** (historical 3.0 validation).
- 3.0r1 after guided transfer, footer, notification-preservation, and Softphone changes: **21/21 total regression gates passed**, including dedicated unsecured IRC frame-budget checks.
- A full native Qt/PJSIP compile must still be performed on a Linux/FreeBSD builder with the project dependencies present. The current packaging workspace does not have Qt 6 development files installed, so a native link test cannot be honestly claimed here.

This setup means visual experimentation can continue without silently changing the working communications engines underneath it.

## Documented 3.0 startup-output fix

`src/sipcore/SipEngine.cpp` contains one intentional post-baseline behavior fix: PJSIP's process-global log threshold is temporarily silenced only for the `libCreate()` → `libInit()` bootstrap window. This prevents PJSIP/pjlib from writing directly over the ncurses handoff. After `libInit()`, the existing level-5 file logger, `consoleLevel=0`, SIP wire monitor, and diagnostics behavior are unchanged. `tests/sip_cli_bootstrap_log_suppression_test.sh` locks this ordering in place.

## 3.0r1 preservation scope

3.0r1 intentionally changes `src/mainwindow.h`, `src/terminalui.h`, and the Softphone presentation sources to add the guided transfer workflow, themed footer placement, and modern Softphone navigation/dial pad. Those presentation/controller files are covered by `tests/r1_300r1_features_test.sh` instead of the byte-for-byte manifest. The AIM/OSCAR, IRC, Telnet/BBS, ANSI, CPX crypto, direct-transfer, existing file-transfer engine, SIP controller/backend, and PJSIP core files remain pinned to the 2.5.4-r6 checksums. The new `src/filetransport.h` is a transport wrapper around the unchanged file-transfer protocol and provides the explicit unsecured AIM/IRC framing.

## 3.0r2 preservation scope

3.0r2 intentionally changes the AIM/IRC backend surfaces only where needed for peer-version request/response handling, and changes GUI/CLI controller state for automatic OSCAR presence. Those deltas are covered by `tests/r2_presence_version_test.sh`. The preservation manifest continues to pin all remaining protocol/security/file-transfer/Telnet/BBS/SIP/PJSIP implementation files to the established baseline.

The automatic presence controller uses the existing OSCAR Away/Idle/Back methods rather than adding a second presence protocol. Manual Away/AFK/Idle remains authoritative.

The 3.0r2 source gate is **25/25 tests passing** before packaging.
## 3.1 secure-room rebuild scope

This rebuild intentionally changes the GUI/CLI chat controller surfaces, `ChatWindow`, and the CPX capability advertisement to add secure AIM/IRC room messaging. The new `SecureRoomManager` is isolated in `src/secureroom.*` and uses libsodium XChaCha20-Poly1305. Existing private-message CPX key exchange remains the transport used to deliver room keys; the underlying AIM/IRC room protocols themselves are not replaced.

The main GUI intentionally removes Softphone controls from the communications dashboard/Buddy List while retaining the existing Softphone implementation and Tools/tray entry points. The current source gate passes **26/26 tests** including `secure_room_31_test.sh`.

