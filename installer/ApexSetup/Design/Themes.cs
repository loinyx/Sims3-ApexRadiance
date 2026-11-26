using Windows.UI;

namespace ApexSetup.Design;

// One colour theme: the stops of its gradient (the main buttons, its tile in Settings).
// Surfaces, lines and texts are neutral in every theme (Palette): the colour is only in what stands out.
public sealed record Theme(string Id, string Name, (double L, double C, double H)[] Stops)
{
    public double Hue => Stops[0].H;

    // the hues of its background light: its own, and what lies between them
    public double[] Sky => Stops.Zip(Stops.Skip(1), (a, b) => (a.H + ((b.H - a.H + 540) % 360 - 180) / 2 + 360) % 360).Concat(Stops.Select(s => s.H)).ToArray();

    public static readonly Theme[] All =
    [
        new("amethyst", "Amethyst", [(0.60, 0.21, 300), (0.53, 0.24, 283)]),
        new("orchid", "Orchid", [(0.60, 0.22, 345), (0.53, 0.24, 300)]),
        new("aurora", "Aurora", [(0.80, 0.14, 165), (0.72, 0.14, 240), (0.72, 0.17, 305)]),
        new("lagoon", "Lagoon", [(0.80, 0.12, 190), (0.70, 0.15, 245)]),
        new("emerald", "Emerald", [(0.84, 0.16, 150), (0.72, 0.13, 185)]),
        new("honey", "Honey", [(0.88, 0.14, 95), (0.77, 0.16, 58)]),
        new("sunset", "Sunset", [(0.82, 0.13, 70), (0.70, 0.19, 18)]),
        new("cherry", "Cherry", [(0.60, 0.21, 25), (0.54, 0.22, 358)]),
        new("sakura", "Sakura", [(0.82, 0.11, 355), (0.72, 0.15, 312)]),
        new("graphite", "Graphite", [(0.96, 0.0, 270), (0.78, 0.0, 270)]),
        new("moonlight", "Moonlight", [(0.86, 0.05, 260), (0.73, 0.11, 280)]),
    ];

    // the one that follows Windows' accent colour
    public const string Windows = "windows";

    public static Theme Of(string? id)
    {
        if (id != Windows) return All.FirstOrDefault(t => t.Id == id) ?? All[0];
        var (l, c, h) = Oklch.From(new global::Windows.UI.ViewManagement.UISettings().GetColorValue(global::Windows.UI.ViewManagement.UIColorType.Accent));
        l = Math.Clamp(l, 0.5, 0.8);
        c = Math.Min(c, 0.18);
        return new(Windows, "Windows colour", [(l, c, h), (Math.Max(0.45, l - 0.07), c, (h + 12) % 360)]);
    }

    // the gradient as colours, and the ink that reads on all of it
    (Color[] Stops, Color Ink)? _made; // (worked out once)
    public (Color[] Stops, Color Ink) Gradient() => _made ??= Make();
    (Color[] Stops, Color Ink) Make()
    {
        var white = Color.FromArgb(255, 255, 255, 255);
        var ink = Stops.Average(s => s.L) < 0.63 ? white : Oklch.Of(0.18, 0.04, Hue);
        return (Stops.Select(s =>
        {
            var l = s.L;
            while (Oklch.Contrast(Oklch.Of(l, s.C, s.H), ink) < 4.6 && l is > 0.3 and < 0.95) l += ink == white ? -0.005 : 0.005;
            return Oklch.Of(l, s.C, s.H);
        }).ToArray(), ink);
    }
}

// Every colour of a theme in dark or in light, by the name App.xaml's brushes have ("Card" is ApexCardBrush).
public sealed class Palette
{
    public Dictionary<string, Color> Colors { get; } = [];
    public Color[] Gradient { get; }
    public Color this[string name] => Colors[name];

