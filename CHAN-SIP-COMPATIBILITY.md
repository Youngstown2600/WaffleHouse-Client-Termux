# Asterisk SIP server compatibility

WaffleHouse-Client uses **PJSIP 2.17 internally** as its SIP user-agent stack. The remote SIP server does **not** need to use PJSIP.

When adding or editing a SIP account, the **SIP server type** selector now shows three explicit choices:

- **Auto / generic SIP (recommended)** — use for non-Asterisk SIP servers or when you are unsure.
- **Asterisk PJSIP (`chan_pjsip`)** — choose this when the Asterisk server is configured with `pjsip.conf` / `chan_pjsip`.
- **Asterisk legacy SIP (`chan_sip`)** — choose this when the Asterisk server is configured with `sip.conf` / legacy `chan_sip`.

The CLI accepts `auto`, `pjsip` (or `chan_pjsip`), and `chan_sip`. Existing saved values `standard` and `asterisk-chan_sip` remain compatible.

In legacy `chan_sip` mode WaffleHouse disables RFC 5626 SIP-Outbound use for the account, uses legacy-safe Contact/Via rewriting, follows the registrar's Digest realm, and keeps the configured Caller-ID/AOR domain independent from the registrar domain.

This setting changes **interoperability behavior toward the remote PBX**. It does not load Asterisk code or replace WaffleHouse's own PJSIP client stack.
