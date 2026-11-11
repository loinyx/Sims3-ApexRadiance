# Delegate Signature Fix: validation

The feature is described in [features/delegate-signature-fix.md](../features/delegate-signature-fix.md). It must keep
every result of the game's delegate check and never read past a zero-parameter signature.

## Automated tests

`tools/delegate_sig_fix_test` (x86):

```powershell
powershell -File tools/delegate_sig_fix_test/run.ps1 [-Exe <TS3W.exe>]
```

It replays the game's check sequence with the stub: the crash's memory (a 16-byte `instance void()` signature followed
by a dword of 1) must give false without a fault, an `(Object[])` signature true and any other single parameter false.
With `-Exe` it checks that the pattern matches once and that its call leads to `mono_type_get_type`.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-10 | branch `release/3.1.1` | `tools/delegate_sig_fix_test -Exe` on Steam 1.67.2 TS3W.exe (timestamp 0x52DEC247) | Stub: ok. One match; call at 0x00D70FB6 -> 0x00F4AC60 (`mono_type_get_type`) | None |
| 2026-11-10 | before the fix | Two crash reports and a minidump | Same fault both times: `TS3W.exe+0xB4AC64` reading `0x00000007`, EAX = EDI = 1, ESI a zero-parameter signature followed by another signature; simulator thread, no Apex or Direct3D frame on the stack | None |

## In-game test plan

1. Start the game with the fix. **Expected log:** `[DelegateSigFix] On at 0x00D70FB6: ...`.
2. Load a lot several times, through loading screens. **Expected:** no crash at `TS3W.exe+0xB4AC64`. A load that does
   not crash proves little on its own, because the crash depends on Mono's memory layout.

## Confirmed in game

- EA app 1.69.47 (2026-11-11): the log reads `[DelegateSigFix] On`, so the pattern and its call target are found on
  that build.

## Open checks

- The log line in a real session, and loads that crashed before the fix.
