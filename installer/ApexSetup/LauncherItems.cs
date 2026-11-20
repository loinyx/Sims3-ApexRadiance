using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// The items the launcher's lists show through compiled bindings (x:Bind in MainWindow.xaml), so the trimmed exe keeps them.

public sealed class ModRow : INotifyPropertyChanged
{
    public ModFile File { get; private set; }
    public IReadOnlyList<string> Findings { get; private set; }
    // every kind it holds, for the filters ("Script", "Hair", "Objects"...); the first is what the row shows
    public IReadOnlyList<string> Kinds { get; set; }
    public bool IsNew { get; set; }
    // what of it replaces the game's own resources (a default replacement), when the game's index is read
    public Replacements Defaults { get; set; } = Replacements.None;
    public string? DuplicateOf { get; set; }

    public string Name => File.FileType == "package" ? System.IO.Path.GetFileNameWithoutExtension(File.Name) : File.Name;
    public string FullName => File.Name + "\n" + Place(File);
    public string Kind => KindOverride ?? KindName(File.Kind);
    public string KindIcon => KindIconOverride ?? IconOf(File.Kind);
    // what the package index says, and the row's label and icon when the player chose its type
    public IReadOnlyList<string> DetectedKinds { get; set; } = [];
    public string? KindOverride { get; set; }
    public string? KindIconOverride { get; set; }
    public string Size => SizeText(File.Size);
    // (a very large package: worth knowing, it slows loading)
    public Brush SizeBrush => File.Size > 200L << 20 ? Res("ApexWarningBrush") : Res("ApexMutedBrush");
    public Brush TileBrush { get; }
    public static bool ShowPlace = true;
    public string Sub => Findings.Count > 0 ? Findings[0]
        : DuplicateOf is { } d ? T("Same as {0}", d)
        : ModNotes.Get(File.Rel)?.Notes is { } n ? n.Split('\n')[0]
        : ShowPlace ? Place(File) : "";
    public Brush SubBrush => Findings.Count > 0 || DuplicateOf is not null ? Res("ApexWarningBrush") : Res("ApexMutedBrush");
    public Visibility LookVisible => Findings.Count > 0 || DuplicateOf is not null ? Visibility.Visible : Visibility.Collapsed;
    public Visibility TagsVisible => TagPills.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility SubVisible => Sub.Length > 0 || TagPills.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility NewVisible => IsNew ? Visibility.Visible : Visibility.Collapsed;
    public string NewText => T("New");
    // up to two tags as pills, then "+n"
    public List<TagDot> TagPills
    {
        get
        {
            var tags = ModNotes.TagsOf(File.Rel);
            var list = tags.Take(2).Select(t => TagDot.Of(t, ModNotes.ColorOf(t))).ToList();
            if (tags.Count > 2) list.Add(new TagDot($"+{tags.Count - 2}", Res("ApexMutedBrush"), Res("ApexPillBrush")));
            return list;
        }
    }
    public bool IsOn => File.Enabled;
    public double TextOpacity => File.Enabled ? 1 : 0.5;
    public Visibility OffVisible => File.Enabled ? Visibility.Collapsed : Visibility.Visible;
    public string OffText => T("Off");
    public Visibility ToggleVisible => File.Locked ? Visibility.Collapsed : Visibility.Visible;
    public Visibility LockVisible => File.Locked ? Visibility.Visible : Visibility.Collapsed;
    public string LockTip => LockText(File);
    public Visibility Placeholder => _image is null ? Visibility.Visible : Visibility.Collapsed;

    // the list's columns, narrowed on a small window (set before the rows are shown)
    public static GridLength KindColumn { get; set; } = new(140);
    public static GridLength SizeColumn { get; set; } = new(72);
    public static double NameMax { get; set; } = 10000;

    // the screen's handler: switch the file and return the moved file, or null when it stayed (the reason shown there)
    public static Func<ModRow, bool, ModFile?>? Toggle;

