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
    public string Name => File.FileType == "package" ? System.IO.Path.GetFileNameWithoutExtension(File.Name) : File.Name;
    public string Kind => KindName(File.Kind);
    public string Size => SizeText(File.Size);
    public string Mono => File.Kind switch
    {
        ModKind.Script => "S", ModKind.Cas => "CAS", ModKind.Objects => "OBJ", ModKind.Lighting => "LT", ModKind.Sound => "SND", ModKind.Textures => "TX",
        ModKind.Tuning => "TN", ModKind.Interface => "UI", ModKind.Asi => "ASI", ModKind.Sims3Pack => "S3P", ModKind.World => "W", ModKind.Lot => "LOT",
        ModKind.Household => "HH", _ => "PK",
    };
    public Brush TileBrush { get; }
    // under the name: what needs a look, else where the mod is
    // under the name: what needs a look, else the player's notes, else where it is (not in one folder: it goes without saying)
    public static bool ShowPlace = true;
    public string Sub => Findings.Count > 0 ? Findings[0]
        : ModNotes.Get(File.Rel)?.Notes is { } n ? n.Split('\n')[0]
        : ShowPlace ? Place(File) : "";
    public Visibility SubVisible => Sub.Length > 0 || TagDots.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    public List<TagDot> TagDots => ModNotes.TagsOf(File.Rel).Select(t => new TagDot(t, TagDot.BrushOf(ModNotes.ColorOf(t)))).ToList();
    public Brush SubBrush => Findings.Count > 0 ? Res("ApexWarningBrush") : Res("ApexMutedBrush");
    public string Chip => Findings.Count > 0 ? T("Needs a look") : Kind;
    public Brush ChipBrush => Findings.Count > 0 ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x2A, 0x21, 0x12)) : Res("ApexPillBrush");
    public Brush ChipText => Findings.Count > 0 ? Res("ApexWarningBrush") : Res("ApexSoftTextBrush");
    public bool IsOn => File.Enabled;
    public double TextOpacity => File.Enabled ? 1 : 0.5;
    public Visibility ToggleVisible => File.Locked ? Visibility.Collapsed : Visibility.Visible;
    public Visibility LockVisible => File.Locked ? Visibility.Visible : Visibility.Collapsed;
    public string LockTip => LockText(File);

    // the screen's handler: switch the file and return the moved file, or null when it stayed (the reason shown there)
    public static Func<ModRow, bool, ModFile?>? Toggle;

    public ModRow(ModFile f, IReadOnlyList<string> findings)
    {
        File = f;
        Findings = findings;
        TileBrush = new SolidColorBrush(f.Kind switch
        {
            ModKind.Script => Rgb(0x3F, 0x4A, 0x7A), ModKind.Cas => Rgb(0x7A, 0x3F, 0x62), ModKind.Objects => Rgb(0x7A, 0x6A, 0x2E),
            ModKind.Lighting => Rgb(0x8A, 0x6E, 0x2E), ModKind.Sound => Rgb(0x2E, 0x6E, 0x7A), ModKind.Textures => Rgb(0x4E, 0x7A, 0x3F),
            ModKind.Tuning => Rgb(0x3F, 0x7A, 0x5B), ModKind.Interface => Rgb(0x5B, 0x3F, 0x7A), ModKind.World => Rgb(0x2E, 0x5E, 0x7A),
            ModKind.Lot => Rgb(0x8A, 0x4E, 0x3A), ModKind.Household => Rgb(0x7A, 0x5A, 0x2E), _ => Rgb(0x34, 0x36, 0x40),
        });
    }

    static Windows.UI.Color Rgb(byte r, byte g, byte b) => Windows.UI.Color.FromArgb(255, r, g, b);
    static Brush Res(string k) => (Brush)Application.Current.Resources[k];

    public static string SizeText(long n) => n >= 1 << 20 ? $"{n / (1024.0 * 1024):0.#} MB" : $"{Math.Max(1, n / 1024)} KB";

    // where a mod is, as the folders pane names it ("Packages \u203A NRaas"); the game folder, Downloads or the Library
    public static string Place(ModFile f) => f.FileType switch
    {
        "asi" => T("Game folder"),
        "sims3pack" => T("Downloads"),
        "library" => T("Library"),
        _ => string.Join(" \u203a ", f.Folder.Split(System.IO.Path.DirectorySeparatorChar).Skip(1)) is { Length: > 0 } p ? p : "Mods",
    };

    public static string LockText(ModFile f) => f.FileType switch
    {
        "asi" => T("In the game folder: installed and removed in Components or by hand."),
        "sims3pack" => T("Installed and removed with the game's own launcher."),
        "library" => T("A household or lot saved to the Library: managed in the game."),
        _ => T("Installed by the launcher: switch it in Components."),
    };

    public static string KindName(ModKind k) => k switch
    {
        ModKind.Script => T("Script mod"), ModKind.Cas => T("Create a Sim"), ModKind.Objects => T("Objects"), ModKind.Lighting => T("Lighting"),
        ModKind.Sound => T("Music and sound"), ModKind.Textures => T("Textures"), ModKind.Tuning => T("Tuning"), ModKind.Interface => T("Interface"),
        ModKind.Asi => T("ASI plugin"), ModKind.Sims3Pack => "Sims3Pack", ModKind.World => T("World"), ModKind.Lot => T("Lot"), ModKind.Household => T("Household"), _ => T("Other"),
    };

    ImageSource? _image;
    public ImageSource? Image { get => _image; set { _image = value; Changed(nameof(Image), nameof(Placeholder)); } }
    public Visibility Placeholder => _image is null ? Visibility.Visible : Visibility.Collapsed;

    // the file moved, was renamed or switched (the same row stays in the list)
    public void Update(ModFile f)
    {
        File = f;
        Changed(nameof(Name), nameof(Sub), nameof(IsOn), nameof(TextOpacity), nameof(ToggleVisible), nameof(LockVisible));
    }

    // notes or tags changed
    public void Refresh() => Changed(nameof(Sub), nameof(SubVisible), nameof(TagDots));

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

public sealed record TagDot(string Name, Brush Fill)
{
    public static Brush BrushOf(string hex)
    {
        var c = Convert.ToUInt32(hex.TrimStart('#'), 16);
        return new SolidColorBrush(Windows.UI.Color.FromArgb(255, (byte)(c >> 16), (byte)(c >> 8), (byte)c));
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
