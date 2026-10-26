# Light through arches and doorways: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/arches.md](../features/night-lighting/arches.md). All entries were made during the
2.14.0 development builds, numbered here as dev.N.

### 2026-10-20: the ambient step at an arch (dev.15)

**Context:** a sharp colour step on the floor along the line of an arch between rooms 3 (a green lamp) and 4.

**Finding:** both floors use the same 256 x 256 light map; the only difference between the two draws is `c4`, the room
ambient: room 3 (0.010 0.037 0.007), room 4 (0.010 0.010 0.007). The floor shader (PS `2BEB2548`) computes
`map.rgb + map.a x c4`, so the colour jumps on the arch's line.

**Outcome:** when the game packs a texel, Apex added alpha x max(0, neighbour - own) x weight, fading over 1.5 m. The
solve's `this` is in `ebx` from `0x006A31E8` and the sample list at `[ebp+8]`, which the texel hook now passes.

### 2026-10-20: neighbours solved again for a new ambient (dev.16)

**Context:** the step remained.

**Finding:** the light map was smooth across the arch (the green fell from 45 to 12 with no step). The log showed the
neighbour's ambient as zero when the texels were packed: a neighbour solved later left the value stale.

**Outcome:** each arch recorded the neighbour's ambient its texels used, and neighbours whose value changed were solved
again after state 0 of a room (`RoomSolveStartHook`). Superseded by dev.20.

### 2026-10-20: the transition moves to the floor shader (dev.20)

**Context:** the baked transition still did not show.

**Finding:** while texels are packed, room 4's ambient is (0.031 0.037 0.030), but the draw uses (0.010 0.010 0.007):
the value during the solve is not the drawn one.

**Outcome:** the transition is computed in the floor shader (`PatchRoomMapSmooth`, arch variant) from the drawn `c4`
and up to 4 segments of "neighbour - own". The baked version was removed. Walls are excluded because the wall atlas's uv
does not give the lot position. A floor whose masked variant does not fit the registers falls back to the combined
variant without the mask, not to one without room-map smoothing.

### 2026-10-20: rooms with the same ambient (dev.21)

**Context:** right from the second story, the step came back after a visit to the ground story.

**Finding:** 301 and 1,464 draws matched an ambient two rooms shared, which cancelled the transition. Rooms without a lamp
of their own can hold exactly the same ambient.

**Outcome:** the draw passes its light-map texture; the story is found from the manager's map set
(`+0x16C` + class x 64, unverified in game) and every room of that story with that ambient gives its arches.

### 2026-10-20: the sill at floor level (dev.22)

**Context:** with the camera on the arch's story, a step remained in the light map itself.

**Finding:** at lighting class 2 the arch's sill (height 0, widened by the map's anti-aliasing) took a soft rim that
darkened room 4's floor just past the arch by 25% (0.559 offline). Specular was ruled out: both sides use the same
constants and textures.

**Outcome:** a sill of 2 cm or less (`kFloorSill`) has no soft rim, in `GridEdgeSpot` and `GridWallHitHard`.

### 2026-10-20: steady light through openings (dev.23)

**Context:** the arch looked right for a few seconds and then wrong again; the stair's colour changed by itself.

**Finding:** with no lamp change, every room of the lots was solved again every one to two seconds (504 invalidations in
13 s). The cause was the mesh blocker experiment: triangles of opaque instanced meshes collected from the screen, with a
budget by on-screen coverage and a hard test, in light through openings and between stories. The set followed the
camera, and each of its 54 publications relit every room near an opening.

**Outcome:** indoor light through openings and between stories no longer tests that set, and its publications relight
nothing (`steady_indoor_light`). The stair treads' hard shadow under a stair is lost; it depended on the camera.

### 2026-10-20: a lamp switched off updates the story below (dev.24)

**Context:** after a sconce over the well was switched off, its green light stayed on the story below.

**Finding:** the periodic relight removed in dev.23 had been resending other stories' rooms after each lamp edit by
accident; without it, an older gap in an edit's sends showed.

**Outcome:** after each lamp edit, once the lamps have been still for 400 ms, the rooms near openings on every lot are
sent again once.

### 2026-10-20: the normalisation at the arch (dev.25)

**Context:** right on entering, wrong afterwards.

**Finding:** across the arch's line the last texel of room 4 was 19 to 22 and the first of room 3 31 to 33. The ratio,
1.5, is exactly that of the two rooms' normalisations (3: 1.500, 4: 1.000). The import is scaled by source over
receiver (`InnerNormScaleAt`), and the receiver's value is set only at the end of its solve (`0x006A0230`), so samples
read the previous one.

**Outcome:** samples use the value the room's last solve ended with; a solve ending with another value resends the room,
at most 3 times in 10 s (`inner_norm_settle`).

### 2026-10-20: the cone's edge (dev.26)

**Context:** the cone of light through the arch ended in steps.

**Finding:** offline (a two-cell arch, a lamp 3 m away at 2.18 m, class 2) the step between neighbouring texels was 0.81.
A lamp of 0.45 m still gave 0.81 at 1 m from the arch. In a real house that edge is soft because of bounce light.

**Outcome:** the cone's edge is spread over 0.5 m at the receiving point (step 0.37). Changed in dev.45.

### 2026-10-20: the arch after a floor switch (dev.29)

**Context:** the step came back when the camera went from story 2 to 1; at that moment the game invalidated rooms 3 and 8
(caller `0x006C7451`).

**Finding:** the seam was a sharp straight line across the floorboards: two floor draws with different ambients. The
room match used exact float equality and gave up when the story was unknown (45,819 draws).

**Outcome:** `c4` matches within 2% plus 1e-4 per channel (`ArchAmbient::Same`), and without a known story every room
with that ambient gives its arches. The F8 snapshot lists the last unmatched draws near a room with an arch.

### 2026-10-20: the opening's shape (dev.45)

**Context:** the light through doors and windows looked less like the opening's shape than before.

**Finding:** 0.5 m blurred the arch and window shapes; 0.15 m changed nothing (step 0.806, as without spread).

**Outcome:** the spread is 0.3 m: the step falls from 0.806 to 0.577. `ArchConeChecks` requires a cut of at least one
third and an edge of 0.3 m or less.

### 2026-10-22: the stories of the recent room maps

**Context:** commit `823e339`. The arch rows of a floor draw find the draw's story by its room map, and the memo kept only
the last map. A frame's floors alternate between the stories' maps, so the memo often missed, and every miss walked the
rooms with arches through `StoryMapsInclude`.

**Outcome:** the memo keeps the stories of 16 room maps, each answer still checked against the manager's sets; a map is
in one manager's sets only, so a confirmed entry is the manager the walk would find. Not yet checked in game
([validation](../validation/night-lighting-arches.md)).

### Release 2.15.6

Repeated floor draws re-scanned map ownership. The direct slot shortcut now revalidates all identities before
reuse, while room ambients remain live. Room-map constants span a small block containing two unchanged middle rows;
batched reads and writes retain those rows exactly and restore them after each draw. No shader equation changes.
