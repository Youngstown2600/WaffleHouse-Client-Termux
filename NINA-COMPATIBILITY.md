# WaffleHouse-Client 5.1 — NINA Network compatibility

WaffleHouse-Client 5.1 imports the **known-good NinaIM 0.2.0 / 0.3.0 OSCAR login compatibility path** that successfully signed into NINA. The reference behavior was derived from the user-supplied NINAPatcher 1.3.2 binary: NINAPatcher redirects a stock AIM client to NINA infrastructure instead of replacing the AIM/OSCAR engine.

## NINA endpoints / behavior

- Login/BOS entry: `login.oscar.nina.chat:5190`
- ARS/rendezvous reference: `ars.oscar.nina.chat:5190`
- BUCP authentication retained as native OSCAR
- BOSS Stage-2 service-cookie FLAP SIGNON carries `TLV 0x0006` (cookie) and `TLV 0x004A` (multi-connection = 1)
- Service bootstrap accepts greeting-first and client-signon-first servers
- BUCP challenge parser accepts both 16-bit and 32-bit challenge-length forms
- `CLIENT_VERSIONS` advertises only OSCAR families WaffleHouse actually implements
- NINA/stock-AIM family versions: OSERVICE v3, FEEDBAG/SSI v2, other implemented families v1
- `HOST_VERSIONS` and rate replies are matched by family/subtype rather than requiring request-ID echo
- `CLIENT_ONLINE` uses classic AIM/libfaim-style tool metadata
- Secondary service redirects (ChatNav, Chat, Admin, BART, etc.) follow the server-advertised endpoint; the optional BOS override does not rewrite them

## Selecting NINA

GUI: Add/Edit Connection → AIM / OSCAR → **AIM network → NINA Network (NINAPatcher compatibility)**.

CLI: set `AIM network` to `nina` in the connection editor.

Command line GUI prefill: `--protocol aim --oscar-network nina`. If the server hostname ends in `.nina.chat`, WaffleHouse automatically enables the same compatibility behavior even when the saved network profile is `auto`.

## Platform propagation

The NINA behavior lives in `src/oscarbackend.cpp`, which is shared by both GUI and CLI. There are no separate protocol forks per operating system. Linux/Unix, FreeBSD, macOS, Windows, and Termux therefore use the same authentication/BOS/service-negotiation implementation.
