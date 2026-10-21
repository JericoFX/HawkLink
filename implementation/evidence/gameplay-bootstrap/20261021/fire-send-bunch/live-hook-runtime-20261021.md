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

The next useful firing observation requires a successful local Steam-authenticated gameplay session. The current capture hook is already installed; no duplicate `SendRawBunch`/`SendBunch` hook is needed. If a successful session still produces no bunch when firing, the next probe must target the earlier UE3 RPC sender gates, not repeat the same transport hook.

## Earlier RPC-gate probe (2026-10-21 20:06 -03:00)

The bridge now has a second, local-only trace for the client-side callsite at RVA `0x690566` into the native RPC sender at `0x687c10`. It wraps the sender's existing calls at `0x687c7a` (class cache), `0x687c91` (function cache), and `0x687cb2` (actor-to-channel lookup), verifies the original five instruction bytes at all four sites, and uses the existing relative-call relay without changing arguments or native return values. The trace records cache-hit status, the cached field ID at function-cache-entry `+8`, actor-channel lookup status, channel index, and opening packet ID. It stores no credentials or packet payload. A `gates_passed` row means those observed checks passed; it does not prove a bunch was serialized or transmitted.

The production bridge and fixture compiled in the isolated build tree `build/windows-msvc-rpc-gate-validation-20261021` with MSVC 19.44.35228.0. The focused `client_bridge_send_bunch_trace` and `win_callsite_hook` tests both passed (2/2). Build log: `build/windows-msvc-rpc-gate-validation-20261021/hawklink-rpc-gate-validation-20261021.log`. Production bridge SHA-256: `A63CD630224B684D67DC71E9307187D31F5BB3A79549C1DDFD020FA426C4B015`.

**CONFIRMADO:** the diagnostic implementation compiles, its fixture cases cover cache misses, actor-channel miss, opening ID `-1`, passed gates, unreadable memory, and output-capacity failure, and the focused hook tests pass.
**NO VERIFICADO:** the new bridge has not been loaded into Hawken and there is no authenticated firing trace. The running Hawken process still has the previously observed `DINPUT8.dll` hash, while its latest readable `Launch.log` ends in `LOGIN FAILED ErrorCode = OSCS_NoSteamConnection`. The next runtime observation therefore requires a successful local Steam-authenticated session; it must use this new bridge build to produce `rpc-gates-*.csv` and a corresponding `send-bunch-*.csv`.
