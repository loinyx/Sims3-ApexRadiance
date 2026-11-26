# Apex Launcher: languages

The launcher shows its texts in the 21 languages of The Sims 3, the same set as Apex Radiance's in-game menu. It starts
in Windows' display language and can be switched from the title bar's language button or Settings > Language; a text
without a translation shows in English.

## Status

| | |
|---|---|
| Availability | Released in 3.1.1 (Apex Radiance Setup); the texts of 4.1.0 and 4.1.1 in every language: Released in 4.1.1 |
| Default | *Automatic*: Windows' display language, else English |
| Menu | Title bar language button; Settings > General > Language |
| Configuration | `language.txt` in the data folder; `APEX_LAUNCHER_LANG` for a test run |
| Source | [`Languages.cs`](../../../installer/ApexSetup/Languages.cs), [`Strings.cs`](../../../installer/ApexSetup/Strings.cs), `Strings.<code>.cs` (one table per language) |

## The problem

Many Sims 3 players do not read English, and the launcher's texts (file names, warnings before a change) must be
understood before the player clicks.

## How Apex Radiance solves it

1. **Tables.** Each language is a `Strings.<code>.cs` table keyed by the English text. `T(english, args)` looks the
   text up in the current language and falls back to English when there is no entry or it is empty. Static XAML text is
   translated when the window is built (`Localize`).
2. **Choice.** `APEX_LAUNCHER_LANG`, then `language.txt`, then Windows' display language (`CurrentUICulture`).
   Traditional Chinese for Hant, TW, HK, MO and CHT, otherwise Simplified; `nb`, `nn` and `no` map to Norwegian; any
   other language falls back to English.
3. **Menu.** *Automatic* (deletes `language.txt`) and the 21 languages, each in its own name. The window is rebuilt in
   the new language at once.

Languages: English, Portuguese, Spanish, French, German, Italian, Dutch, Polish, Russian, Czech, Hungarian, Greek,
Danish, Swedish, Norwegian, Finnish, Japanese, Korean, Simplified Chinese, Traditional Chinese, Thai (`en`, `pt`, `es`,
`fr`, `de`, `it`, `nl`, `pl`, `ru`, `cs`, `hu`, `el`, `da`, `sv`, `no`, `fi`, `ja`, `ko`, `zh_hans`, `zh_hant`, `th`).

## Settings

| Menu label | Key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Language | `language.txt` | choice | Automatic | Automatic, the 21 languages | The launcher's language |

## Compatibility and interactions

- The launcher's tables are separate from Apex Radiance's in-game translations (`i18n/`); the two share the language
  list, not the texts.

## Limitations

- A text added to the code shows in English in every language until its tables get the entry.

## Technical reference

- The Portuguese table is the reference list of keys. `ApexSetup.Tests` checks that every table loads, holds only keys
  of that list, keeps each key's `{n}` placeholders, has no empty text and no key twice, and prints how many texts each
  language has; missing texts are counted, not failed.
- The layout tour (`APEX_LAUNCHER_AUDIT_TOUR`) opens the screens at several window sizes in every language to find cut
  texts ([validation](../../validation/installer.md)).

## Rejected approaches

None recorded.

## See also

- [Settings](settings.md)
- [Validation](../../validation/installer.md)
