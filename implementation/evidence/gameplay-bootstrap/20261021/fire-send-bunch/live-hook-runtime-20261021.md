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
