2.5.4-r5 AIM /join private-room compatibility
--------------------------------------
- AIM/OSCAR /join and /j route through private-room exchange semantics; IRC stays public-channel.
- Registers `/j` as a true alias of `/join` in dispatch, completion, and help.
- Uses the existing AIM/OSCAR room and IRC channel join implementation without duplicate join logic.

2.5.4-r3 build lifecycle/status-bar update
----------------------------------------------
- Folds client-up lifecycle behavior into build.sh with build-before-replace upgrades and config-preserving uninstall.
- Simplifies the CLI status bar to the active numbered screen plus state; the global Status screen remains IDLE.

2.5.4-r2 saved-profile persistence hotfix
-------------------------------------------
- Restored QSettings profiles remain persistent in memory without rewriting the settings array during the read pass.
- `/connections` and `/accounts` again list restored saved profiles; `/active` remains live-session-only.

2.5.4 CLI/BBS lifecycle update
------------------------------
- Adds /active and /accounts, ephemeral /telnet quick-connect, preserved disconnected BBS screens, and masked password input.

2.5.3 BBS/theme update
----------------------
- Corrects curses bright-black/gray BBS rendering and raw CLI input visibility.
- Adds complete S.I.P.H.E.R. GUI/CLI theme family while preserving WaffleHouse themes.

# WaffleHouse-Client 2.5.1 — Fully Integrated Multi-Account Softphone

## Merge parents

- WaffleHouse-Client 2.3 Alpha: retained as the application/UI/protocol base.
- S.I.P.H.E.R. r14 FreeBSD-audio-compat branch, carrying forward r12 live headset switching and r13 runtime dial-prefix changes: used as the current SIP/PJSUA2 compatibility source.


## 2.5.1 integration model

SIP accounts are normal WaffleHouse saved connections. Multiple accounts can coexist and register independently through one shared PJSIP endpoint. GUI `Connection > Add...` and CLI `/add` both accept SIP/VoIP. The Buddy List exposes SIP account selection, runtime PBX prefix, quick dialing, registration controls, local SIP contacts, and active-call children. The Softphone launcher lives under Tools, while the Softphone Profile tab edits the same saved WaffleHouse SIP connection.

## 2.5.1 GUI workflow

The GUI Accounts menu is connection-centric: each saved AIM/OSCAR, IRC, SIP, or Telnet profile gets its own submenu. AIM/IRC submenus expose one combined IM/Chatroom dialog and a dedicated buddy manager; SIP exposes a local contact manager for reusable dial targets. Tools owns Open Softphone, Show Connections Window, Change AIM Password, and Secure Identity Fingerprint.

## Preserved WaffleHouse features

No existing WaffleHouse protocol capability was intentionally removed. AIM/OSCAR, IRC, Telnet/MUD/BBS, CPX3 secure DMs, fingerprint trust, encrypted/resumable file transfer, saved connections, themes, tray behavior, GUI conversations, and the ncurses frontend remain in place.

## Added softphone features

The GUI has a dedicated Softphone window with these tabs:

1. Main
2. Active Call
3. SIP Log
4. SIP Ladder
5. Profile
6. Activity

The CLI exposes the same functional areas as native WaffleHouse slash commands and buffers.

### CLI commands

- `/phone`
- `/phoneprofile`
- `/phoneconfig`
- `/phonestart`, `/phonestop`
- `/prefix [VALUE|off]`
- `/dial DEST [CID]`
- `/dialraw DEST [CID]`
- `/dialpreview DEST`
- `/calls`
- `/answer ID`, `/reject ID`, `/hangup ID`
- `/hold ID`, `/callresume ID`
- `/mute ID`, `/unmute ID`
- `/dtmf ID DIGITS`
- `/siplog [ID]`
- `/ladder ID`
- `/phoneactivity`
- `/audio-devices`
- `/audio-use CAPTURE-ID PLAYBACK-ID`
- `/audio-auto on|off`

## Explicitly excluded from the S.I.P.H.E.R. merge

- PBX/SIP audit and security-testing suite
- extension/service enumeration tools
- queue/call-blast/multi-call launch features
- audio-file queue injection
- SIP PCAP controls
- RTP PCAP controls
- combined call PCAP controls
- Wireshark-launch capture workflow

The low-level S.I.P.H.E.R. call engine remains internally namespaced `trunkmonkey` for source compatibility, but WaffleHouse is the user-facing application and SIP User-Agent.

## r12-r14 softphone compatibility behavior

The r12 live audio-route polling is retained. On Linux it follows PipeWire/PulseAudio route changes when available; on FreeBSD it follows PulseAudio where present and otherwise watches native OSS/snd_hda route state. A route change reopens the PJSIP sound devices and reattaches the foreground call without intentionally dropping the SIP dialog.

r13's dial-prefix model is adapted to WaffleHouse multi-account SIP: each saved SIP account supplies a startup default prefix, while its live PJSIP account has an independently changeable runtime PBX prefix. GUI Buddy List, Softphone Main, and CLI `/prefix` all edit that runtime value. `/dial` applies it to plain dial strings, `/dialraw` bypasses it, and `/dialpreview` shows the exact Request-URI.

r14's FreeBSD builder compatibility layer is also integrated. During a normal FreeBSD build, `build.sh` inspects `snd_hda` topology and only considers automatic retasking when the detector sees exactly one fixed Speaker, one jack Headphones output, and no analog Line-out. It tests a same-association headphone `seq=15` layout, validates playback/capture PCM availability, rolls back failed tests immediately, and persists only a validated headphone hint to `/boot/device.hints` after making a timestamped backup. Existing custom HDA hints cause a skip; more complex hardware is never retasked automatically. WaffleHouse recognizes both its own r14-managed marker and a previously generated S.I.P.H.E.R. r14 marker so the builders do not compete. `--audio-diagnose` is read-only and `--no-audio-fix` disables automatic repair.

## Build

`./build.sh` now installs/checks the existing WaffleHouse dependencies plus SIP audio/crypto prerequisites, then bootstraps a managed PJSIP 2.17 build under `~/.local/wafflehouse-pjsip` when needed.

Use `./build.sh --pjsip` to force a clean managed PJSIP rebuild.
