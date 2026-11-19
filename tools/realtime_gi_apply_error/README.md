# Real-time GI: error of the multiplicative apply

The detail layers of real-time GI (contact, capsule and rig shadows, bounce detail) scale the finished pixel:
`colour *= 1 + s`, `s` being the relative change of the light at the pixel. This tool states how wrong that is behind
the two curves of the game that sit between light and colour, in 8-bit levels of the finished pixel. Standard library
only; `--frames` needs Pillow. Read-only apart from `results.md`.

```bat
python tools/realtime_gi_apply_error/run.py [--selftest] [--write] [--frames <image or folder> [--stride 7]]
```

- Model: the room-lit floor shaders end with `c = y tanh(K m) / m` on `y = light x albedo`, `m = mean(y)`
  ([docs/engine/shaders.md](../../docs/engine/shaders.md), `features/opening_lamp_balance.h`); the game's room solve
  writes `x tanh(m / 2) / m` into the maps it solves (`features/gpu_lamp_law.h`). Walls and objects have no shader curve.
- Because both curves scale the colour by a function of its mean, the true pixel after the light change follows from
  the pixel alone: `c' = c (1 + s) tanh(atanh(u) q / u) / q`, `u = mean(c)`, `q = mean(c (1 + s))`. `K` cancels, so
  the open question about the base of the shader's `exp` does not touch the result.
- `--frames` counts the colours of real frames and takes every pixel as a floor pixel: an upper bound for that
  picture, since every other pixel has no error from the shader curve.
- [results.md](results.md): the tables of the last `--write`.

Not modelled: bloom and other post effects after the apply, fog, and the pack's cut of a map at 1.
