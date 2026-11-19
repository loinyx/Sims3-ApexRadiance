# Real-time GI shader cost

Compiles the shader snippets planned for real-time GI as `ps_3_0` with `fxc` and prints their cost: instruction slots,
instructions run, texture reads, temporaries, constants, samplers and inputs, with a verdict against the `ps_3_0`
limits (512 slots, 32 temporaries). Read-only apart from `results.md`.

```bat
python toolsealtime_gi_shader_costun.py [--fxc <fxc.exe>] [--write]
```

- `lamps.hlsl`: what a patched copy of a game shader takes: one lamp, its cone, its shadow from the atlas of cube
  faces (1 compare, 4 taps), and the lamp index cells with the lamp table read by `texldl`. Its verdict adds the
  snippet to the host sizes of research note 02 section 2.
- `passes.hlsl`: passes of their own: compose of the per-lamp layers, the gather over virtual lights, the sphere trace
  of the wall-plan distance field.
- `run.py`: the variant list, the `fxc` calls and the listing parser. Slots are counted from the listing with the
  documented slot cost of each instruction and checked against `fxc`'s own summary (which counts `lrp` as 1); the run
  fails if the count is below `fxc`'s. "Run" multiplies loop bodies by their `defi` counts. `fxc` is taken from the
  newest installed Windows SDK unless `--fxc` names one.
- [results.md](results.md): the last table written with `--write`.

The snippets are cost prototypes. Their constants and atlas layout are placeholders; only the instruction shape is
meant to match the planned shaders.
