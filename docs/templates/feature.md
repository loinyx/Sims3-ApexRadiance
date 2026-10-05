# Feature name

One paragraph for players: what the feature does and what changes on screen. No implementation detail here.

## Status

| | |
|---|---|
| Availability | Released in X.Y.Z / In development (PR #N) / Developer mode |
| Default | On / Off |
| Menu | Page > Card |
| Configuration | `[patches.PatchName]` in `ApexRadiance.toml` |
| Source | [`path/file.cpp`](../../path/file.cpp) |

## The problem

What the unmodified game does, and why it looks or performs worse. Reference the engine page that documents it.

## How Apex Radiance solves it

The approach in two or three sentences, then the processing steps:

1. Step one.
2. Step two.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| | | | | | |

## Compatibility and interactions

- Other Apex Radiance features that share resources or order.
- Game options and other mods.
- Hardware or driver requirements.

## Limitations

- Known gaps a player could notice.
- Conditions in which the feature turns itself off or falls back to the game's rendering.

## Technical reference

Hooks, shaders, registers, resources, addresses and measured cost. Keep it short; move long material to an engine page.

## Rejected approaches

- Approach: one-line reason. Details in [history](../history/feature.md).

## See also

- [Validation](../validation/feature.md)
- [History](../history/feature.md)
