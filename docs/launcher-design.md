# Apex Launcher: design system

How the launcher looks, and where each part of the look is defined. Code: `installer/ApexSetup/Design/`.

## Parts

| File | Holds |
|---|---|
| `Tokens.cs` | Distances (`Space`), corners (`Radius`), text sizes (`TextSize`), control sizes (`Size`), times (`Motion`) |
| `Themes.cs` | The colour themes (`Theme.All`) and every colour of a theme in dark and light (`Palette`) |
| `Oklch.cs` | Colours by lightness, strength and hue; WCAG contrast |
| `DesignSystem.cs` | Puts a theme into the app: App.xaml's brushes and the controls' own accent keys, changed in place |
| `IconTile.cs` | The icon at the start of a row or a card |

`App.xaml` declares the brushes (`Apex<Name>Brush`); their written values are what a Windows contrast theme keeps. Code
reads a brush with `Res("ApexCardBrush")` and a colour with `Tone("Card")`. A screen has no colour, distance or text size
of its own.

## Colour

- Surfaces, lines and texts are neutral in every theme. Colour is only in what stands out: the main button, a switch that
  is on, the selected place, links, warnings and errors.
- A theme is the two or three stops of its gradient. The main button, a switch, a checked box and the mark of a choice
  wear the gradient; the ink on it is the one with at least 4.6:1 of contrast on every stop.
- Dark and light follow Windows, or stay as chosen in Settings (`LauncherSettings.Mode`). The theme is
  `LauncherSettings.Theme`; `windows` follows Windows' accent.
- To add a theme, add a line to `Theme.All` and its name to the translations.

## Space

| Where | Value |
|---|---|
| Page margin | 32 |
| Title to the line under it | 4 |
| Head to body | 24 |
| Between sections | 32; a section's title to its content 12 |
| Card padding | 20, the same on its four sides |
| Cards side by side or stacked | 12 |
| Row | at least 68 high, 20 at both ends, icon tile 40, 16 to the words |
| Install card | 840 wide, 32 inside, header 64, footer 72 |

Something in a corner of a card stands as far from the top as from the side.

## Rows

Rows one under another are one card: `Join` gives them one outline, a line between them, and corners only on the first and
the last. A page's rows come from `Row` and `Toggle`, Game settings' from `SettingRow`.

## Text

Segoe UI Variable. Title 28 semibold, section 16 semibold, row 14 semibold, detail 13, page line 14, small label 12.

## Icons

Lucide, stroke 1.75, one family everywhere (the side pane too). Neutral; a tile takes a hue only for a warning or an error.

## Background light

`Aurora` (install and move to the launcher): quiet pools of the theme's hues, still when Windows' animations are off.
`SoftGlow`: one pool behind a logo or Health's crystal.
