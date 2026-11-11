using System.Globalization;

namespace ApexSetup;

// Setup's languages: the 21 of The Sims 3, the same set, order, names and automatic choice as the mod's menu (ui/i18n.cpp).
// Each language but English is one table, Strings.<code>.cs. No WinUI here: ApexSetup.Tests compiles this file and the tables.
public static partial class Strings
{
    // (code as in the menu's [ui] language and i18n/lang_<code>.cpp, the language's own name)
    public static readonly (string Code, string Name)[] Languages =
    {
        ("en", "English"), ("pt", "Português"), ("es", "Español"), ("fr", "Français"),
        ("de", "Deutsch"), ("it", "Italiano"), ("nl", "Nederlands"), ("pl", "Polski"),
        ("ru", "Русский"), ("cs", "Čeština"), ("hu", "Magyar"), ("el", "Ελληνικά"),
        ("da", "Dansk"), ("sv", "Svenska"), ("no", "Norsk"), ("fi", "Suomi"),
        ("ja", "日本語"), ("ko", "한국어"), ("zh_hans", "简体中文"), ("zh_hant", "繁體中文"),
        ("th", "ไทย"),
    };

    // the language shown: a code of Languages
    public static string Lang { get; set; } = "en";

    // every table, registered here only (a switch, not a static dictionary: the tables live in other files of this partial
    // class, whose static fields may be set after this one's); null for English and unknown codes
    public static Dictionary<string, string>? Table(string code) => code switch
    {
        "pt" => PtBr,
        "es" => Es,
        "fr" => Fr,
        "de" => De,
        "it" => It,
        "nl" => Nl,
        "pl" => Pl,
        "ru" => Ru,
        "cs" => Cs,
        "hu" => Hu,
        "el" => El,
        "da" => Da,
        "sv" => Sv,
        "no" => No,
        "fi" => Fi,
        "ja" => Ja,
        "ko" => Ko,
        "zh_hans" => ZhHans,
        "zh_hant" => ZhHant,
        "th" => Th,
        _ => null,
    };

    // the language for a Windows display language, as the menu's SystemLanguage(): Chinese by script (Taiwan, Hong Kong,
    // Macao and Hant write Traditional characters), Bokmal and Nynorsk are Norsk, every Portuguese is pt, else English
    public static string SystemLanguage(CultureInfo c)
    {
        var two = c.TwoLetterISOLanguageName;
        if (two == "zh") return c.Name.Split('-').Any(p => p is "Hant" or "TW" or "HK" or "MO" or "CHT") ? "zh_hant" : "zh_hans";
        if (two is "nb" or "nn" or "no") return "no";
        return Languages.Any(l => l.Code == two) ? two : "en";
    }

    // the BCP-47 tag WinUI takes as an element's Language: it picks the right font for the Han characters of ja, zh_hans
    // and zh_hant
    public static string Bcp47(string code) => code switch { "zh_hans" => "zh-Hans", "zh_hant" => "zh-Hant", "no" => "nb", _ => code };

    // the player's choice, kept next to games.txt: %LOCALAPPDATA%\Apex Radiance Setup\language.txt holds a code; no file
    // (or an unknown code) is automatic
    static string ChoiceFile => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Apex Radiance Setup", "language.txt");

    public static string? SavedChoice()
    {
        try { return File.Exists(ChoiceFile) && File.ReadAllText(ChoiceFile).Trim() is var c && Languages.Any(l => l.Code == c) ? c : null; }
        catch (Exception) { return null; } // (unreadable: automatic)
    }

    public static void SaveChoice(string? code)
    {
        try
        {
            if (code is null) { File.Delete(ChoiceFile); return; }
            Directory.CreateDirectory(Path.GetDirectoryName(ChoiceFile)!);
            File.WriteAllText(ChoiceFile, code);
        }
        catch (Exception) { } // (not saved: the choice holds until Setup closes)
    }
}
