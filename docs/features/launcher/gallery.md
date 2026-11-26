# Apex Launcher: gallery and scrapbook

The Gallery shows every picture and video that Apex Radiance and the game save, from all the folders they can save to,
so changing the capture folder hides nothing. *Albums* lists them by day or by household, with favourites, captions, a
search and the player's own albums; *Scrapbook* lays them on a sideways timeline board with frames, tape, notes and
stickers, where an album is a pile that opens in place. The viewer zooms, plays a slideshow and videos, and moves a
picture to the Recycle Bin. Ctrl+Z and Ctrl+Y undo and redo.

## Status

| | |
|---|---|
| Availability | Released in 4.1.0 |
| Default | Always available |
| Menu | Gallery |
| Configuration | `gallery.json`, `thumbs\` ([data files](data-and-files.md)); the capture folders from `ApexRadiance.toml` |
| Source | [`MainWindow.Gallery.cs`](../../../installer/ApexSetup/MainWindow.Gallery.cs), [`Gallery.cs`](../../../installer/ApexSetup.Core/Gallery.cs), [`Fonts.cs`](../../../installer/ApexSetup.Core/Fonts.cs) |

## The problem

The game saves screenshots in `Documents\...\Screenshots` and its videos in `Recorded Videos`; Apex Radiance can save to
its own folders or a chosen one ([Screenshots and video](../screenshots-and-video.md)). Pictures end up in several
places with file names that say only the time, and nothing ties them to the household they show.

## How Apex Radiance solves it

1. **Folders** (`Gallery.cs`). The launcher reads `ApexRadiance.toml` where the installed ASI reads it and lists every
   folder a setting can name, all at once: for pictures `[ui] screenshot_folder` (`game`, `apex` or `custom` with
   `screenshot_custom_folder`), for videos `[capture.video] folder` and `custom_folder`. File types: `png`, `jpg`,
   `jpeg`, `bmp`; `mp4`, `mkv`, `avi`, `wmv`, `mov`, `webm`.
2. **Albums.** *All pictures*, *Favourites*, one album per household and the player's own albums (*New album*, with an
   icon and an optional household; deleting an album keeps its pictures). A picture's household is the household of
   the save written soonest after it, and can be set by hand. The album view groups *By day* or *By household*, 60
   pictures at a time, with a search over captions, households, file names, dates and album names.
3. **Scrapbook.** A horizontal timeline, newest first. *Customise* has tabs Board (colour, light, paper pattern and
   strength), Frames (polaroid, print, washi, clip, rounded, kraft, deckle, doodle, stamp, film; tape), Writing (any
   installed font) and Stickers (emoji groups; right-click for *Bigger*, *Smaller*, *Take it off*). Sticky notes come in
   five colours. Dropping a picture on another makes or joins an album shown as a pile (*Open album*, *Rename*,
   *Ungroup*); an album opened from its pile keeps its picture sizes and rows and scrolls sideways in a narrow window.
4. **Viewer.** Zoom 1x to 6x (wheel, buttons, double-click for 2x), previous and next with the arrow keys, a slideshow
   every 4 s, full screen, *Open in Photos*, videos in the window. The side panel holds the household, the caption,
   *Add to favourites*, *Add to an album*, *Use on Home*, *Show in Explorer* and *Move to the Recycle Bin* (after a
   confirmation; its data is forgotten).
5. **Undo.** Ctrl+Z, and Ctrl+Y or Ctrl+Shift+Z, up to 60 steps (changes within 700 ms are one step), not while a text
   box has focus. On the scrapbook, undo takes back board changes only and keeps the view and the open album.

## Settings

| Menu label | Key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Use on Home (viewer) | `gallery.json` `HomePicture` | button | None (the newest picture) | Any picture | The gallery picture on top of Home ([Settings](settings.md#settings)) |
| Scrapbook look (*Customise*) | `gallery.json` `Albums` (one look per album) | several | Colour `#0A0A09`, frame *rounded*, tape *match*, light *soft*, pattern *dots*, font Segoe UI Variable | | The board, frames, writing and stickers of that album's scrapbook |

## Compatibility and interactions

- The Gallery only reads the player's pictures and videos; it writes its own `gallery.json` and thumbnails, and deletes
  a picture only to the Recycle Bin when the player asks.
- A save renamed from the [Saves](saves-and-backups.md) screen keeps its household and album.

## Limitations

- The game's own `Video.avi` recordings show without a picture when Windows has no decoder for them; they play when
  Windows can.
- The household of a picture is a guess from save times; it can be wrong when several saves were written close
  together, and is then set by hand.
- Not yet verified with a real mouse: dragging a picture onto another to make or join an album
  ([validation](../../validation/installer.md#open-checks)).

## Technical reference

- Thumbnails are JPEGs of 1100 px in `thumbs\` of the data folder, named by the file's hash, length, time and size, made
  in the background at start (`WarmThumbs`); up to 90 stay in memory.
- An unreadable `gallery.json` is renamed `gallery.json.corrupt` and a new one started.

## Rejected approaches

None recorded.

## See also

- [Screenshots and video](../screenshots-and-video.md), [Home and Play](home-and-play.md)
- [Validation](../../validation/installer.md)
