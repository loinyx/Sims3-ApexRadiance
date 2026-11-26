---
name: apex-launcher-ds
description: Design, build or review any Apex Launcher screen, dialog, card, panel, menu or text (installer/ApexSetup). Use before drawing options, before writing UI code, and for every polish pass, so new work matches the launcher's design system.
---

# Apex Launcher design system

The launcher has one look. Every new piece is made of the parts that already exist; nothing is drawn by hand when a part
does it. Read `docs/launcher-design.md` (tokens, parts, rows, side pane, dialogs, words) and the `Design/` folder it points
to before designing. This skill is the checklist on top of it: the rules that keep coming back.

## 1. Before designing

- Find the closest existing screen or part and build from it. A new shape is allowed only when no part fits, and then it
  becomes a part in `Design/Parts.cs` (or a shared helper next to `OptionDot`) and is added to `docs/launcher-design.md`.
- Read the request: "improve this one" means the current design, polished; "rethink" means a new direction, still inside
  the system. Never a reskin when a redesign is asked, never a new direction when a polish is asked.
- Show options first as an artifact: real launcher screens at real size, in the launcher's own tokens, dark and light,
  with real content. Give the number of options asked for. Wait for the pick, then build exactly that, with nothing
  invented on top.
- An artifact option is a proposal, not a spec of new tokens: if it uses a radius, size, glass or font the launcher does
  not have, translate it to the system's own before building (see section 6, the Home health card).

## 2. Surfaces and colour

- Neutral black and grey surfaces; dark is the default and is never tinted with the accent. Light must look as good.
- Colour only where something stands out: the main button, a switch that is on, the chosen option, the selected place,
  links, warnings and errors. Icons, tiles and panels stay neutral; a tile takes a hue only for a warning or error.
- Gradients, glows and tints are extremely soft. `SoftGlow` for one pool of light behind a crystal or logo, `Aurora` for
  install backgrounds; nothing else glows.
- Every colour comes from a brush (`Res("Apex…Brush")`) or `Tone(…)`. On a picture, words are white and the surface is
  frosted glass (`Glass(tint, hue)`, an acrylic); on a plain card the same element uses the theme's brushes. Anything
  that can sit on both builds both (see `HomeActions`, `HealthChip`).
- Borders exist in both themes and follow the corner radius. No double shadows, no stale colours after a theme switch:
  anything built with theme brushes is rebuilt in `ThemeChanged` (the credits list was a bug of this kind).

## 3. Tokens (never a literal)

`Design.Space`, `Design.Radius`, `Design.TextSize`, `Design.Size`, `Design.Motion` in `Design/Tokens.cs`.

- Corners: tag 6, control 8, tile 10, card 12, panel and dialog 16. Nothing else (no 17, 18, 20).
- Controls: 32 small, 36 normal, 40 large/dialog footer; the Home's Play and More are 56 and are the only exception.
- Text: 11 to 40 on the scale only. Page title 28, section 16, row title 14 semibold, detail 12–13, label 12. No giant
  numbers or headings, no numbered sections, titles and inputs never wrap or break.
- Space: page 32, card padding 20 on all four sides, rows 68 high with 20 at both ends, icon to words 16, title to line 4,
  head to body 24, sections 32. Something in a card's corner stands as far from the top as from the side.
- `ApexSetup.Tests/DesignTests.cs` counts text sizes, corners and colours off the scale; those counts only go down.

## 4. Parts to use

