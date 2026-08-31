# AIM/OSCAR Debug & Login Audit — WaffleHouse-Client 5.0r17

Each AIM/OSCAR account has an **OSCAR debug / audit** setting:

- **Off** — normal operation.
- **Login Audit** — traces the OSCAR login/bootstrap path without dumping secrets.
- **Full Wire Trace** — adds every FLAP/SNAC header and payload to the audit stream; secret-bearing fields are redacted.

## Login Audit output

Login Audit records the auth server target, FLAP greeting/version, BUCP challenge request/response, login request metadata, decoded login-response TLVs, authorization error codes and error URLs, BOS redirect, BOS/service-cookie length (not the cookie), HOST_ONLINE families, family-version negotiation, rate negotiation, and CLIENT_ONLINE completion.

Legacy OSCAR authentication failures delivered on **FLAP channel 4 / SIGNOFF** are decoded when they contain the standard error TLVs. This is particularly useful when auditing login failures against AIM/OSCAR servers other than WaffleHouse.

## Persistent audit log and GUI viewer

When **Login Audit** or **Full Wire Trace** is enabled, the same redacted diagnostic stream shown in Activity is also appended to a per-account log file in the platform application-data directory under `logs/`.

On Linux/FreeBSD this normally resolves beneath `~/.local/share/WaffleHouse-Client/logs/`; the exact path is shown at the top of the viewer. Account/server names are sanitized before being used in the filename.

Open the log from either:

- **Tools → View OSCAR Audit Log…**; or
- right-click an AIM/OSCAR account → **View OSCAR Audit Log…**.

If multiple AIM accounts exist, the Tools command asks which account to display. The internal viewer shows the last 512 KiB for large logs. Audit files rotate at 5 MiB, preserving one `.old` generation. Logging remains disabled when OSCAR debug mode is **Off**.

## Secret handling

Debug output intentionally never prints:

- the account password;
- the MD5 password hash used in BUCP login;
- BUCP challenge material;
- BOS/service authentication cookies;
- old/new account passwords used by the OSCAR Admin family.

Those values appear only as `<redacted:N bytes>`.

## Native OSCAR idle

WaffleHouse-Client's Idle control uses the native OSCAR Generic Service command **OSERVICE__IDLE_NOTIFICATION**, SNAC `0x0001/0x0011`. The payload is a big-endian 32-bit count of seconds since user activity. A non-zero value advertises Idle; `0` advertises Active/Back.

Automatic idle/away only sends the idle notification when crossing a presence threshold, rather than continuously resending it.

## CLI / Termux

In the account editor set:

`OSCAR audit (off/login/full)`

to `off`, `login`, or `full`. Desktop command-line startup also supports:

`--oscar-debug off|login|full`

The older generic `--debug` switch remains a backward-compatible alias for full OSCAR tracing when AIM/OSCAR is selected.
