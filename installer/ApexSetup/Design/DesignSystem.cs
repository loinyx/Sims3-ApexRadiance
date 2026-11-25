using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace ApexSetup.Design;

// Puts a theme into the app: the colours of App.xaml's brushes and of the controls' own accent brushes change in place, so what is
// on screen follows at once. What code drew with a colour of its own is drawn again by the window (MainWindow.ThemeChanged).
public static class DesignSystem
{
    public static Theme Theme { get; private set; } = Theme.All[0];
    static Palette _dark = new(Theme.All[0], false), _light = new(Theme.All[0], true);
    public static Palette Now => App.Light ? _light : _dark;

    static readonly Color White = Color.FromArgb(255, 255, 255, 255), Black = Color.FromArgb(255, 0, 0, 0);

    // one brush for every main button, its stops recoloured with the theme (made on the UI thread, at start)
    static LinearGradientBrush? _main, _over, _down;
    static SolidColorBrush? _ink, _edge, _accent, _accentText, _dialog, _dialogFoot, _dialogEdge;
    static LinearGradientBrush Sweep() => new() { StartPoint = new(0, 0), EndPoint = new(1, 1) };
    public static Brush MainBrush => _main ?? (Brush)App.Brush("ApexAccentBrush");

    static bool HighContrast => new global::Windows.UI.ViewManagement.AccessibilitySettings().HighContrast;

    // once, at start
    public static void Install(string? theme)
    {
        if (HighContrast) return;
        (_main, _over, _down, _ink, _edge, _accent, _accentText, _dialog, _dialogFoot, _dialogEdge) = (Sweep(), Sweep(), Sweep(), new(), new(), new(), new(), new(), new(), new());
        Into(Application.Current.Resources);
        Apply(theme);
    }

    // the names the controls look these up by. In the app, and again in the window and in each dialog: a control's states (under the
    // pointer, pressed) read the nearest ones, and the app's alone leave them to Windows' accent
    public static void Into(ResourceDictionary res)
    {
        if (_main is null) return;
        foreach (var (state, fill) in new[] { ("", _main), ("PointerOver", _over!), ("Pressed", _down!) })
        {
            res["AccentButtonBackground" + state] = fill;
            res["AccentButtonForeground" + state] = _ink;
            res["AccentButtonBorderBrush" + state] = _edge;
            // a switch that is on, a checked box and the choice of a round one wear the same gradient
            res["ToggleSwitchFillOn" + state] = res["ToggleSwitchStrokeOn" + state] = fill;
            res["ToggleSwitchKnobFillOn" + state] = _ink;
            res["CheckBoxCheckBackgroundFillChecked" + state] = res["CheckBoxCheckBackgroundStrokeChecked" + state] = fill;
            res["CheckBoxCheckBackgroundFillIndeterminate" + state] = res["CheckBoxCheckBackgroundStrokeIndeterminate" + state] = fill;
            res["CheckBoxCheckGlyphForegroundChecked" + state] = res["CheckBoxCheckGlyphForegroundIndeterminate" + state] = _ink;
            res["RadioButtonOuterEllipseCheckedFill" + state] = res["RadioButtonOuterEllipseCheckedStroke" + state] = fill;
        }
        // links, sliders, progress, the mark of the selected place, the line of a field with the keyboard in it. By name and not in the
        // controls' own dictionaries: those answer with the colours of Windows' theme, whatever the window's is
        foreach (var state in new[] { "", "PointerOver", "Pressed" })
        {
            res["HyperlinkButtonForeground" + state] = _accentText;
            res["SliderTrackValueFill" + state] = res["SliderThumbBackground" + state] = _accent;
        }
        foreach (var key in new[]
        {
            "GridViewItemSelectedBorderBrush", "GridViewItemSelectedPointerOverBorderBrush", "GridViewItemSelectedPressedBorderBrush", "NavigationViewSelectionIndicatorForeground", "ListViewItemSelectionIndicatorBrush", "ListViewItemSelectionIndicatorPointerOverBrush", "ListViewItemSelectionIndicatorPressedBrush", "ComboBoxItemPillFillBrush",
            "SelectorBarItemPillFill", "ProgressBarForeground", "ProgressBarIndeterminateForeground", "ProgressRingForegroundThemeBrush", "TextControlBorderBrushFocused", "TextControlSelectionHighlightColor",
        }) res[key] = _accent;
        res["ListViewItemCornerRadius"] = new CornerRadius(Radius.Control);
        res["GridViewItemCornerRadius"] = new CornerRadius(Radius.Panel);
        (res["ContentDialogTopOverlay"], res["ContentDialogBackground"], res["ContentDialogSeparatorBorderBrush"], res["ContentDialogBorderBrush"]) = (_dialog, _dialogFoot, _dialogEdge, _dialogEdge);
    }

    public static void Apply(string? theme)
    {
        if (HighContrast) return;
        Theme = Theme.Of(theme);
        (_dark, _light) = (new(Theme, false), new(Theme, true));
        foreach (var (name, p) in new[] { ("Default", _dark), ("Light", _light) })
            foreach (var (key, c) in p.Colors) Paint(name, "Apex" + key + "Brush", c);
        Restate();
    }

    // the brushes that are one for dark and light
    public static void Restate()
    {
        if (_main is null) return;
        (_accent!.Color, _accentText!.Color) = (Now["Accent"], Now["AccentText"]);
        (_dialog!.Color, _dialogFoot!.Color, _dialogEdge!.Color) = (Now["Card"], Now["Footer"], Now["CardBorder"]);
        Stops(_main, Now.Gradient, 0);
        Stops(_over!, Now.Gradient, 0.08);
        Stops(_down!, Now.Gradient, -0.08);
        _ink!.Color = Now["Ink"];
        _edge!.Color = Color.FromArgb(0x24, 255, 255, 255);
    }

    // a theme's gradient on its own, for its tile in Settings
    public static LinearGradientBrush Swatch(Theme t)
    {
        var b = Sweep();
        Stops(b, t.Gradient().Stops, 0);
        return b;
    }

    static void Stops(LinearGradientBrush b, Color[] colors, double lift)
    {
        while (b.GradientStops.Count > colors.Length) b.GradientStops.RemoveAt(b.GradientStops.Count - 1);
        for (int i = 0; i < colors.Length; i++)
        {
            if (i == b.GradientStops.Count) b.GradientStops.Add(new GradientStop());
            b.GradientStops[i].Color = Mix(colors[i], lift >= 0 ? White : Black, Math.Abs(lift));
            b.GradientStops[i].Offset = i / Math.Max(1.0, colors.Length - 1);
        }
    }

    static Color Mix(Color a, Color b, double k) =>
        Color.FromArgb(255, (byte)(a.R + (b.R - a.R) * k), (byte)(a.G + (b.G - a.G) * k), (byte)(a.B + (b.B - a.B) * k));

    static void Paint(string theme, string key, Color c, double opacity = 1)
    {
        foreach (var d in All(Application.Current.Resources))
            if (d.ThemeDictionaries.TryGetValue(theme, out var t) && t is ResourceDictionary td && td.TryGetValue(key, out var v) && v is SolidColorBrush b)
                (b.Color, b.Opacity) = (c, opacity);
    }

    static IEnumerable<ResourceDictionary> All(ResourceDictionary d) => new[] { d }.Concat(d.MergedDictionaries.SelectMany(All));
}
