# HawkLink `SendBunch` hook: live verification

Observed 2026-10-21 19:46 -03:00. This was a read-only inspection of the local Hawken process using `ReadProcessMemory`; no game memory or original file was modified.

## Artifact identity

| Artifact | SHA-256 |
|---|---|
| `F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe` | `4A861ECB4AC7ADD9554F8DAF8F98AD2C12B9CB6C1865A83E8BE808BEB46EA8A2` |
| `F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\DINPUT8.dll` | `65882CD722F40679E491AAB7D98E2566C3171FFB8CD30D7016880327425248C1` |
| `src/client_bridge.cpp` | `64ABEC0CF0F47AA740EAD35D0EDC2C2E8BB2FAA6D28627A0F179FDD73DEFB82A` |
| `src/win_callsite_hook.cpp` | `9694537D23A483A6D896C8B3E7ADACAD5027424E1F6C3D8C34D494899ABEE55E` |
| `send-bunch-disassembly.txt` | `F5C007E00FD0DF58C081868CBBD88DC1DF8E9ED0444CF8ADF232E6BC91233095` |
| `weapon-fire-findings.json` | `E9CEB5EE318CF9E8606EB7245F5747A39D5CEA2D06C520588DB6883FA7D2337E` |

## Runtime observation

The running client was `HawkenGame-Win64-Shipping`, PID `756052`, started at `2026-10-21 18:31:47`. Its game-image base was `0x7FF7D6C40000`.

Static disassembly locates the `UChannel::SendBunch` call to `UNetConnection::SendRawBunch` at RVA `0x9F2B77`, with original bytes `E8 94 08 00 00`, targeting RVA `0x9F3410`. In the live process, the same call site contained `E8 84 D4 62 01` and decoded to relay `0x7FF7D8C60000`, not the original target `0x7FF7D7633410`. The relay bytes were `45 89 C1 49 89 D0 48 89 CA 48 89 F9 48 B8 A0 52 94 78 FD 7F 00 00 FF E0`; the embedded handler address `0x7FFD789452A0` lies inside the loaded project `DINPUT8.dll` at base `0x7FFD78940000`.

**CONFIRMADO:** the DLL hook recommended by Gemini is installed in this live process. It patches the call site inside `UChannel::SendBunch` and redirects it through the HawkLink handler before calling the original `UNetConnection::SendRawBunch`. This is stronger evidence than merely seeing `DINPUT8.dll` loaded.

The handler in `src/client_bridge.cpp:280-445` records channel indexes `>= 2`, send flags, bunch flags, bit count, and at most the first 64 payload bytes, with a 4096-record cap. The trace is `C:\Users\Jerico\AppData\Local\HawkLink\send-bunch-756052-134370919080671256.csv`, SHA-256 `3D507E83AFA5413BD2177EBF24D9F88E8CFC49C771065311D14FEEB453A7765B`, length 123 bytes; it contains only the CSV header.

## What this does and does not establish

- **CONFIRMADO:** the transport-call hook is active, and no channel-2-or-higher bunch was recorded in this trace as of the observation.
- **CONFIRMADO:** `C:\Users\Jerico\Documents\My Games\Hawken\HawkenGame\Logs\Launch.log` (SHA-256 `9A3C9B4C0E997827EC33F7FAC87AE66D3E41D3B4B11837FFE310E61EF5311D9A`) records `LOGIN FAILED ErrorCode = OSCS_NoSteamConnection` at line 189. The Core log snapshot (SHA-256 `FD803F36209276D90159A264FFA1AF2B3223FA0ED1A9F59F952763617F5433AE`) records Steam `context_lost` and `authenticated_datagrams=0`.
- **NO VERIFICADO:** whether this client emitted `ServerStartFire`, whether its runtime field ID equals the Core's candidate ID 26, or whether UE3 stopped earlier at the owner/controller, function-cache, actor-channel, or channel-open-packet checks. The transport hook cannot observe a sender that returns before it reaches `UChannel::SendBunch`.
- **NO VERIFICADO:** the Core's static candidate field mapping. `weapon-fire-findings.json` explicitly says the static UPK/cache metadata does not prove the live per-connection wire field ID.

The next useful firing observation requires a local session that loads the new bridge build. Older local sessions and packet captures exist, but they predate this RPC-gate trace; their UDP payloads do not identify the UE3 fire RPC. No duplicate `SendRawBunch`/`SendBunch` hook is needed.

## Earlier RPC-gate probe (2026-10-21 20:06 -03:00)

