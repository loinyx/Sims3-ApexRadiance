# Steam Deck and Proton: history

[Feature page](../features/steam-deck.md)

## 2026-10-20: first support

- A player on the Steam Deck reported Ambient Occlusion not working. The code shows no vendor or path check that would
  turn it off; the likely causes, in order, are Wine's builtin `d3dcompiler_47` refusing a `ps_3_0` shader, the shared
  depth not becoming ready, and the game's own multisampling. None of them logged a reason a player could see.
- Research on DXVK's sources: its app profile for `TS3W.exe` reports a GeForce GTX 580 on any card, so adapter strings
  cannot identify the Deck; `D3DLOCK_DONOTWAIT` is ignored and a lock after `GetRenderTargetData` synchronises CPU and
  GPU; `GetData` with `D3DGETDATA_FLUSH` on a pending query forces a submission. Proton makes executables large address
  aware by default.
- An audit of Apex's readbacks and queries under those rules found them asynchronous or on demand in released builds,
  except the Instant (GPU) lamps' floor mask buffer, locked in place while the previous draws may still read it; it now
  alternates between two buffers.
- A shader cache file shipped beside the ASI was rejected in favour of a resource inside it: one file to install.