    public Palette(Theme t, bool light)
    {
        var h = t.Hue;
        (Gradient, var ink) = t.Gradient();
        var strong = Math.Min(0.16, t.Stops[0].C); // (a theme of quiet colours stays quiet)
        Put("Ink", ink);
        Put("OnAccent", light ? Color.FromArgb(255, 255, 255, 255) : Oklch.Of(0.18, 0.04, h));
        if (light)
        {
            N("Page", 0.965); N("Pane", 0.958); N("Content", 0.985); N("Card", 1); N("Raised", 1);
            N("Hover", 0.975); N("Pill", 0.950); N("Strip", 0.965); N("Footer", 0.972);
            N("CardBorder", 0.915); N("InputBorder", 0.870); N("InputBorderHover", 0.740); N("Selected", 0.930);
            N("SoftText", 0.370); N("Muted", 0.510); N("IconMuted", 0.720);
            Put("Accent", Oklch.Of(0.520, Math.Max(0.10, strong * 1.15), h)); Put("AccentText", Oklch.Of(0.450, Math.Max(0.10, strong), h));
            Put("AccentSoft", Oklch.Of(0.600, strong, h, 0.10)); Put("AccentLine", Oklch.Of(0.560, strong, h, 0.38));
            S("Success", 0.500, 0.12, 158); S("Warning", 0.550, 0.12, 68); S("Error", 0.530, 0.19, 25);
            S("SuccessSoft", 0.600, 0.13, 158, 0.12); S("WarningSoft", 0.750, 0.13, 78, 0.16); S("WarningLine", 0.650, 0.13, 78, 0.35); S("WarningText", 0.430, 0.09, 72);
            S("DangerSoft", 0.600, 0.19, 25, 0.10); S("DangerText", 0.470, 0.17, 25);
            Put("Glass", Color.FromArgb(0xF0, 255, 255, 255)); Put("Hairline", Color.FromArgb(0x12, 0, 0, 0)); Put("Tile", Color.FromArgb(0x0B, 0, 0, 0)); Put("Veil", Color.FromArgb(0xB8, 255, 255, 255)); Put("GlassLine", Color.FromArgb(0x12, 0, 0, 0));
            Touch(0, 0x0A, 0x12, 0x08);
            return;
        }
        N("Page", 0.115); N("Pane", 0.110); N("Content", 0.142); N("Card", 0.178); N("Raised", 0.222);
        N("Hover", 0.202); N("Pill", 0.232); N("Strip", 0.160); N("Footer", 0.150);
        N("CardBorder", 0.240); N("InputBorder", 0.300); N("InputBorderHover", 0.400); N("Selected", 0.255);
        N("SoftText", 0.810); N("Muted", 0.670); N("IconMuted", 0.420);
        Put("Accent", Oklch.Of(0.740, strong, h)); Put("AccentText", Oklch.Of(0.825, strong * 0.75, h));
        Put("AccentSoft", Oklch.Of(0.700, strong, h, 0.13)); Put("AccentLine", Oklch.Of(0.700, strong, h, 0.45));
        S("Success", 0.780, 0.14, 158); S("Warning", 0.810, 0.13, 78); S("Error", 0.720, 0.16, 22);
        S("SuccessSoft", 0.780, 0.14, 158, 0.12); S("WarningSoft", 0.810, 0.13, 78, 0.10); S("WarningLine", 0.810, 0.13, 78, 0.28); S("WarningText", 0.860, 0.08, 82);
        S("DangerSoft", 0.720, 0.16, 22, 0.14); S("DangerText", 0.840, 0.09, 22);
        Put("Glass", Oklch.Of(0.165, 0, 0, 0.94)); Put("Hairline", Color.FromArgb(0x10, 255, 255, 255)); Put("Tile", Color.FromArgb(0x12, 255, 255, 255)); Put("Veil", Color.FromArgb(0xA6, 0, 0, 0)); Put("GlassLine", Color.FromArgb(0x12, 255, 255, 255));
        Touch(255, 0x0F, 0x1A, 0x0B);

        void Put(string name, Color c) => Colors[name] = c;
        // what is pressed, at rest, under the pointer and held: one ink at four strengths, so a fade from one to another never
        // passes through a lighter colour (Windows' "Transparent" is a clear white)
        void Touch(byte ink, byte rest, byte over, byte down)
        {
            foreach (var (name, a) in new[] { ("Clear", (byte)0), ("Fill", rest), ("FillOver", over), ("FillDown", down) }) Put(name, Color.FromArgb(a, ink, ink, ink));
        }
        void N(string name, double l) => Put(name, Oklch.Of(l, 0, 0));
        void S(string name, double l, double c, double hue, double a = 1) => Put(name, Oklch.Of(l, c, hue, a));
    }
}
