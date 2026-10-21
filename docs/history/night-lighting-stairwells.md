# Light through stairwells: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/stairwells.md](../features/night-lighting/stairwells.md). All entries were made
during the 2.14.0 development builds, numbered here as dev.N.

### 2026-10-20: lamp light through walls to the stairwell (dev.12)

**Context:** with *Indoor light between floors* on, a red lamp (room 8) and a green lamp (room 3) of the ground story,
hanging 0.8 m under the upper slab, lit seven rooms of the story above. Neither lamp was under the stairwell opening.

**Finding:** the segment of the ray inside the lamp's story passed only the game's soft 2D wall test, which shades a ray
only near the end of a short wall. The walls in the way were 2 m long.

**Outcome:** that segment is also walked on the lamp story's wall grid (real wall heights, doors and windows), as a veto
only. It had no visible effect; see the next entry.

### 2026-10-20: a hard crossing for the veto (dev.13)

**Context:** the dev.12 veto changed nothing in game.

**Finding:** the green lamp's ray crossed the wall between rooms 3 and 4 2.6 cm under its top, where the slab begins.
The grid gives a wall's top a soft rim, so the ray passed in part, and the veto cut only rays with no light left.

**Outcome:** the veto uses a hard crossing (`GridWallHitHard`): a ray that crosses a wall below its top, outside a door,
window or arch, is cut. Rays over a half wall still pass.

### 2026-10-20: a full wall counts up to the floor above (dev.14)

**Context:** a green trace remained on the wall of the bathroom above (room 5 of the second story).

**Finding:** in a developer F7 and F8 capture, only the top row of the wall's samples still passed. That ray crossed
the wall 9 mm under the slab: the wall is drawn to 27.212 and the slab starts at 27.222.

**Outcome:** in the veto, a full wall counts up to the floor above, so any crossing of a full wall by a ray that ends on
that floor is blocked. A half wall keeps its own top.

### 2026-10-20: stair tiles sampled higher (dev.21)

**Context:** a sconce over the well lit the stair's upper steps less than expected.

**Finding:** the game samples each tile of the directional maps 0.5 m over the lower floor, 3 m under the sconce, while
the upper steps stand close to it.

**Outcome:** for a lamp of the story above, the tiles under the upper floor's opening are sampled 1.5 m higher (2.0 m over
the floor, still under the slab). Other lamps and tiles are unchanged.

### 2026-10-20: shared ambient through the opening (dev.30)

**Context:** the floor of the landing beside the stairwell drew (1, 1, 1) while the room below had a lamp.

**Finding:** the landing's ambient was (0.0068 0.0068 0.0068), the unlit-room level of Rooms at Night. Stacked rooms are
merged into an atrium only above 30% overlap, so nothing crossed a stair opening.

**Outcome:** a smaller opening shares 0.5 x (opening quadrants / room quadrants) x the other room's ambient, capped and
never darker (`opening_bounce.h`). In game the landing went from 0.0068 to 0.0083: correct, but too small to see.

### 2026-10-20: a band at the opening's edge (dev.31)

**Context:** the shared ambient was spread over the whole landing.

**Finding:** the lower room's own ambient was only (0.0162 0.0145 0.0121).

**Outcome:** the floor beside the opening took the lower room's ambient in full at the edge, fading over 1.5 m (the
arch transition of [arches](night-lighting-arches.md)); the floor shader took 6 segments instead of 4. Removed in dev.34.

### 2026-10-20: the band removed, direct light checked (dev.33 and dev.34)

**Context:** the third story looked unnatural: a warm strip on the carpet beside the railing, the rest dark.

**Finding:** the band is physically inverted: a lamp below the floor's plane cannot reach the floor's top face. The F8
record gained lamps of other stories up to 20 m and the test that stopped each ray. It showed that direct light through
the opening works: walls that see the lamp through the opening receive it in full, and the game's falloff leaves them
dim because the lamp is 10 to 11 m away and below the floor. A flux estimate (an opening of 20 m^2 seen from 0.8 m
below, spread over about 500 m^2 of surfaces above) gives about +0.0006 of ambient, the size of what the shared ambient
already adds.

