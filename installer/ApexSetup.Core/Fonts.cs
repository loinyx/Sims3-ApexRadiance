using Microsoft.Win32;

namespace ApexSetup;

// The font families installed on this PC (for everyone and for this user), from the registry's list of font files:
// "Segoe UI Semibold Italic (TrueType)" is the family "Segoe UI", "Cambria & Cambria Math (TrueType)" is two families.
public static class Fonts
{
    const string Key = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts";
    static readonly string[] Styles =
        ["Regular", "Bold", "Italic", "Oblique", "Light", "Semilight", "SemiLight", "Semibold", "SemiBold", "Demibold", "DemiBold", "Black", "Heavy", "Thin",
         "ExtraLight", "Extralight", "UltraLight", "ExtraBold", "Extrabold", "UltraBold", "Medium", "Book"];

    // the families one entry of the registry's list names
    public static IEnumerable<string> Families(string entry)
    {
        var name = entry;
        var paren = name.IndexOf('(');
        if (paren > 0) name = name[..paren];
        foreach (var part in name.Split('&', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var words = part.Split(' ', StringSplitOptions.RemoveEmptyEntries).ToList();
            while (words.Count > 1 && Styles.Contains(words[^1])) words.RemoveAt(words.Count - 1);
            if (words.Count > 0) yield return string.Join(' ', words);
        }
    }

    public static List<string> Installed()
    {
        var names = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var hive in new[] { Registry.LocalMachine, Registry.CurrentUser })
            try
            {
                using var k = hive.OpenSubKey(Key);
                foreach (var v in k?.GetValueNames() ?? [])
                    foreach (var f in Families(v))
                        if (!f.StartsWith('@')) names.Add(f);
            }
            catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { }
        return [.. names];
    }
}
