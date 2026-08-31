# WaffleHouse-Client 3.1 Secure Room Chat

Secure Room mode adds encrypted WaffleHouse-to-WaffleHouse conversation inside an ordinary IRC channel or AIM chatroom without changing the underlying IRC/AIM server.

## How it works

1. The room initiator creates a random 256-bit XChaCha20-Poly1305 room key.
2. That key is never posted to the public room. WaffleHouse delivers it to each included peer through the existing CPX encrypted private-message session.
3. Room messages are encrypted locally and sent to IRC/AIM as `[[CPXROOM1:...]]` authenticated ciphertext.
4. WaffleHouse peers with the matching key decrypt and display the text locally with a `[secure-room]` marker.
5. Ordinary unencrypted messages received while Secure Room mode is active are marked `[plaintext]`.
6. The client that created the room key rotates the key when room membership changes and redistributes it to current secure peers.

The local WaffleHouse profile ID is intentionally not included in the AEAD authenticated wire data. Different WaffleHouse installations have different local profile IDs, so only stable shared values (normalized room name and key ID) are authenticated on the wire.

## GUI

Open an AIM chatroom or IRC channel. Establish a normal CPX Secure private session with each WaffleHouse peer who should receive the room key. In the room window choose **Security -> Start Secure Room** (or type `/secure`).

The room window shows Secure Room status and makes encrypted-vs-plaintext traffic obvious.

## CLI

From an AIM/IRC room buffer:

```text
/secure
/securestatus
/secureoff
```

Use `/secure USER` from a PM or other buffer to establish the one-to-one CPX session used for room-key delivery.

When active, the CLI header displays a `LOCK ROOM <key-id>` indicator (rendered with the lock glyph when supported), decrypted room text is tagged `[secure-room]`, and ordinary room text is tagged `[plaintext]`.

## Two-client verification

For a simple test with two WaffleHouse clients:

1. Join the same IRC channel or AIM chatroom on both clients.
2. Establish a CPX secure PM session between the two users and compare/trust fingerprints if desired.
3. On Client A, start Secure Room mode.
4. Confirm Client A reports that the room key was sent privately to Client B.
5. Send a room message from Client A. A raw/non-WaffleHouse observer should see the `CPXROOM1` frame rather than the plaintext; Client B should display the recovered text as `[secure-room]`.
6. Send a normal room message from a client without the room key. WaffleHouse clients with Secure Room active display it as `[plaintext]`.

## Main window / Softphone layout

WaffleHouse-Client 5.0r13 uses a compact main **Accounts** window without the old navigation rail. SIP accounts remain visible alongside AIM/IRC/Telnet accounts. The full Softphone workspace is available from **Tools -> Open Softphone**, the system tray, and supported `/phone` commands.
