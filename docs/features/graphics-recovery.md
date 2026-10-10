# Graphics recovery

Restart Apex refreshes the mod's image-effect resources without discarding settings or closing the game. It is a recovery action for graphics resources, not an uninstall operation.

## Status

| | |
|---|---|
| Availability | 2.12.0 |
| Menu | Settings > Compatibility > Recovery |
| Configuration | No persistent recovery setting |
| Source | `apex_gui.cpp`, `framework/d3d9_bootstrap.cpp`, image-effect restart callbacks |

## The problem

Image effects hold textures, shader objects and runtime state for the current graphics device. Recreating those resources can recover an interrupted effect without resetting the player's adjustments.

## How Apex Radiance solves it

The button queues one request. The render thread processes it at Present and releases the effect resources for Color, AO, Depth Blur and Edge Smoothing. Enabled effects recreate what they need on their next pass. Exposure and autofocus may briefly readapt. The button shows a pending state until the request is consumed.

## Compatibility and interactions

Settings, presets and LUT files stay intact. The action does not restart the game's device, unload the ASI, remove hooks, reset lighting patches or clear the compiled shader bytecode cache. Successful ASI attachment retains the module until the process exits, keeping live hooks and worker code mapped. Close the game before replacing or removing the ASI.

## Limitations

Restart does not clean Windows, the registry, driver caches or other programs. It does not re-enable an effect disabled by an exception guard. A queued request needs a subsequent Present callback. It is not a guarantee of resolving every visual issue.

## Technical reference

`RequestEffectsRestart` uses an atomic request consumed by the graphics thread. Effect callbacks own their resource teardown. AO and Depth Blur reject incomplete state capture and release partially acquired references. Their saved-state scope restores captured device state on early exits; Depth Blur additionally scopes its backbuffer reference and reentry flag.

## See also

- [Validation](../validation/graphics-recovery.md)
- [History](../history/graphics-recovery.md)
- [Architecture](../architecture.md)
