using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace ApexSetup;

// One item the game's own launcher installed (Mods > Installed by the game's launcher): its name, its type and its
// picture, read from the game's cache when the tile first shows
public sealed class InstalledRow(InstalledItem item, string kind, string icon) : INotifyPropertyChanged
{
    public InstalledItem Item { get; } = item;
    public string Name => Item.Name;
    public string Kind { get; } = kind;
    public string KindIcon { get; } = icon;
    public bool Loading { get; set; }
    ImageSource? _image;
    public ImageSource? Image { get => _image; set { _image = value; Changed(nameof(Image)); Changed(nameof(Placeholder)); } }
    public Visibility Placeholder => _image is null ? Visibility.Visible : Visibility.Collapsed;
    public event PropertyChangedEventHandler? PropertyChanged;
    void Changed(string n) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}
