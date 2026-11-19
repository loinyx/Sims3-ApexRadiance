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
    public string Name => File.Name;
    public string Folder => File.Folder;
    public string Kind { get; }
    public string Size { get; }
    public bool IsOn => File.Enabled;
    public double TextOpacity => File.Enabled ? 1 : 0.55;
    public Visibility ToggleVisible => File.Locked ? Visibility.Collapsed : Visibility.Visible;
    public Visibility LockVisible => File.Locked ? Visibility.Visible : Visibility.Collapsed;
    public string LockTip => File.Kind == ModKind.Asi ? T("In the game folder: installed and removed in Components or by hand.") : T("Installed by the launcher: switch it in Components.");
    public string Status { get; private set; } = "";
    public Brush StatusBrush { get; private set; } = Muted;
    public bool NeedsLook { get; }

    // the screen's handler: switch the file and return the moved file, or null when it stayed (the reason shown there)
    public static Func<ModRow, bool, ModFile?>? Toggle;

    public ModRow(ModFile f, bool needsLook)
    {
        File = f;
        NeedsLook = needsLook;
        Kind = KindName(f.Kind);
        Size = f.Size >= 1 << 20 ? $"{f.Size / (1024.0 * 1024):0.#} MB" : $"{Math.Max(1, f.Size / 1024)} KB";
        SetStatus();
    }

    public static string KindName(ModKind k) => k switch
    {
        ModKind.Script => T("Script mod"), ModKind.Cas => T("Create a Sim"), ModKind.Objects => T("Buy and Build"), ModKind.Asi => T("ASI plugin"), _ => T("Package"),
    };

    void SetStatus()
    {
        (Status, StatusBrush) = !File.Enabled ? (T("Off"), Muted)
            : NeedsLook ? (T("Needs a look"), (Brush)Application.Current.Resources["ApexWarningBrush"])
            : File.Locked ? (File.Kind == ModKind.Asi ? T("Game folder") : T("Installed by the launcher"), Muted)
            : (T("On"), (Brush)Application.Current.Resources["ApexSuccessBrush"]);
    }

    static Brush Muted => (Brush)Application.Current.Resources["ApexMutedBrush"];

    public void OnToggled(object sender, RoutedEventArgs e)
    {
        if (sender is not Microsoft.UI.Xaml.Controls.ToggleSwitch t || t.IsOn == File.Enabled) return; // (the binding setting it back)
        if (Toggle?.Invoke(this, t.IsOn) is { } moved) File = moved;
        SetStatus();
        Changed(nameof(IsOn), nameof(TextOpacity), nameof(Status), nameof(StatusBrush));
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    void Changed(params string[] names) { foreach (var n in names) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n)); }
}

public sealed class ThumbItem(ModFile file) : INotifyPropertyChanged
{
    public ModFile File { get; } = file;
    public string Name => File.Name;
    public string Glyph => File.Kind == ModKind.Script ? "" : "";
    public double TextOpacity => File.Enabled ? 1 : 0.55;
    ImageSource? _image;
    public ImageSource? Image { get => _image; set { _image = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Image))); PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Placeholder))); } }
    public Visibility Placeholder => _image is null ? Visibility.Visible : Visibility.Collapsed;
    public event PropertyChangedEventHandler? PropertyChanged;
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
