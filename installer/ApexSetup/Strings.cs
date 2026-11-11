using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace ApexSetup;

// English is the key, as in the mod's menu (ui/i18n.h): T("English") returns the text in the current language (Lang), and
// English for a text without a pair. Static XAML texts are translated in place by Localize. The tables: Languages.cs.
public static partial class Strings
{
    public static string T(string en, params object[] args)
    {
        var s = Table(Lang) is { } table && table.TryGetValue(en, out var t) && t.Length > 0 ? t : en;
        return args.Length == 0 ? s : string.Format(s, args);
    }

    public static void Localize(object? e)
    {
        // the screen reader's name (also of a Tag="dyn" element: its name is static, its text is not)
        if (e is FrameworkElement fe && AutomationProperties.GetName(fe) is { Length: > 0 } name)
            AutomationProperties.SetName(fe, T(Remember(fe, name)));
        switch (e)
        {
            case FrameworkElement { Tag: "dyn" }:
                return;
            case TextBlock tb:
                tb.Text = T(Remember(tb, tb.Text));
                break;
            case NavigationView nv:
                foreach (var i in nv.MenuItems.Concat(nv.FooterMenuItems)) Localize(i);
                Localize(nv.Content);
                break;
            case ScrollViewer sv:
                Localize(sv.Content);
                break;
            case ContentControl cc when cc.Content is string s:
                cc.Content = T(Remember(cc, s));
                break;
            case ContentControl cc:
                Localize(cc.Content);
                break;
            case Panel p:
                foreach (var c in p.Children) Localize(c);
                break;
            case Border b:
                Localize(b.Child);
                break;
        }
    }

    // the English key of a static text shown in any language (Tag="dyn" elements are set from code instead); an English key
    // stays itself, so a translation that happens to be another key's English text does not move it
    static Dictionary<string, string>? _english;
    static string Remember(object o, string current)
    {
        if (PtBr.ContainsKey(current)) return current;
        _english ??= Languages.SelectMany(l => (IEnumerable<KeyValuePair<string, string>>?)Table(l.Code) ?? [])
            .Where(p => p.Value.Length > 0).GroupBy(p => p.Value).ToDictionary(g => g.Key, g => g.First().Key);
        return _english.TryGetValue(current, out var en) ? en : current;
    }
}