| Need | Use |
|---|---|
| A setting or fact in a list | `Row(icon, title, detail, control)`, `Toggle(...)`; consecutive rows `Join` into one card |
| One choice out of a few, with a sentence each | `ChoiceList([(icon, title, text)], chosen, pick)` (install's last step, a save's automatic backups) |
| Big side-by-side options with pictures | `RadioButton` with the `ApexOptionCard` style in a `Border`, lit by `OptionLit`, marked by `OptionDot`/`OptionOn` (renderer cards) |
| Two or more values of one thing | `Design.Segmented` |
| An icon at a row's or card's start | `Design.IconTile.Of(icon)` |
| A button with only an icon | `Parts.Icon` (32), `Parts.IconBoxed` (40, footers), `Parts.OverPicture` (round, on a picture's corner) |
| Back to the previous screen | `Parts.Back` |
| A side panel | `Parts.SidePanel`, `PanelOf`, `FillFoot`; close with `Parts.PanelClose`, always inside the panel's top corner |
| A dialog | `ContentDialog` through `ShowDialog` (`Dialogs.Dress`): icon in `Tag` (`!` for care), body width `Dialogs.Small/Medium/Large`, Primary and Cancel only |
| A card, its title, a heading | `Parts.Card`, `Parts.Title`, `Parts.Heading` |
| A tag of what something is | `Parts.Chip`; chips in a row are the same height |
| A value typed over in place | `Parts.QuietField` |
| A menu or popup | `Parts.Pop`, `Parts.PopCard`, `Parts.Sideways` |
| An empty page or list | `Empty`, `EmptyPlace` (icon and one line) |
| A message after an action | `SetInfo` / the floating result in Mods and Saves; offer Undo when possible |

Never hand-make a radio card, a pill button, a back link, a close button or a dialog footer: those already exist and the
hand-made ones are what looked "out of the system" every time (focus rectangles that ignore the corners, borders clipped
at the dialog's edge, a pill of a size nothing else has).

## 5. Behaviour

- The whole row is the hover and click target; everything clickable shows the hand cursor (`HandCursor.On`).
- Disabled looks disabled. Every warning can be ignored, and the choice is remembered. A problem offers its next step.
- Long work shows progress; the app never looks frozen. Destructive or bulk changes offer Undo (and Ctrl+Z where the
  screen has it).
- Content that does not fit scrolls (horizontal rows get round arrow buttons at both ends; the left one appears once
  scrolled), it is never cut. Scrollbars sit flush on the edge. Layouts use the whole width and are checked narrow and at
  4K fullscreen.
- Motion is short and restrained (`Design.Motion`), still when Windows' animations are off, live while resizing or
  customising. Pictures open by growing from the clicked card, sharp from the first frame.

## 6. Words

- Use the `ux-writing` skill. Plain, short, direct: a title is a noun, a button an action, a line under a title one
  sentence with no final period. Nothing poetic or descriptive.
- A count is not a title. Say the state in words ("Needs your attention") and put the first thing to look at under it.
- The app is Apex Launcher, the mod is Apex Radiance (with its own icon wherever it is named), the game is The Sims 3.
  Never mention the Steam guides.
- One word for one thing (see the glossary in `docs/launcher-design.md`). Every new key goes into all 21
  `Strings.<code>.cs`; a key no screen uses is removed.

### Example: the Home health card

An approved artifact showed a tall glass card with a large rounded corner, a pill "Open Health" button inside and a
corner glow. Built literally it looked foreign. In the system it became: a `Button` the size of a card (corner 12,
padding 20), the same surface as Play and More (frosted glass on the picture, `ApexPillBrush` on the plain card), the
crystal 56 with a small `SoftGlow` behind it, the state at 16 semibold and the first finding at 13, and a chevron instead
of an inner button, because the whole card opens Health.

## 7. Verify and polish

- Capture the real app through its own renderer (RenderTargetBitmap), never by grabbing the screen; dark and light, the
  languages with the longest words, narrow and wide. Look at every capture before saying it is done.
- Run the launcher under test only with its own data folder (`APEX_LAUNCHER_LOCALAPPDATA`), never touching the user's
  installed copy.
- End every UI round with a polish pass over every state of what changed (hover, pressed, focus, disabled, empty,
  long text, error) and the screens around it: spacing, alignment, symmetry, sizes, words. Report what was checked and
  what was not.
- During a round of tweaks, commit each change; run the tests (`ApexSetup.Tests`) once at the end. Every launcher change
  raises `<Version>` in `ApexSetup.csproj` once per release.

## 8. Rule: no feature ships without its standard pass

Every new feature, and every change to a screen or dialog, includes this before it is called done, without waiting to be
asked:

1. Audit the code of the whole screen or dialog it touches, not only the new lines: every size, space, corner and text
   size is a token, every list is `Row`s joined into one card, every icon start is an `IconTile`, every dialog goes
   through `Dialogs`. A literal number or a hand-made card found there is fixed in the same change.
2. Capture every state the feature can show (dark and light, narrow, longest language, long names, warnings and
   problems, empty, hover) and look at each one: alignment, spacing, weight, words.
3. Only then commit, build and show it, with the captures.

A feature that skipped this pass is not finished, however well it works.