**Outcome:** the band is removed (`no_opening_band`). Raising the shared ambient further was rejected: it would invent
light. A second bounce was the physically based addition left.

### 2026-10-20: second bounce on the walls above (dev.35)

**Context:** the walls of the upper story facing the opening stayed dark.

**Finding:** each opening patch is a window onto the floor or wall below; that surface reflects albedo x irradiance / pi.

**Outcome:** `stairwell_bounce` adds radiance x cos x cos x area / r^2 per opening patch to the upper walls. On the
house's geometry the window wall takes 1.7% of the light below and a wall beside the opening 13.5%. In game the walls
received light, but little was visible.

### 2026-10-20: the ceiling bounce and a first adaptation (dev.36)

**Context:** the floor of the third story stayed at 0, 0, 0. The game asked for a normalisation above 1 in 67 solves of
rooms lit only from another story; Apex kept 1.

**Finding:** the floor lies in the opening's plane and sees nothing below.

**Outcome:** the opening's reflected flux goes up along 24 rays in 3 rings and the ceiling returns it to the floor over
two legs of 3 m. Adaptation up to x4 was added; it never acted (next entry).

### 2026-10-20: adaptation by the measured light (dev.37)

**Context:** the upper room stayed at normalisation 1.000 after dev.36.

**Finding:** the game's normalisation at `0x006A0230` takes the brightest of the room's samples and lamps and counts the
lamp below at full strength (1.0), although the slab blocks most of it
([room-light-maps.md](../engine/room-light-maps.md)).

**Outcome:** a room lit only from another story takes 1 / the brightest light its texels received in the last solve, up
to x4. The normalisation settling solves the room again when the value changes.

### 2026-10-20: the well lift and a lower cost (dev.40)

**Context:** in game the window wall went to R 44 to 54 while the floor stayed at R 4, and lamp switches became slow. The
bounce cost 392 ms over 9 room solves, against under 90 ms for every other Apex test.

**Finding:** the rays at 20 and 45 degrees fell into the stair's well and met nothing.

**Outcome:** a ray reaching the lower floor's plane where there is no floor meets the stair 1.5 m above it
(`kWellLift`). The upper floor took albedo x 0.35 x the mean light of the room's walls. Walls more than 2 m from a tile
use one source per tile instead of four quadrants, and the line below is walked in 0.5 m steps. Edits were noticeably
faster afterwards.

### 2026-10-20: slab penumbra and room fill (dev.41)

**Context:** a lamp on the third story, 1.5 m from the opening, drew a patch with a hard straight edge on the story below
and left the rest of the room black.

**Finding:** the edge was the projection of the opening's east edge from the lamp.

**Outcome:** the light through the opening takes the share of a 0.25 m lamp disc the slab's edge leaves open (about
0.7 m of transition). A room lit only through the opening takes albedo x the mean light of its floor and wall texels in
the last solve (`room_fill`), replacing the walls-to-floor term.

### 2026-10-20: switching off without delay, doors and windows excluded (dev.42)

**Context:** the ground story took a moment to go dark after a lamp was switched off, and the shape of the light
through doors and windows looked wider than before.

**Finding:** room fill used the mean of the previous solve and nothing asked for another. Adaptation and fill also
applied to rooms lit only through a door or window, spreading their light over the whole room.

**Outcome:** fill and adaptation apply only to rooms whose lamps all come from another story (`CrossStoryOnly`). A room
whose measured mean differs from the one it used by more than 25% is solved again, at most twice in 10 s.

### 2026-10-20: partial adaptation (dev.45)

**Context:** rooms 1, 6, 7 and 8 of the ground story reached a normalisation of 4.000 from a lamp on the second story
shining down through the opening.

**Finding:** the eye compensates only part of a dark room and adapts to the whole view, not room by room.

**Outcome:** the normalisation is sqrt(1 / the brightest measured texel), between the game's value capped at 1 and x2
(`StairwellBounce::BorrowedAdapt`). A room with 5% of the light becomes twice as bright, not four times. The upper walls
of light paper reached about R 25 while a dark wooden floor stayed at R 0 to 8, which is consistent with reflected
light only.