The bridge now has a second, local-only trace for the client-side callsite at RVA `0x690566` into the native RPC sender at `0x687c10`. It wraps the sender's existing calls at `0x687c7a` (class cache), `0x687c91` (function cache), and `0x687cb2` (actor-to-channel lookup), verifies the original five instruction bytes at all four sites, and uses the existing relative-call relay without changing arguments or native return values. The trace records cache-hit status, the cached field ID at function-cache-entry `+8`, actor-channel lookup status, channel index, and opening packet ID. It stores no credentials or packet payload. A `gates_passed` row means those observed checks passed; it does not prove a bunch was serialized or transmitted.

The production bridge and fixture compiled in the isolated build tree `build/windows-msvc-rpc-gate-validation-20261021` with MSVC 19.44.35228.0. The focused `client_bridge_send_bunch_trace` and `win_callsite_hook` tests both passed (2/2). Build log: `build/windows-msvc-rpc-gate-validation-20261021/hawklink-rpc-gate-validation-20261021.log`. Production bridge SHA-256: `A63CD630224B684D67DC71E9307187D31F5BB3A79549C1DDFD020FA426C4B015`.

**CONFIRMADO:** the diagnostic implementation compiles, its fixture cases cover cache misses, actor-channel miss, opening ID `-1`, passed gates, unreadable memory, and output-capacity failure, and the focused hook tests pass.
**NO VERIFICADO:** the new bridge has not been loaded into Hawken, so no in-process RPC-gate or plaintext bunch trace exists. The currently running Hawken process still has the previously observed `DINPUT8.dll` hash, and its latest readable `Launch.log` ends in `LOGIN FAILED ErrorCode = OSCS_NoSteamConnection`; earlier authenticated UDP sessions are summarized below but predate this probe. The next runtime observation must use the new bridge in a local Steam-authenticated session to produce `rpc-gates-*.csv` and a corresponding `send-bunch-*.csv`.

## Retrospective packet and Core-log review (2026-10-21)

TShark 4.6.9 read `implementation/evidence/local-steam-routing-20261021-031341/gunfire-observation-20261021-001900.pcapng` (SHA-256 `A529C8ADEF4A165977D20EF64F36C4826BB7EB2749928976E4625A2CBE5A7D1E`). It contains 5,462 loopback UDP frames over 44.765 seconds between client port 64953 and Core port 7777: 1,821 client-to-Core datagrams (1,797 with UDP length 100, 18 length 95, 3 length 102, 3 length 99) and 3,641 Core-to-client datagrams (length 60). TShark identifies only UDP; the application payload remains opaque in this capture. This is not enough to identify a fire message or claim encryption.

The matching `implementation/evidence/local-steam-routing-20261021-031341/server.diagnostics.log` (SHA-256 `8BA1B44C800BD8DC31AD8A4179B0F80DC25B94E7243243B192CF0D9E717B6B1F`) records 9,787 authenticated datagrams and reaches `match_start_requested=1`, `gri_match_begun_sent=1`, `client_restart_sent=1`, inventory profile/openings/links/`GivenTo` sent or acknowledged, and `acknowledged_pawn_revision=1`. Across the session it records `inventory_current_weapon_received=0` and contains zero `udp_fire_request` records. These are confirmed server-side observations, not proof of what the local client displayed. The separate field `gameplay=0` is hard-coded by the current `src/server_main.cpp` (SHA-256 `6F79AE920ECE319313CA8E9463C66BC0A6A94606C0EA46F46A1DC4A80A59777B`, line 487); it is not a runtime gameplay result.

The earlier `implementation/evidence/local-steam-routing-20261021-024001/result.json` (SHA-256 `AE2FAB4C56B28EB264AFBFD7090BF77A98BB5478118777656C1C0092894E1168`) reports `authenticationVerified=true`; its `fire-input-test.pcapng` (SHA-256 `3F6B57F1828DF52ECA1877717D855BECAF64F901EEABEDBCCD3F3B4F631B1C84`) also contains 4,950 loopback UDP frames but does not expose a decoded fire RPC. Together the captures confirm that transport datagrams reached the local Core, while neither the packet dissector nor the Core's reliable actor-bunch parser observed a decoded firing request. They do not distinguish a client-side UE3 RPC gate from a channel/field mismatch or an incomplete Core decoder. The new pre-serialization gate trace is aimed at that distinction.

**CONFIRMADO:** the historical gunfire-labelled run had client-to-Core UDP traffic, Core match-start milestones, no logged `udp_fire_request`, and no observed current-weapon selection RPC. The loopback PCAP itself does not reveal a readable fire opcode.
**PROBABLE, AÚN NO VERIFICADO:** the shot loss occurs before or at reliable actor-RPC decoding; the specific gate, field, and owning channel remain unknown until `rpc-gates-*.csv` and the corresponding plaintext `send-bunch-*.csv` are produced by the new bridge in a local session.