    public ModRow(ModFile f, IReadOnlyList<string> findings)
    {
        File = f;
        Findings = findings;
        Kinds = [f.Kind.ToString()];
        // one soft hue per kind: the glyph in it, the tile a faint wash of it (calmer than solid blocks of colour)
        var hue = f.Kind switch
        {
            ModKind.Script => Rgb(0x8E, 0x9C, 0xFF), ModKind.Cas => Rgb(0xF0, 0x8F, 0xC4), ModKind.Objects => Rgb(0xE8, 0xC4, 0x66),
            ModKind.Lighting => Rgb(0xF2, 0xB2, 0x5C), ModKind.Sound => Rgb(0x62, 0xCB, 0xDD), ModKind.Textures => Rgb(0x93, 0xD2, 0x7C),
            ModKind.Tuning => Rgb(0x6E, 0xD3, 0xA8), ModKind.Interface => Rgb(0xB5, 0x98, 0xF6),
            ModKind.Asi => Rgb(0xA6, 0xAC, 0xCC),
            _ => Rgb(0xA2, 0xA5, 0xB2),
        };
        GlyphBrush = new SolidColorBrush(hue);
        TileBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x24, hue.R, hue.G, hue.B));
    }

    public Brush GlyphBrush { get; }

    // the row's checkbox mirrors the list's selection; a click on it adds or takes the row out (the screen's handler)
    bool _picked;
    public bool Picked { get => _picked; set { if (_picked != value) { _picked = value; Changed(nameof(Picked)); } } }
    public static Action<ModRow>? Check;
    public void OnCheck() => Check?.Invoke(this);

    static Windows.UI.Color Rgb(byte r, byte g, byte b) => Windows.UI.Color.FromArgb(255, r, g, b);
    static Brush Res(string k) => App.Brush(k);

    public static string IconOf(ModKind k) => k switch
    {
        ModKind.Script => "code-xml", ModKind.Cas => "shirt", ModKind.Objects => "sofa", ModKind.Lighting => "lamp", ModKind.Sound => "music",
        ModKind.Textures => "image", ModKind.Tuning => "sliders-vertical", ModKind.Interface => "app-window", ModKind.Asi => "plug",
        _ => "puzzle",
    };

    public static string SizeText(long n) => n >= 1L << 30 ? $"{n / (1024.0 * 1024 * 1024):0.#} GB" : n >= 1 << 20 ? $"{n / (1024.0 * 1024):0.#} MB" : $"{Math.Max(1, n / 1024)} KB";

    // where a mod is, as the side pane names it ("Packages › NRaas"); the game folder
    public static string Place(ModFile f) => f.FileType switch
    {
        "asi" => T("Game folder"),
        _ => string.Join(" \u203a ", f.Folder.Split(System.IO.Path.DirectorySeparatorChar).Skip(1)) is { Length: > 0 } p ? p : "Mods",
    };

    public static string LockText(ModFile f) => f.FileType switch
    {
        "asi" => T("In the game folder: installed and removed in Components or by hand."),
        _ => T("Installed by the launcher: switch it in Components."),
    };

    public static string KindName(ModKind k) => k switch
    {
        ModKind.Script => T("Script mod"), ModKind.Cas => T("Create a Sim"), ModKind.Objects => T("Objects"), ModKind.Lighting => T("Lighting"),
        ModKind.Sound => T("Music and sound"), ModKind.Textures => T("Textures"), ModKind.Tuning => T("Tuning"), ModKind.Interface => T("Interface"),
        ModKind.Asi => T("ASI plugin"), _ => T("Other"),
    };

    ImageSource? _image;
    public ImageSource? Image { get => _image; set { _image = value; Changed(nameof(Image), nameof(Placeholder)); } }

    // the file moved, was renamed or switched (the same row stays in the list)
    public void Update(ModFile f)
    {
        File = f;
        Changed(nameof(Name), nameof(FullName), nameof(Sub), nameof(SubVisible), nameof(IsOn), nameof(TextOpacity), nameof(OffVisible), nameof(ToggleVisible), nameof(LockVisible), nameof(TagPills), nameof(TagsVisible));
    }

    public void SetFindings(IReadOnlyList<string> findings) { Findings = findings; Refresh(); }

    // notes, tags, findings or the New mark changed
    public void Refresh() => Changed(nameof(Sub), nameof(SubVisible), nameof(SubBrush), nameof(LookVisible), nameof(TagPills), nameof(TagsVisible), nameof(NewVisible), nameof(Kind), nameof(KindIcon));

    public void OnToggled(object sender, RoutedEventArgs e)
    {
        if (sender is not Microsoft.UI.Xaml.Controls.ToggleSwitch t || t.IsOn == File.Enabled) return; // (the binding setting it back)
        if (Toggle?.Invoke(this, t.IsOn) is { } moved) Update(moved);
        else Changed(nameof(IsOn));
    }

    // (the name a screen reader and UI tests read for the list item)
    public override string ToString() => Name;

    public event PropertyChangedEventHandler? PropertyChanged;
    void Changed(params string[] names) { foreach (var n in names) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n)); }
}

public sealed record TagDot(string Name, Brush Fill, Brush Back)
{
    public static TagDot Of(string name, string hex) => new(name, BrushOf(hex), BrushOf(hex, 0x26));

    public static Brush BrushOf(string hex, byte alpha = 255)
    {
        var c = Convert.ToUInt32(hex.TrimStart('#'), 16);
        return new SolidColorBrush(Windows.UI.Color.FromArgb(alpha, (byte)(c >> 16), (byte)(c >> 8), (byte)c));
    }
}

public sealed class SaveCard(Save save) : INotifyPropertyChanged
{
    public Save Save { get; } = save;
    public string Name => Save.Name;
    public string When => T("Last played {0}", Save.LastPlayed.ToString("g"));
    public string PlayText => T("Play this save");
    public string BackupText => T("Back up this save");
    public static Action<SaveCard>? Play, Backup;
    public void OnPlay(object sender, RoutedEventArgs e) => Play?.Invoke(this);
    public void OnBackup(object sender, RoutedEventArgs e) => Backup?.Invoke(this);
    ImageSource? _photo;
    public ImageSource? Photo { get => _photo; set { _photo = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Photo))); PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Placeholder))); } }
    public Visibility Placeholder => _photo is null ? Visibility.Visible : Visibility.Collapsed;
    public event PropertyChangedEventHandler? PropertyChanged;

    // image bytes from a package (a JPEG or PNG the game stored) as something an Image shows; null when not an image
    public static async Task<ImageSource?> ImageOf(byte[]? bytes)
    {
        if (bytes is null || bytes.Length < 8) return null;
        try
        {
            var bmp = new BitmapImage();
            using var ms = new Windows.Storage.Streams.InMemoryRandomAccessStream();
            await ms.WriteAsync(System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions.AsBuffer(bytes));
            ms.Seek(0);
            await bmp.SetSourceAsync(ms);
            return bmp;
        }
        catch (Exception) { return null; } // (a thumbnail in a format Windows does not read: the placeholder stays)
    }
}
