using System.Diagnostics;
using System.Numerics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// Gallery: the game's pictures in albums (all, favourites, one per household) seen two ways. Albums: rows by day or by
// household, a click opens the viewer. Scrapbook: the album as a board of polaroids that fall in one after another, tape
// sticking and captions writing themselves; they are dragged into place, captions written on, sticky notes added, and
// every album keeps its own board, handwriting, layout and notes (Gallery.cs keeps them in gallery.json).
public sealed partial class MainWindow
{
    const int GalleryScreen = 15;
    GalleryData? _gal;
    string _galAlbum = Gallery.All;
    bool _galBook;
    bool _galByHome;
    int _galLimit = 60;
    List<Picture> _galPics = [];
    List<Save> _galSaves = [];

    static readonly (string Key, string Name, Windows.UI.Color From, Windows.UI.Color To, bool Light)[] Boards =
    [
        ("desk", "Night desk", Rgb(0x1C, 0x19, 0x14), Rgb(0x0F, 0x0E, 0x0C), false),
        ("cork", "Cork", Rgb(0x9A, 0x74, 0x4C), Rgb(0x6E, 0x50, 0x33), false),
        ("paper", "Paper", Rgb(0xF0, 0xEA, 0xDD), Rgb(0xDD, 0xD3, 0xBF), true),
        ("felt", "Green felt", Rgb(0x23, 0x52, 0x39), Rgb(0x12, 0x2F, 0x20), false),
        ("night", "Midnight blue", Rgb(0x1E, 0x27, 0x4A), Rgb(0x0B, 0x0F, 0x22), false),
        ("blush", "Blush", Rgb(0x4A, 0x2C, 0x3B), Rgb(0x24, 0x14, 0x1C), false),
    ];
    static readonly string[] Hands = ["Ink Free", "Segoe Print", "Segoe Script", "Gabriola", "Comic Sans MS", "Bahnschrift", "Segoe UI Variable Display"];
    static readonly (string Key, Windows.UI.Color Color)[] NoteColors =
        [("yellow", Rgb(0xFF, 0xE5, 0x8A)), ("pink", Rgb(0xFF, 0xB8, 0xD2)), ("green", Rgb(0xBF, 0xF0, 0xB8)), ("blue", Rgb(0xB8, 0xDC, 0xFF)), ("lilac", Rgb(0xD9, 0xCC, 0xFF))];
    static readonly Windows.UI.Color[] Tapes = [Windows.UI.Color.FromArgb(0x8C, 0x7F, 0x77, 0xDD), Windows.UI.Color.FromArgb(0x99, 0xE0, 0xA8, 0x4F), Windows.UI.Color.FromArgb(0x8C, 0x7D, 0xBE, 0x9A), Windows.UI.Color.FromArgb(0x80, 0xE8, 0x6F, 0xB8)];

    // the picture Home opens on: the one chosen in the Gallery, else the newest
    string? HomePicture() => (_gal ??= Gallery.Load()).HomePicture is { } p && File.Exists(p) ? p : Screenshots(1).FirstOrDefault();

    void BuildGallery()
    {
        var host = ScreenGallery;
        host.Children.Clear();
        if (_game is null) { host.Children.Add(Heading(T("Gallery"))); host.Children.Add(Muted(T("Choose the game first."))); return; }
        _gal ??= Gallery.Load();
        _galPics = Gallery.List(Docs);
        _galSaves = Saves.List(Docs);
        var page = new Grid { ColumnSpacing = 28 };
        page.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(220) });
        page.ColumnDefinitions.Add(new ColumnDefinition());
        var albums = new ScrollViewer { Content = GalleryAlbums(), VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        page.Children.Add(albums);
        var side = 220.0;
        var main = new StackPanel { Spacing = 18 };
        var picker = new DropDownButton { Content = AlbumName(_galAlbum), Visibility = Visibility.Collapsed,
            Flyout = new Flyout { Content = new ScrollViewer { Content = GalleryAlbums(), MaxHeight = 560, Width = 260 } } };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(picker, T("Albums"));
        main.Children.Add(picker);
        page.SizeChanged += (_, e) =>
        {
            var narrow = e.NewSize.Width < (_galBook ? 900 : 980);
            page.ColumnDefinitions[0].Width = new GridLength(narrow ? 0 : side);
            page.ColumnSpacing = narrow || _galBook ? 0 : 28;
            albums.Visibility = narrow ? Visibility.Collapsed : Visibility.Visible;
            picker.Visibility = narrow ? Visibility.Visible : Visibility.Collapsed;
        };
        main.Children.Add(GalleryHead());
        FullBleed(_galBook);
        if (!_galBook)
        {
            main.Children.Add(GalleryRows());
            Grid.SetColumn(main, 1);
            page.Children.Add(main);
            host.Children.Add(page);
            return;
        }
        var look = Gallery.Look(_gal!, _galAlbum);
        side = 256;
        page.ColumnDefinitions[0].Width = new GridLength(side);
        page.ColumnSpacing = 0;
        page.Height = Math.Max(420, Scroller.ActualHeight);
        page.Loaded += (_, _) => page.Height = Math.Max(420, Scroller.ActualHeight);
        _galPage = page;
        WatchBoardSize();
        albums.Padding = new Thickness(22, 26, 14, 18);
        // the title row, then the board in all the room left: the room measured, the board laid to it
        var head = (FrameworkElement)main.Children[^1];
        main.Children.Clear();
        var column = new Grid();
        column.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        column.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        column.RowDefinitions.Add(new RowDefinition());
        picker.Margin = new Thickness(32, 20, 0, 0);
        column.Children.Add(picker);
        head.Margin = new Thickness(32, 22, 28, 6);
        Grid.SetRow(head, 1);
        column.Children.Add(head);
        var room = new Border();
        Grid.SetRow(room, 2);
        column.Children.Add(room);
        _boardHost = new Border { Child = GalleryBoard(), VerticalAlignment = VerticalAlignment.Top };
        Grid.SetRow(_boardHost, 2);
        column.Children.Add(_boardHost);
        room.SizeChanged += (_, e) =>
        {
            _boardRoom = e.NewSize.Height;
            if (Math.Abs(BoardFits() - BoardHeight) > 8) RefreshBoard();
        };
        var book = new Grid { Background = BoardBrush(look) };
        _boardFrame = book;
        _boardLight = new Border { IsHitTestVisible = false, Background = LightBrush(look) };
        book.Children.Add(_boardLight);
        book.Children.Add(column);
        Grid.SetColumn(book, 1);
        page.Children.Add(book);
        host.Children.Add(page);
    }

    List<Picture> InAlbum() => _galPics.Where(p => Gallery.InAlbum(p, _galAlbum, _gal!, _galSaves)).ToList();
    string AlbumName(string a) => a == Gallery.All ? T("All pictures") : a == Gallery.Favourites ? T("Favourites")
        : Gallery.CustomOf(_gal!, a) is { } c ? c.Name : a;

    // ---------- the albums ----------

    UIElement GalleryAlbums()
    {
        var s = new StackPanel { Spacing = 2 };
        Button Album(string key, string name, int count, Windows.UI.Color dot, string? glyph = null)
        {
            var on = key == _galAlbum;
            var g = new Grid { ColumnSpacing = 12 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(glyph is null ? new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = new SolidColorBrush(dot), VerticalAlignment = VerticalAlignment.Center }
                : new FontIcon { Glyph = glyph, FontSize = 15, Foreground = new SolidColorBrush(dot), VerticalAlignment = VerticalAlignment.Center });
            var t = new TextBlock { Text = name, FontSize = 14, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(t, 1);
            g.Children.Add(t);
            if (count > 0) { var n = new TextBlock { Text = count.ToString(), FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center }; Grid.SetColumn(n, 2); g.Children.Add(n); }
            var b = new Button { Content = g, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, Height = 38, Padding = new Thickness(12, 0, 12, 0), BorderThickness = new Thickness(0),
                Background = on ? new SolidColorBrush(Rgb(0x22, 0x23, 0x2A)) : new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Rgb(0x1C, 0x1D, 0x22));
            b.Click += (_, _) => { _galAlbum = key; _galLimit = 60; BuildGallery(); };
            return b;
        }
        Windows.UI.Color[] dots = [Rgb(0x7D, 0xBE, 0x9A), Rgb(0x57, 0xB6, 0xE8), Rgb(0xE0, 0xA8, 0x4F), Rgb(0xE8, 0x6F, 0xB8), Rgb(0xB4, 0xAE, 0xF2), Rgb(0x8B, 0x8C, 0x96)];
        s.Children.Add(Eyebrow(T("LIBRARY")));
        s.Children.Add(Album(Gallery.All, T("All pictures"), _galPics.Count, Rgb(0x7F, 0x77, 0xDD)));
        s.Children.Add(Album(Gallery.Favourites, T("Favourites"), _galPics.Count(p => Gallery.InAlbum(p, Gallery.Favourites, _gal!, _galSaves)), Rgb(0xF2, 0xC9, 0x4C)));
        var head = Eyebrow(T("HOUSEHOLDS"));
        head.Margin = new Thickness(0, 18, 0, 0);
        s.Children.Add(head);
        // the player's own albums (a trip, a family's story) under the household they belong to, each with its icon;
        // right-click one to rename or delete it, a household to make an album in it
        var homes = _gal!.Custom.ToDictionary(c => c, c => Gallery.HomeOf(c, _gal, _galPics, _galSaves));
        var i = 0;
        foreach (var save in _galSaves)
        {
            var hb = Album(save.Name, save.Name, _galPics.Count(p => Gallery.InAlbum(p, save.Name, _gal!, _galSaves)), dots[i++ % dots.Length]);
            var here = new MenuFlyoutItem { Text = T("New album in {0}", save.Name), Icon = new FontIcon { Glyph = "\uE710" } };
            var home = save.Name;
            here.Click += async (_, _) => { if (await AlbumDialog(null, home) is { } made) { _galAlbum = made.Key; BuildGallery(); } };
            hb.ContextFlyout = new MenuFlyout { Items = { here } };
            s.Children.Add(hb);
            foreach (var c in _gal.Custom.Where(c => string.Equals(homes[c], save.Name, StringComparison.OrdinalIgnoreCase)))
                s.Children.Add(Mine(c, true));
        }
        var loose = _gal.Custom.Where(c => !_galSaves.Any(v => string.Equals(homes[c], v.Name, StringComparison.OrdinalIgnoreCase))).ToList();
        Button Mine(CustomAlbum c, bool inside)
        {
            var b = Album(c.Key, c.Name, _galPics.Count(p => c.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase)), Rgb(0xB4, 0xAE, 0xF2), c.Icon);
            if (inside) { b.Margin = new Thickness(18, 0, 0, 0); b.Height = 34; }
            var menu = new MenuFlyout();
            var rename = new MenuFlyoutItem { Text = T("Rename or change the icon"), Icon = new FontIcon { Glyph = "\uE70F" } };
            var one = c;
            rename.Click += async (_, _) => { if (await AlbumDialog(one) is not null) BuildGallery(); };
            var drop = new MenuFlyoutItem { Text = T("Delete the album"), Icon = new FontIcon { Glyph = "\uE74D" } };
            drop.Click += async (_, _) =>
            {
                if (!await Confirm(T("Delete {0}?", one.Name), T("Only the album goes: its pictures stay in your Screenshots folder and the other albums."), T("Delete"))) return;
                _gal.Custom.Remove(one);
                _gal.Albums.Remove(one.Key);
                Gallery.Save(_gal);
                if (_galAlbum == one.Key) _galAlbum = Gallery.All;
                BuildGallery();
            };
            menu.Items.Add(rename);
            menu.Items.Add(drop);
            b.ContextFlyout = menu;
            return b;
        }
        var mine = new Grid { Margin = new Thickness(0, 18, 0, 0) };
        mine.ColumnDefinitions.Add(new ColumnDefinition());
        mine.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var mineHead = Eyebrow(T("ALBUMS"));
        mineHead.VerticalAlignment = VerticalAlignment.Center;
        mine.Children.Add(mineHead);
        var make = new Button { Content = new FontIcon { Glyph = "\uE710", FontSize = 12 }, Width = 28, Height = 28, Padding = new Thickness(0), BorderThickness = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(make, T("New album"));
        ToolTipService.SetToolTip(make, T("New album"));
        make.Click += async (_, _) => { if (await AlbumDialog(null) is { } made) { _galAlbum = made.Key; BuildGallery(); } };
        Grid.SetColumn(make, 1);
        mine.Children.Add(make);
        s.Children.Add(mine);
        foreach (var c in loose) s.Children.Add(Mine(c, false));
        if (_gal.Custom.Count == 0)
        {
            var hint = new HyperlinkButton { Content = T("+ an album of your own, like a trip"), FontSize = 13, Padding = new Thickness(12, 4, 0, 4) };
            hint.Click += async (_, _) => { if (await AlbumDialog(null) is { } made) { _galAlbum = made.Key; BuildGallery(); } };
            s.Children.Add(hint);
        }
        var note = Muted(T("Pictures go to the household saved right after they were taken; change one in its viewer. Albums sit inside their household."));
        note.TextWrapping = TextWrapping.Wrap;
        note.FontSize = 12;
        note.Margin = new Thickness(12, 18, 0, 0);
        s.Children.Add(note);
        return s;
    }

    // the title, what it holds, and the controls of the open view; the switch between the two views on the right
    UIElement GalleryHead()
    {
        var g = new Grid { ColumnSpacing = 12 };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var pics = InAlbum();
        var look = Gallery.Look(_gal!, _galAlbum);
        var titles = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        if (Gallery.CustomOf(_gal!, _galAlbum) is { } own) name.Children.Add(new FontIcon { Glyph = own.Icon, FontSize = _galBook ? 28 : 22, Foreground = new SolidColorBrush(Rgb(0xB4, 0xAE, 0xF2)), VerticalAlignment = VerticalAlignment.Center });
        _boardTitle = _galBook
            ? new TextBlock { Text = AlbumName(_galAlbum), FontFamily = new FontFamily(look.Font), FontSize = 40, FontWeight = FontWeights.SemiBold, Foreground = Ink(look) }
            : new TextBlock { Text = AlbumName(_galAlbum), FontSize = 26, FontWeight = FontWeights.SemiBold };
        name.Children.Add(_boardTitle);
        titles.Children.Add(name);
        var hint = Muted(_galBook ? T("Drag to travel the timeline · click a picture to open it")
            : pics.Count == 1 ? T("1 picture · newest first") : T("{0} pictures · newest first", pics.Count));
        if (_galBook) { hint.Foreground = Ink(look); hint.Opacity = 0.7; hint.TextWrapping = TextWrapping.NoWrap; hint.TextTrimming = TextTrimming.CharacterEllipsis; _boardTitle.TextTrimming = TextTrimming.CharacterEllipsis; }
        titles.Children.Add(hint);
        g.Children.Add(titles);

        var tools = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        if (_galBook)
        {
            T Round<T>(T b, string glyph, string name) where T : Button
            {
                b.Content = new FontIcon { Glyph = glyph, FontSize = 16 };
                b.Width = 44; b.Height = 44; b.Padding = new Thickness(0); b.CornerRadius = new CornerRadius(22);
                ToolTipService.SetToolTip(b, name);
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
                return b;
            }
            var custom = Round(new Button { Flyout = CustomizeFlyout(look) }, "\uE790", T("Customize"));
            var add = Round(new Button(), "\uE70B", T("Add a note"));
            var menu = new MenuFlyout();
            foreach (var (key, color) in NoteColors)
            {
                var item = new MenuFlyoutItem { Text = T(char.ToUpperInvariant(key[0]) + key[1..]), Icon = new FontIcon { Glyph = "", Foreground = new SolidColorBrush(color) } };
                item.Click += (_, _) => AddNote(key);
                menu.Items.Add(item);
            }
            add.Flyout = menu;
            tools.Children.Add(custom);
            tools.Children.Add(add);
            var shuffle = Round(new Button(), "\uE8B1", T("Shuffle"));
            shuffle.Click += (_, _) => ShuffleBoard();
            tools.Children.Add(shuffle);
        }
        else
        {
            var by = new ComboBox { MinWidth = 150 };
            by.Items.Add(T("By day"));
            by.Items.Add(T("By household"));
            by.SelectedIndex = _galByHome ? 1 : 0;
            by.SelectionChanged += (_, _) => { _galByHome = by.SelectedIndex == 1; BuildGallery(); };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(by, T("Group"));
            tools.Children.Add(by);
            tools.Children.Add(Button(T("Screenshots folder"), () => OpenFolder(Gallery.Folder(Docs))));
        }
        Grid.SetColumn(tools, 1);
        g.Children.Add(tools);
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        g.SizeChanged += (_, e) =>
        {
            var low = e.NewSize.Width < (_galBook ? 640 : 1150);
            Grid.SetRow(tools, low ? 1 : 0);
            Grid.SetColumn(tools, low ? 0 : 1);
            Grid.SetColumnSpan(tools, low ? 3 : 1);
            tools.HorizontalAlignment = HorizontalAlignment.Left;
            tools.Margin = new Thickness(0, low ? 12 : 0, 0, 0);
        };
        var sw = ViewSwitch();
        Grid.SetColumn(sw, 2);
        g.Children.Add(sw);
        return g;
    }

    // Albums or Scrapbook: two buttons on a track, the pill sliding under the chosen one
    FrameworkElement ViewSwitch()
    {
        var track = new Grid { Padding = new Thickness(3), CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), VerticalAlignment = VerticalAlignment.Center };
        track.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(118) });
        track.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(118) });
        var pill = new Border { CornerRadius = new CornerRadius(7), Background = new SolidColorBrush(Rgb(0x2A, 0x2B, 0x31)), Width = 118, HorizontalAlignment = HorizontalAlignment.Left,
            TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(320) }, Translation = new Vector3(_galBook ? 0 : 118, 0, 0) };
        Grid.SetColumnSpan(pill, 2);
        track.Children.Add(pill);
        DispatcherQueue.TryEnqueue(() => pill.Translation = new Vector3(_galBook ? 118 : 0, 0, 0));
        Button Side(string glyph, string text, bool book)
        {
            var c = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
            c.Children.Add(new FontIcon { Glyph = glyph, FontSize = 14 });
            c.Children.Add(new TextBlock { Text = text, FontSize = 13 });
            var b = new Button { Content = c, Height = 34, HorizontalAlignment = HorizontalAlignment.Stretch, BorderThickness = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                Foreground = _galBook == book ? new SolidColorBrush(Microsoft.UI.Colors.White) : Res("ApexSoftTextBrush") };
            b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, text);
            b.Click += (_, _) => { if (_galBook == book) return; _galBook = book; pill.Translation = new Vector3(book ? 118 : 0, 0, 0); DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, BuildGallery); };
            return b;
        }
        var albums = Side("", T("Albums"), false);
        var book = Side("", T("Scrapbook"), true);
        Grid.SetColumn(book, 1);
        track.Children.Add(albums);
        track.Children.Add(book);
        return track;
    }

    // ---------- Albums: rows by day or by household ----------

    UIElement GalleryRows()
    {
        var s = new StackPanel { Spacing = 26 };
        var pics = InAlbum();
        if (pics.Count == 0) { s.Children.Add(Muted(_galAlbum == Gallery.Favourites ? T("No favourites yet: open a picture and add it.") : T("No pictures here yet."))); return s; }
        var shown = pics.Take(_galLimit).ToList();
        var groups = _galByHome
            ? shown.GroupBy(p => Gallery.HouseholdOf(p, _gal!, _galSaves) ?? T("Not sorted yet")).Select(x => (Key: x.Key, List: x.ToList()))
            : shown.GroupBy(p => p.Taken.Date).Select(x => (Key: x.Key.ToString("D"), List: x.ToList()));
        var index = 0;
        foreach (var (key, list) in groups)
        {
            var block = new StackPanel { Spacing = 10 };
            var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            head.Children.Add(new TextBlock { Text = key, FontSize = 17, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            head.Children.Add(new Border { CornerRadius = new CornerRadius(10), Padding = new Thickness(8, 1, 8, 2), Background = Res("ApexPillBrush"), VerticalAlignment = VerticalAlignment.Center,
                Child = new TextBlock { Text = list.Count.ToString(), FontSize = 12, Foreground = Res("ApexSoftTextBrush") } });
            if (!_galByHome && string.Join(" · ", list.Select(p => Gallery.HouseholdOf(p, _gal!, _galSaves)).OfType<string>().Distinct()) is { Length: > 0 } homes)
                head.Children.Add(new TextBlock { Text = homes, FontSize = 13, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
            block.Children.Add(head);
            var wrap = new CardGrid { MinItemWidth = 250, Spacing = 10, ChildrenTransitions = new TransitionCollection { new EntranceThemeTransition { IsStaggeringEnabled = true, FromVerticalOffset = 30 } } };
            foreach (var p in list)
            {
                var at = pics.IndexOf(p);
                wrap.Children.Add(Tile(p, () => OpenViewer(pics, at)));
                index++;
            }
            block.Children.Add(wrap);
            s.Children.Add(block);
        }
        if (pics.Count > shown.Count)
        {
            var more = Button(T("Show {0} more", Math.Min(60, pics.Count - shown.Count)), () => { _galLimit += 60; BuildGallery(); });
            more.HorizontalAlignment = HorizontalAlignment.Center;
            s.Children.Add(more);
        }
        return s;
    }

    // a picture in the grid: it fades in once read, rises and grows a touch under the pointer with its time showing, a star
    // when it is a favourite
    UIElement Tile(Picture p, Action open)
    {
        var g = new Grid { CornerRadius = new CornerRadius(12), Background = Res("ApexPillBrush") };
        var image = new Image { Stretch = Stretch.UniformToFill, Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(320) } };
        var bmp = new BitmapImage { DecodePixelWidth = 480 };
        bmp.ImageOpened += (_, _) => image.Opacity = 1;
        bmp.UriSource = new Uri(p.Path);
        image.Source = bmp;
        g.Children.Add(image);
        var when = new Border { VerticalAlignment = VerticalAlignment.Bottom, Padding = new Thickness(12, 18, 12, 9), Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(180) },
            Background = new LinearGradientBrush { StartPoint = new(0, 0), EndPoint = new(0, 1), GradientStops = { new GradientStop { Color = Windows.UI.Color.FromArgb(0, 0, 0, 0), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(0xB3, 0, 0, 0), Offset = 1 } } },
            Child = new TextBlock { Text = p.Taken.ToString("t") + "  ·  " + (Gallery.HouseholdOf(p, _gal!, _galSaves) ?? ""), FontSize = 12, Foreground = new SolidColorBrush(Microsoft.UI.Colors.White), TextTrimming = TextTrimming.CharacterEllipsis } };
        g.Children.Add(when);
        if (Gallery.Peek(_gal!, p.Path).Favourite)
            g.Children.Add(new Border { Width = 26, Height = 26, CornerRadius = new CornerRadius(13), Margin = new Thickness(8), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB3, 0x0B, 0x0C, 0x0E)), Child = new FontIcon { Glyph = "", FontSize = 13, Foreground = new SolidColorBrush(Rgb(0xF2, 0xC9, 0x4C)) } });
        var b = new Button { Content = g, Padding = new Thickness(0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(12), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), Shadow = new ThemeShadow(), Translation = new Vector3(0, 0, 8) };
        b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, Path.GetFileNameWithoutExtension(p.Path));
        // (16:9, as wide as its column)
        b.SizeChanged += (_, e) => { if (Math.Abs(g.Height - e.NewSize.Width * 9 / 16) > 1) g.Height = e.NewSize.Width * 9 / 16; b.CenterPoint = new Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0); };
        b.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(220) };
        b.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(220) };
        b.PointerEntered += (_, _) => { b.Translation = new Vector3(0, -6, 30); b.Scale = new Vector3(1.025f, 1.025f, 1); when.Opacity = 1; };
        b.PointerExited += (_, _) => { b.Translation = new Vector3(0, 0, 8); b.Scale = Vector3.One; when.Opacity = 0; };
        b.Click += (_, _) => open();
        return b;
    }

    // a storyboard step: one property of one element to a value, over a time, with an ease
    static void Tween(Storyboard sb, DependencyObject target, string property, double to, int ms, EasingFunctionBase? ease = null, int begin = 0)
    {
        var a = new DoubleAnimation { To = to, Duration = TimeSpan.FromMilliseconds(ms), EasingFunction = ease, BeginTime = TimeSpan.FromMilliseconds(begin) };
        Storyboard.SetTarget(a, target);
        Storyboard.SetTargetProperty(a, property);
        sb.Children.Add(a);
    }

    // a card's landing: from above, turned and a little large, it falls with a bounce, its turn settling with a spring
    static CompositeTransform Land(FrameworkElement el, double angle, int delay, double fromX)
    {
        var t = _boardCalm ? new CompositeTransform { Rotation = angle } : new CompositeTransform { TranslateY = -260, TranslateX = fromX, Rotation = angle + 26, ScaleX = 1.2, ScaleY = 1.2 };
        el.RenderTransform = t;
        el.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);
        if (_boardCalm) return t;
        el.Opacity = 0;
        var sb = new Storyboard { BeginTime = TimeSpan.FromMilliseconds(delay) };
        Tween(sb, t, "TranslateY", 0, 900, new BounceEase { EasingMode = EasingMode.EaseOut, Bounces = 2, Bounciness = 3.2 });
        Tween(sb, t, "TranslateX", 0, 760, new CubicEase { EasingMode = EasingMode.EaseOut });
        Tween(sb, t, "Rotation", angle, 1250, new ElasticEase { EasingMode = EasingMode.EaseOut, Oscillations = 2, Springiness = 5 });
        Tween(sb, t, "ScaleX", 1, 640, new CubicEase { EasingMode = EasingMode.EaseOut });
        Tween(sb, t, "ScaleY", 1, 640, new CubicEase { EasingMode = EasingMode.EaseOut });
        Tween(sb, el, "Opacity", 1, 240);
        sb.Begin();
        return t;
    }

    // a quick turn of a card (hover, shuffle)
    static void Turn(CompositeTransform t, double to, int ms = 320, EasingFunctionBase? ease = null)
    {
        var sb = new Storyboard();
        Tween(sb, t, "Rotation", to, ms, ease ?? new CubicEase { EasingMode = EasingMode.EaseOut });
        sb.Begin();
    }

    // the tape's colour: bright, or the board's own hue made paler
    Brush TapeBrush(AlbumLook look, int order)
    {
        if (look.Tape == "match") { var c = Shade(BoardColor(look), 0.45); return new SolidColorBrush(Windows.UI.Color.FromArgb(0xB8, c.R, c.G, c.B)); }
        return new SolidColorBrush(Tapes[order % Tapes.Length]);
    }

    // ---------- Scrapbook ----------

    // The album as a timeline: the oldest pictures on the left, the newest on the right, a line through the board with each
    // day marked; it scrolls sideways (the mouse wheel too) and opens on the newest. Every album keeps its own colour (one
    // hue fading softly darker), handwriting and frames.
    const double Step = 270;
    double BoardHeight = 660, _boardScale = 1;
    Canvas? _boardCanvas;
    Panel? _boardFrame;
    ScrollViewer? _boardScroll;
    readonly List<(Picture Pic, FrameworkElement Card, int Index)> _boardCards = [];
    static readonly string[] Frames = ["polaroid", "rounded", "stamp", "film", "circle"];
    static readonly (string Hex, string Name)[] QuickColors = [("#1C1914", "Night desk"), ("#9A744C", "Cork"), ("#E9E1CF", "Paper"), ("#2C5E43", "Green felt"), ("#24305C", "Midnight blue"), ("#5A3346", "Blush")];

    static Windows.UI.Color Hex(string? hex, Windows.UI.Color fallback)
    {
        if (hex is not { Length: 7 } || hex[0] != '#') return fallback;
        try { return Windows.UI.Color.FromArgb(255, Convert.ToByte(hex[1..3], 16), Convert.ToByte(hex[3..5], 16), Convert.ToByte(hex[5..7], 16)); }
        catch (FormatException) { return fallback; }
    }
    static string Hex(Windows.UI.Color c) => $"#{c.R:X2}{c.G:X2}{c.B:X2}";
    static Windows.UI.Color Shade(Windows.UI.Color c, double by) =>
        by >= 0 ? Windows.UI.Color.FromArgb(255, (byte)(c.R + (255 - c.R) * by), (byte)(c.G + (255 - c.G) * by), (byte)(c.B + (255 - c.B) * by))
                : Windows.UI.Color.FromArgb(255, (byte)(c.R * (1 + by)), (byte)(c.G * (1 + by)), (byte)(c.B * (1 + by)));
    static bool IsLight(Windows.UI.Color c) => (0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B) / 255 > 0.6;

    // the album's one colour: its own, else the colour of the board it had before colours
    static Windows.UI.Color BoardColor(AlbumLook look) => Hex(look.Color, Boards.FirstOrDefault(b => b.Key == look.Background) is { Key: not null } b ? b.From : Boards[0].From);

    // one hue, a touch lighter at the top, softly darker towards the bottom
    static Brush BoardBrush(AlbumLook look)
    {
        var c = BoardColor(look);
        return new LinearGradientBrush { StartPoint = new(0.2, 0), EndPoint = new(0.8, 1), GradientStops =
            { new GradientStop { Color = Shade(c, 0.08), Offset = 0 }, new GradientStop { Color = c, Offset = 0.45 }, new GradientStop { Color = Shade(c, -0.42), Offset = 1 } } };
    }
    static Brush Ink(AlbumLook look) => new SolidColorBrush(IsLight(BoardColor(look)) ? Rgb(0x3A, 0x34, 0x2A) : Rgb(0xE8, 0xE2, 0xD4));

    // the board's colour on a wheel, a few ready ones, the handwriting and the frames
    Flyout CustomizeFlyout(AlbumLook look)
    {
        var panel = new StackPanel { Spacing = 14, Width = 330 };
        var rebuild = false;
        panel.Children.Add(new TextBlock { Text = T("Board colour"), FontWeight = FontWeights.SemiBold });
        var wheel = new ColorPicker
        {
            ColorSpectrumShape = ColorSpectrumShape.Ring, IsAlphaEnabled = false, IsMoreButtonVisible = false, IsColorChannelTextInputVisible = false,
            IsHexInputVisible = true, IsColorPreviewVisible = false, IsColorSliderVisible = true, Color = BoardColor(look),
        };
        wheel.ColorChanged += (_, e) => { look.Color = Hex(e.NewColor); if (_boardFrame is not null) _boardFrame.Background = BoardBrush(look); Changed(); };
        void Changed() { rebuild = true; RefreshBoard(); }
        panel.Children.Add(wheel);
        var quick = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var (hex, name) in QuickColors)
        {
            var c = Hex(hex, Boards[0].From);
            var b = new Button { Width = 30, Height = 30, Padding = new Thickness(0), CornerRadius = new CornerRadius(15), BorderThickness = new Thickness(1), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x40, 255, 255, 255)),
                Background = new SolidColorBrush(c) };
            b.Resources["ButtonBackgroundPointerOver"] = b.Background;
            ToolTipService.SetToolTip(b, T(name));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, T(name));
            b.Click += (_, _) => wheel.Color = c;
            quick.Children.Add(b);
        }
        panel.Children.Add(quick);
        panel.Children.Add(new TextBlock { Text = T("Handwriting"), FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 6, 0, 0) });
        var hand = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
        foreach (var f in Hands) hand.Items.Add(new ComboBoxItem { Content = f, FontFamily = new FontFamily(f), FontSize = 18 });
        hand.SelectedIndex = Math.Max(0, Array.IndexOf(Hands, look.Font));
        hand.SelectionChanged += (_, _) => { look.Font = Hands[Math.Max(0, hand.SelectedIndex)]; Changed(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(hand, T("Handwriting"));
        panel.Children.Add(hand);
        panel.Children.Add(new TextBlock { Text = T("Frames"), FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 6, 0, 0) });
        var frames = new Grid { ColumnSpacing = 6 };
        string[] names = [T("Polaroid"), T("Rounded"), T("Stamp"), T("Film"), T("Circle")];
        var buttons = new List<ToggleButton>();
        for (int i = 0; i < Frames.Length; i++)
        {
            frames.ColumnDefinitions.Add(new ColumnDefinition());
            var key = Frames[i];
            var t = new ToggleButton { Content = FrameIcon(key), IsChecked = look.Frame == key, HorizontalAlignment = HorizontalAlignment.Stretch, Height = 52, Padding = new Thickness(0) };
            ToolTipService.SetToolTip(t, names[i]);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(t, names[i]);
            t.Click += (_, _) => { look.Frame = key; foreach (var o in buttons) o.IsChecked = o == t; Changed(); };
            Grid.SetColumn(t, i);
            frames.Children.Add(t);
            buttons.Add(t);
        }
        panel.Children.Add(frames);
        StackPanel Choice(string title, (string Key, string Name)[] options, Func<string> now, Action<string> set)
        {
            var box = new StackPanel { Spacing = 6, Margin = new Thickness(0, 6, 0, 0) };
            box.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold });
            var row = new Grid { ColumnSpacing = 6 };
            var all = new List<ToggleButton>();
            for (int i = 0; i < options.Length; i++)
            {
                row.ColumnDefinitions.Add(new ColumnDefinition());
                var (key, name) = options[i];
                var t = new ToggleButton { Content = name, IsChecked = now() == key, HorizontalAlignment = HorizontalAlignment.Stretch };
                t.Click += (_, _) => { set(key); foreach (var o in all) o.IsChecked = o == t; Changed(); };
                Grid.SetColumn(t, i);
                row.Children.Add(t);
                all.Add(t);
            }
            box.Children.Add(row);
            return box;
        }
        panel.Children.Add(Choice(T("Light"), [("soft", T("Soft")), ("spot", T("Spotlight")), ("flat", T("Flat"))], () => look.Light, v => look.Light = v));
        panel.Children.Add(Choice(T("Tape"), [("colour", T("Colourful")), ("match", T("Matching")), ("none", T("None"))], () => look.Tape, v => look.Tape = v));
        Slider Slide(string title, double min, double max, double value, string left, string right, Action<double> set)
        {
            var s = new Slider { Header = title, Minimum = min, Maximum = max, StepFrequency = 0.05, Value = value, Margin = new Thickness(0, 6, 0, 0) };
            ToolTipService.SetToolTip(s, left + "  ←→  " + right);
            s.ValueChanged += (_, e) => { set(e.NewValue); Changed(); };
            return s;
        }
        panel.Children.Add(Slide(T("Picture size"), 0.75, 1.3, look.Size, T("Small"), T("Large"), v => look.Size = v));
        panel.Children.Add(Slide(T("Tilt: tidy to messy"), 0, 1.6, look.Tilt, T("Tidy"), T("Messy"), v => look.Tilt = v));
        var fly = new Flyout { Content = new ScrollViewer { Content = panel, MaxHeight = 680, Padding = new Thickness(0, 0, 14, 0) }, Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedRight };
        fly.Closed += (_, _) => { Gallery.Save(_gal!); if (rebuild) RefreshBoard(page: true); };
        return fly;
    }

    // a small drawing of each frame for its button
    static UIElement FrameIcon(string key)
    {
        var paper = new SolidColorBrush(Rgb(0xF2, 0xEF, 0xE8));
        var photo = new SolidColorBrush(Rgb(0x6E, 0x8F, 0xB8));
        return key switch
        {
            "rounded" => new Border { Width = 30, Height = 24, CornerRadius = new CornerRadius(7), Background = photo },
            "stamp" => new Grid { Width = 28, Height = 30, Children = { new Microsoft.UI.Xaml.Shapes.Rectangle { Fill = paper, Stroke = new SolidColorBrush(Rgb(0x9A, 0x90, 0x7E)), StrokeDashArray = [1.5, 1.2], StrokeThickness = 2 },
                new Border { Margin = new Thickness(5), Background = photo } } },
            "film" => new Border { Width = 32, Height = 26, Background = new SolidColorBrush(Rgb(0x16, 0x16, 0x16)), Padding = new Thickness(3, 6, 3, 6), Child = new Border { Background = photo } },
            "circle" => new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 26, Height = 26, Fill = photo, Stroke = paper, StrokeThickness = 2 },
            _ => new Border { Width = 26, Height = 30, Background = paper, Padding = new Thickness(3, 3, 3, 9), Child = new Border { Background = photo } },
        };
    }

    Border? _boardHost;
    static bool _boardCalm;
    double? _boardKeep;
    int _boardRedraw;
    void RefreshBoard(bool page = false)
    {
        var mine = ++_boardRedraw;
        After(page ? 0 : 120, () =>
        {
            if (mine != _boardRedraw || !_galBook) return;
            _boardKeep = _boardScroll is { ScrollableWidth: > 0 } sv ? sv.HorizontalOffset / sv.ScrollableWidth : null;
            _boardCalm = true;
            try
            {
                if (page || _boardHost is null) { BuildGallery(); return; }
                _boardHost.Child = GalleryBoard();
                var look = Gallery.Look(_gal!, _galAlbum);
                if (_boardLight is not null) _boardLight.Background = LightBrush(look);
                if (_boardTitle is not null) { _boardTitle.FontFamily = new FontFamily(look.Font); _boardTitle.Foreground = Ink(look); }
            }
            finally { _boardCalm = false; }
        });
    }

    Border? _boardLight;
    TextBlock? _boardTitle;

    // the light: an even fade, a lit middle with darker edges, or nothing over the colour
    static Brush? LightBrush(AlbumLook look) => look.Light == "flat" ? null : new RadialGradientBrush
    {
        Center = new(0.5, 0.42), GradientOrigin = new(0.5, 0.35), RadiusX = 0.75, RadiusY = 0.85, GradientStops =
        {
            new GradientStop { Color = look.Light == "spot" ? Windows.UI.Color.FromArgb(0x1C, 255, 255, 255) : Windows.UI.Color.FromArgb(0, 0, 0, 0), Offset = 0 },
            new GradientStop { Color = Windows.UI.Color.FromArgb(0, 0, 0, 0), Offset = 0.55 },
            new GradientStop { Color = Windows.UI.Color.FromArgb((byte)(look.Light == "spot" ? 0x8C : 0x40), 0, 0, 0), Offset = 1 },
        },
    };

    double _boardRoom;
    Grid? _galPage;
    double BoardFits() => Math.Clamp((_boardRoom > 0 ? _boardRoom : Scroller.ActualHeight - 120) - 62, 170, 1400);

    void FullBleed(bool on)
    {
        Scroller.Padding = on ? new Thickness(0) : new Thickness(32, 28, 32, 0);
        Screens.Margin = on ? new Thickness(0) : new Thickness(0, 0, 0, 32);
        Scroller.VerticalScrollBarVisibility = on ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto;
    }

    bool _boardWatched;
    int _boardResize;
    void WatchBoardSize()
    {
        if (_boardWatched) return;
        _boardWatched = true;
        // (the page follows the window's height; the room under the title then tells the board its new height)
        Scroller.SizeChanged += (_, _) =>
        {
            if (!_galBook || ScreenGallery.Visibility != Visibility.Visible || _galPage is null) return;
            var mine = ++_boardResize;
            After(200, () => { if (mine == _boardResize && _galPage is not null) _galPage.Height = Math.Max(420, Scroller.ActualHeight); });
        };
    }

    UIElement GalleryBoard()
    {
        var look = Gallery.Look(_gal!, _galAlbum);
        // (boards laid out before the timeline get the timeline's layout once)
        if (look.Layout < 2) { look.Places.Clear(); look.Layout = 2; look.Background = "desk"; }
        var ink = Ink(look);
        // as tall as the window allows, the pictures sized with it
        BoardHeight = BoardFits();
        // (pictures and notes placed on a board of another height keep their place on this one)
        if (look.Height > 0 && Math.Abs(look.Height - BoardHeight) > 1)
        {
            var k = BoardHeight / look.Height;
            foreach (var pl in look.Places.Values) { pl.X *= k; pl.Y *= k; }
            foreach (var nt in look.Notes) { nt.X *= k; nt.Y *= k; }
        }
        look.Height = BoardHeight;
        var s = _boardScale = BoardHeight / 660;
        var mid = BoardHeight / 2;
        // the newest sixty, laid oldest to newest along a gentle wave around the line, a breath of room where a day begins
        var pics = InAlbum().Take(60).Reverse().ToList();
        var xs = new double[pics.Count];
        var days = new List<(double X, DateTime Day)>();
        var x = 110 * s;
        double last = 0;
        for (int i = 0; i < pics.Count; i++)
        {
            if (i == 0 || pics[i].Taken.Date != pics[i - 1].Taken.Date) { if (i > 0) x += 90 * s; days.Add((x, pics[i].Taken.Date)); }
            xs[i] = x;
            last = ((i % 4 == 0 ? 290 : 230) * s * look.Size + 24);
            x += last * 0.8;
        }
        var width = Math.Max(1100, pics.Count == 0 ? 0 : xs[^1] + last + 110 * s);
        var canvas = new Canvas { Width = width, Height = BoardHeight, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        _boardCanvas = canvas;
        _boardCards.Clear();
        // the line of time, and a mark with its date where each day begins
        var lineColor = IsLight(BoardColor(look)) ? Windows.UI.Color.FromArgb(0x55, 0x3A, 0x34, 0x2A) : Windows.UI.Color.FromArgb(0x40, 0xE8, 0xE2, 0xD4);
        var line = new Border { Width = width - 120, Height = 2, Background = new SolidColorBrush(lineColor), CornerRadius = new CornerRadius(1), Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(600) } };
        Canvas.SetLeft(line, 60); Canvas.SetTop(line, mid);
        canvas.Children.Add(line);
        After(60, () => line.Opacity = 1);
        var pill = new SolidColorBrush(Windows.UI.Color.FromArgb(0xCC, BoardColor(look).R, BoardColor(look).G, BoardColor(look).B));
        foreach (var (dx, day) in days)
        {
            var dot = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 12, Height = 12, Fill = ink };
            Canvas.SetLeft(dot, dx - 6); Canvas.SetTop(dot, mid - 5); Canvas.SetZIndex(dot, 800);
            var label = new Border { Background = pill, CornerRadius = new CornerRadius(12), Padding = new Thickness(10, 0, 10, 2),
                Child = new TextBlock { Text = day.ToString("d MMM"), FontFamily = new FontFamily(look.Font), FontSize = 22 * Math.Max(0.85, s), Foreground = ink } };
            Canvas.SetLeft(label, dx + 12); Canvas.SetTop(label, mid + 8); Canvas.SetZIndex(label, 800);
            canvas.Children.Add(dot);
            canvas.Children.Add(label);
        }
        foreach (var under in canvas.Children) under.IsHitTestVisible = false;
        if (pics.Count == 0)
        {
            var empty = new TextBlock { Text = T("no pictures here yet"), FontFamily = new FontFamily(look.Font), FontSize = 30, Foreground = ink };
            Canvas.SetLeft(empty, 140); Canvas.SetTop(empty, mid - 90);
            canvas.Children.Add(empty);
        }
        var chapters = _galAlbum.StartsWith(Gallery.Mine, StringComparison.Ordinal) ? []
            : _gal!.Custom.Where(c => pics.Any(pc => c.Pictures.Contains(pc.Path, StringComparer.OrdinalIgnoreCase))).ToList();
        var roof = 14 + Math.Min(2, chapters.Count) * 40;
        var rnd = new Random(_galAlbum.GetHashCode());
        for (int i = 0; i < pics.Count; i++)
        {
            if (!look.Places.TryGetValue(pics[i].Path, out var place))
            {
                var phase = i * 1.1;
                var h = ((i % 4 == 0 ? 194 : 154) * s * look.Size) + 70;
                place = new Placed
                {
                    X = xs[i] + rnd.Next(-12, 12),
                    Y = Math.Clamp(mid - h / 2 + Math.Sin(phase) * mid * 0.5, roof, Math.Max(roof, BoardHeight - h - 14)),
                    Angle = Math.Round(-Math.Cos(phase) * 5 + rnd.NextDouble() * 3 - 1.5, 1),
                };
                look.Places[pics[i].Path] = place;
            }
            // (the newest fall in first, the ones further back already lie there when scrolled to)
            var delay = 120 + Math.Min(pics.Count - 1 - i, 10) * 130;
            var card = Framed(pics[i], place, look, ink, i, delay, pics);
            canvas.Children.Add(card);
            _boardCards.Add((pics[i], card, i));
        }
        var lanes = new List<double>();
        foreach (var c in chapters)
        {
            var spots = pics.Select((pc, k) => (pc, k)).Where(t => c.Pictures.Contains(t.pc.Path, StringComparer.OrdinalIgnoreCase))
                .Select(t => (X: look.Places[t.pc.Path].X, W: (t.k % 4 == 0 ? 290 : 230) * s * look.Size + 24)).ToList();
            var from = spots.Min(t => t.X);
            var to = Math.Max(spots.Max(t => t.X + t.W), from + 200);
            var lane = lanes.FindIndex(end => end < from - 16);
            if (lane < 0) { lane = lanes.Count; lanes.Add(0); }
            lanes[lane] = to;
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            row.Children.Add(new FontIcon { Glyph = c.Icon, FontSize = 15, Foreground = ink, VerticalAlignment = VerticalAlignment.Center });
            row.Children.Add(new TextBlock { Text = c.Name, FontFamily = new FontFamily(look.Font), FontSize = 19, Foreground = ink, VerticalAlignment = VerticalAlignment.Center });
            row.Children.Add(new TextBlock { Text = spots.Count.ToString(), FontSize = 12, Foreground = ink, Opacity = 0.65, VerticalAlignment = VerticalAlignment.Center });
            var ribbon = new Button { Content = row, Width = to - from, Height = 34, Padding = new Thickness(16, 0, 16, 0), CornerRadius = new CornerRadius(17), HorizontalContentAlignment = HorizontalAlignment.Left,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x40, 0x7F, 0x77, 0xDD)), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x99, 0xB4, 0xAE, 0xF2)), BorderThickness = new Thickness(1),
                Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(400) } };
            ribbon.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Windows.UI.Color.FromArgb(0x70, 0x7F, 0x77, 0xDD));
            ribbon.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Windows.UI.Color.FromArgb(0x90, 0x7F, 0x77, 0xDD));
            ribbon.Resources["ButtonBorderBrushPointerOver"] = new SolidColorBrush(Rgb(0xB4, 0xAE, 0xF2));
            ToolTipService.SetToolTip(ribbon, T("Open the album {0}", c.Name));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(ribbon, T("Open the album {0}", c.Name));
            var key = c.Key;
            ribbon.Click += (_, _) => { _galAlbum = key; BuildGallery(); };
            Canvas.SetLeft(ribbon, from);
            Canvas.SetTop(ribbon, 10 + lane * 40);
            Canvas.SetZIndex(ribbon, 950);
            canvas.Children.Add(ribbon);
            if (_boardCalm) ribbon.Opacity = 1; else After(700, () => ribbon.Opacity = 1);
        }
        var n = 0;
        foreach (var note in look.Notes) canvas.Children.Add(Sticky(note, look, n++));

        var scroll = new ScrollViewer { Content = canvas, HorizontalScrollMode = ScrollMode.Enabled, HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden,
            VerticalScrollMode = ScrollMode.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled, ZoomMode = ZoomMode.Disabled };
        _boardScroll = scroll;
        // the mouse wheel moves along the timeline
        scroll.AddHandler(UIElement.PointerWheelChangedEvent, new PointerEventHandler((_, e) =>
        {
            var d = e.GetCurrentPoint(scroll).Properties.MouseWheelDelta;
            scroll.ChangeView(scroll.HorizontalOffset - d * 1.4, null, null, false);
            e.Handled = true;
        }), true);
        var opened = false;
        var keep = _boardKeep;
        _boardKeep = null;
        scroll.LayoutUpdated += (_, _) =>
        {
            if (opened || scroll.ScrollableWidth <= 0) return;
            opened = true;
            scroll.ChangeView((keep ?? 1) * scroll.ScrollableWidth, null, null, true);
        };
        var panning = false;
        double grabX = 0, grabOffset = 0;
        canvas.PointerPressed += (_, e) =>
        {
            if (e.OriginalSource != canvas) return;
            panning = canvas.CapturePointer(e.Pointer);
            grabX = e.GetCurrentPoint(scroll).Position.X;
            grabOffset = scroll.HorizontalOffset;
            e.Handled = true;
        };
        canvas.PointerMoved += (_, e) => { if (panning) scroll.ChangeView(grabOffset - (e.GetCurrentPoint(scroll).Position.X - grabX), null, null, true); };
        canvas.PointerReleased += (_, e) => { panning = false; canvas.ReleasePointerCapture(e.Pointer); };
        canvas.PointerCaptureLost += (_, _) => panning = false;
        var inside = new Grid();
        inside.Children.Add(scroll);
        Border Edge(bool left) => new()
        {
            Width = 90, HorizontalAlignment = left ? HorizontalAlignment.Left : HorizontalAlignment.Right, IsHitTestVisible = false, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(250) },
            Background = new LinearGradientBrush { StartPoint = new(left ? 0 : 1, 0.5), EndPoint = new(left ? 1 : 0, 0.5), GradientStops = { new GradientStop { Color = Windows.UI.Color.FromArgb(0xD0, BoardColor(look).R, BoardColor(look).G, BoardColor(look).B), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(0, 0, 0, 0), Offset = 1 } } },
        };
        var leftEdge = Edge(true);
        var rightEdge = Edge(false);
        inside.Children.Add(leftEdge);
        inside.Children.Add(rightEdge);
        var frame = new Border { Height = BoardHeight, Child = inside };

        // the strip of dates under the board: each day where it lies, the part in view lit; a click goes there
        var strip = new Grid { Height = 40, Margin = new Thickness(32, 4, 32, 18), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        var track = new Canvas();
        strip.Children.Add(track);
        var window = new Border { Height = 30, CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x26, 0x7F, 0x77, 0xDD)), BorderBrush = Res("ApexAccentBrush"), BorderThickness = new Thickness(1) };
        Canvas.SetTop(window, 5);
        void Lay()
        {
            var tw = strip.ActualWidth;
            if (tw <= 0) return;
            track.Children.Clear();
            var rule = new Border { Width = tw, Height = 1, Background = ink, Opacity = 0.25 };
            Canvas.SetTop(rule, 20);
            track.Children.Add(rule);
            var lastRight = -1000.0;
            foreach (var (dx, d) in days)
            {
                var at = dx / width * tw;
                var tick = new Border { Width = 2, Height = 10, CornerRadius = new CornerRadius(1), Background = ink, Opacity = 0.6 };
                Canvas.SetLeft(tick, at); Canvas.SetTop(tick, 15);
                track.Children.Add(tick);
                if (at < lastRight + 8) continue;  // (labels that would touch stay ticks)
                var label = new TextBlock { Text = d.ToString("d MMM"), FontSize = 11, Foreground = ink, Opacity = 0.75 };
                Canvas.SetLeft(label, at + 5); Canvas.SetTop(label, 0);
                track.Children.Add(label);
                lastRight = at + 5 + d.ToString("d MMM").Length * 6.5;
            }
            track.Children.Add(window);
            Place();
        }
        void Place()
        {
            var tw = strip.ActualWidth;
            if (tw <= 0 || scroll.ExtentWidth <= 0) return;
            window.Width = Math.Max(24, scroll.ViewportWidth / scroll.ExtentWidth * tw);
            Canvas.SetLeft(window, scroll.HorizontalOffset / scroll.ExtentWidth * tw);
            leftEdge.Opacity = scroll.HorizontalOffset > 4 ? 1 : 0;
            rightEdge.Opacity = scroll.HorizontalOffset < scroll.ScrollableWidth - 4 ? 1 : 0;
        }
        strip.SizeChanged += (_, _) => Lay();
        scroll.ViewChanged += (_, _) => Place();
        var scrubbing = false;
        void Jump(PointerRoutedEventArgs e)
        {
            var at = e.GetCurrentPoint(strip).Position.X / Math.Max(1, strip.ActualWidth) * width;
            scroll.ChangeView(Math.Clamp(at - scroll.ViewportWidth / 2, 0, scroll.ScrollableWidth), null, null, true);
        }
        strip.PointerPressed += (_, e) => { scrubbing = strip.CapturePointer(e.Pointer); Jump(e); };
        strip.PointerMoved += (_, e) => { if (scrubbing) Jump(e); };
        strip.PointerReleased += (_, e) => { scrubbing = false; strip.ReleasePointerCapture(e.Pointer); };
        strip.PointerCaptureLost += (_, _) => scrubbing = false;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(strip, T("Dates"));
        var all = new StackPanel();
        all.Children.Add(frame);
        if (pics.Count > 0) all.Children.Add(strip);
        Gallery.Save(_gal!);
        return all;
    }

    // a picture in the album's frame: it falls in turning and bounces, its tape sticks, its caption writes itself; dragged
    // to move, lifted under the pointer, its caption written on with a click, opened with a double-click
    FrameworkElement Framed(Picture p, Placed place, AlbumLook look, Brush ink, int order, int delay, List<Picture> pics)
    {
        var info = Gallery.Peek(_gal!, p.Path);
        var big = order % 4 == 0;
        double w = (big ? 290 : 230) * _boardScale * look.Size, h = (big ? 194 : 154) * _boardScale * look.Size;
        var source = new BitmapImage(new Uri(p.Path)) { DecodePixelWidth = 600 };
        var dark = new SolidColorBrush(Rgb(0x2B, 0x2A, 0x26));
        var caption = new TextBlock { FontFamily = new FontFamily(look.Font), FontSize = (big ? 24 : 20) * Math.Max(0.85, _boardScale), TextTrimming = TextTrimming.CharacterEllipsis, MinHeight = 28, HorizontalAlignment = HorizontalAlignment.Left };
        var stack = new StackPanel();
        FrameworkElement body;
        var tape = false;
        switch (look.Frame)
        {
            case "rounded":
                stack.Children.Add(new Border { Width = w, Height = h, CornerRadius = new CornerRadius(20), Background = new ImageBrush { ImageSource = source, Stretch = Stretch.UniformToFill } });
                caption.Foreground = dark;
                stack.Children.Add(new Border { Child = caption, Margin = new Thickness(10, -18, 0, 0), Padding = new Thickness(12, 2, 12, 2), CornerRadius = new CornerRadius(14), HorizontalAlignment = HorizontalAlignment.Left,
                    Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xEB, 0xF6, 0xF2, 0xEA)) });
                body = stack;
                break;
            case "stamp":
                var stamp = new Grid { Padding = new Thickness(12) };
                stamp.Children.Add(new Microsoft.UI.Xaml.Shapes.Rectangle { Fill = new SolidColorBrush(Rgb(0xFB, 0xF8, 0xF1)), Stroke = new SolidColorBrush(Rgb(0xB9, 0xAF, 0x9C)), StrokeDashArray = [1.2, 1.6], StrokeThickness = 5, Margin = new Thickness(-12) });
                var inner = new StackPanel();
                inner.Children.Add(new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h });
                caption.Foreground = dark;
                caption.Margin = new Thickness(2, 6, 2, 0);
                inner.Children.Add(caption);
                stamp.Children.Add(inner);
                // (a postmark over the corner, with the day)
                var mark = new Grid { Width = 74, Height = 74, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, -22, -26, 0), Rotation = -14, CenterPoint = new Vector3(37, 37, 0) };
                mark.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Stroke = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB0, 0x6A, 0x4E, 0x8C)), StrokeThickness = 2 });
                mark.Children.Add(new TextBlock { Text = p.Taken.ToString("dd MMM\nyyyy").ToUpperInvariant(), FontSize = 10, FontWeight = FontWeights.Bold, TextAlignment = TextAlignment.Center, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(0xC0, 0x6A, 0x4E, 0x8C)), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
                stamp.Children.Add(mark);
                body = stamp;
                break;
            case "film":
                var film = new StackPanel { Background = new SolidColorBrush(Rgb(0x14, 0x14, 0x14)), Padding = new Thickness(10, 6, 10, 8) };
                StackPanel Holes() { var r = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9, Margin = new Thickness(2, 3, 0, 3) }; for (int k = 0; k < (int)(w / 19); k++) r.Children.Add(new Border { Width = 10, Height = 7, CornerRadius = new CornerRadius(2), Background = new SolidColorBrush(Rgb(0x3A, 0x3A, 0x3A)) }); return r; }
                film.Children.Add(Holes());
                film.Children.Add(new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h });
                film.Children.Add(Holes());
                caption.Foreground = new SolidColorBrush(Rgb(0xE6, 0xE0, 0xD2));
                caption.Margin = new Thickness(2, 2, 2, 0);
                film.Children.Add(caption);
                body = film;
                break;
            case "circle":
                var size = (big ? 210 : 170) * _boardScale * look.Size;
                stack.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = size, Height = size, StrokeThickness = 7, Stroke = new SolidColorBrush(Rgb(0xF6, 0xF2, 0xEA)), Fill = new ImageBrush { ImageSource = source, Stretch = Stretch.UniformToFill } });
                caption.Foreground = ink;
                caption.HorizontalAlignment = HorizontalAlignment.Center;
                caption.Margin = new Thickness(0, 8, 0, 0);
                stack.Children.Add(caption);
                body = stack;
                break;
            default:
                stack.Children.Add(new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h });
                caption.Foreground = dark;
                caption.Margin = new Thickness(2, 8, 2, 0);
                stack.Children.Add(caption);
                body = new Border { Background = new SolidColorBrush(Rgb(0xF2, 0xEF, 0xE8)), Padding = new Thickness(11, 11, 11, 12), Child = stack };
                tape = true;
                break;
        }
        var holder = new Grid();
        holder.Children.Add(body);
        if (look.Frame != "circle" && look.Frame != "film") body.Shadow = new ThemeShadow();
        Border? strip = null;
        if (tape && look.Tape != "none")
        {
            strip = new Border { Width = 96, Height = 22, Background = TapeBrush(look, order), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, -22, 0, 0),
                RenderTransform = new CompositeTransform { Rotation = (order % 3 - 1) * 4, ScaleX = _boardCalm ? 1 : 0 }, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5) };
            holder.Children.Add(strip);
        }
        var outer = new Border { Child = holder };
        Canvas.SetLeft(outer, place.X);
        Canvas.SetTop(outer, place.Y);
        Canvas.SetZIndex(outer, order);
        outer.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(220) };
        outer.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(240) };
        outer.Translation = new Vector3(0, 0, 32);
        outer.SizeChanged += (_, e) => outer.CenterPoint = new Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0);
        var angle = place.Angle * look.Tilt;
        var turn = Land(holder, angle, delay, order % 2 == 0 ? -50 : 50);
        var text = info.Caption is { Length: > 0 } c ? c : T("+ write something");
        if (strip is not null && !_boardCalm)
        {
            var tapeIn = new Storyboard { BeginTime = TimeSpan.FromMilliseconds(delay + 640) };
            Tween(tapeIn, strip.RenderTransform, "ScaleX", 1, 420, new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.6 });
            tapeIn.Begin();
        }
        if (_boardCalm) caption.Text = text; else After(delay + 820, () => Write(caption, text));

        // (under the pointer it lifts, grows a touch and nearly straightens; back to its tilt when left)
        outer.PointerEntered += (_, _) => { if (_dragging) return; outer.Translation = new Vector3(0, -12, 56); outer.Scale = new Vector3(1.04f, 1.04f, 1); Canvas.SetZIndex(outer, 900); Turn(turn, angle * 0.25); };
        outer.PointerExited += (_, _) => { if (_dragging) return; outer.Translation = new Vector3(0, 0, 32); outer.Scale = Vector3.One; Canvas.SetZIndex(outer, order); Turn(turn, angle, 420, new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 }); };
        Draggable(outer, () => { place.X = Canvas.GetLeft(outer); place.Y = Canvas.GetTop(outer); Gallery.Save(_gal!); });
        outer.Tapped += (_, e) => { e.Handled = true; OpenViewer(pics, pics.IndexOf(p)); };
        caption.Tapped += (_, e) =>
        {
            e.Handled = true;
            if (caption.Parent is not Panel host) return;
            var box = new TextBox { Text = info.Caption ?? "", FontFamily = caption.FontFamily, FontSize = caption.FontSize, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0, 0, 0, 1),
                Foreground = caption.Foreground, PlaceholderText = T("write something"), MinWidth = 160, Margin = caption.Margin };
            box.Resources["TextControlBackgroundFocused"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            box.Resources["TextControlForegroundFocused"] = caption.Foreground;
            var at = host.Children.IndexOf(caption);
            host.Children[at] = box;
            box.Focus(FocusState.Programmatic);
            void Done()
            {
                var keep = Gallery.Info(_gal!, p.Path);
                keep.Caption = box.Text.Trim();
                info = keep;
                Gallery.Save(_gal!);
                caption.Text = keep.Caption.Length > 0 ? keep.Caption : T("+ write something");
                if (host.Children.Contains(box)) host.Children[host.Children.IndexOf(box)] = caption;
            }
            box.LostFocus += (_, _) => Done();
            box.KeyDown += (_, k) => { if (k.Key == Windows.System.VirtualKey.Enter) { k.Handled = true; Done(); } };
        };
        return outer;
    }

    // a sticky note: it lands like the pictures, is written on in the album's handwriting, dragged by its edge, and goes
    // with its little cross
    Border Sticky(StickyNote note, AlbumLook look, int order)
    {
        var color = NoteColors.FirstOrDefault(c => c.Key == note.Color).Color;
        if (color.A == 0) color = NoteColors[0].Color;
        var box = new TextBox { Text = note.Text, FontFamily = new FontFamily(look.Font), FontSize = 20, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, PlaceholderText = T("write here"),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0), Foreground = new SolidColorBrush(Rgb(0x2B, 0x2A, 0x26)), Padding = new Thickness(0) };
        box.Resources["TextControlBackgroundFocused"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        box.Resources["TextControlBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        box.Resources["TextControlForegroundFocused"] = new SolidColorBrush(Rgb(0x2B, 0x2A, 0x26));
        box.Resources["TextControlBorderBrushFocused"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        box.LostFocus += (_, _) => { note.Text = box.Text; Gallery.Save(_gal!); };
        var close = new Button { Content = new FontIcon { Glyph = "", FontSize = 10 }, Width = 24, Height = 24, Padding = new Thickness(0), BorderThickness = new Thickness(0), Opacity = 0,
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x1A, 0, 0, 0)), Foreground = new SolidColorBrush(Rgb(0x2B, 0x2A, 0x26)),
            OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(150) } };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(close, T("Remove the note"));
        var grid = new Grid { Width = 180, Height = 170, Padding = new Thickness(16, 26, 14, 12) };
        grid.Children.Add(box);
        grid.Children.Add(close);
        close.Margin = new Thickness(0, -20, -8, 0);
        var paper = new Border { Child = grid, Background = new SolidColorBrush(color), Shadow = new ThemeShadow(), CornerRadius = new CornerRadius(2), Translation = new Vector3(0, 0, 24) };
        var tilt = new Border { Child = paper };
        var outer = new Border { Child = tilt };
        Canvas.SetLeft(outer, note.X);
        Canvas.SetTop(outer, note.Y);
        Canvas.SetZIndex(outer, 500 + order);
        Land(tilt, note.Angle, 160 + order * 140, 0);
        outer.PointerEntered += (_, _) => close.Opacity = 1;
        outer.PointerExited += (_, _) => close.Opacity = 0;
        close.Click += (_, _) =>
        {
            look.Notes.Remove(note);
            Gallery.Save(_gal!);
            var away = new Storyboard();
            Tween(away, outer, "Opacity", 0, 260);
            Tween(away, tilt.RenderTransform, "TranslateY", 60, 300, new CubicEase { EasingMode = EasingMode.EaseIn });
            Tween(away, tilt.RenderTransform, "Rotation", note.Angle + 14, 300);
            away.Completed += (_, _) => _boardCanvas?.Children.Remove(outer);
            away.Begin();
        };
        Draggable(outer, () => { note.X = Canvas.GetLeft(outer); note.Y = Canvas.GetTop(outer); Gallery.Save(_gal!); });
        return outer;
    }

    bool _dragging;
    // moved with the pointer (or a finger), lifted while it moves, kept where it is let go
    void Draggable(FrameworkElement el, Action dropped)
    {
        el.ManipulationMode = ManipulationModes.TranslateX | ManipulationModes.TranslateY;
        el.ManipulationStarted += (_, _) => { _dragging = true; Canvas.SetZIndex(el, 1000); el.Scale = new Vector3(1.06f, 1.06f, 1); };
        el.ManipulationDelta += (_, e) =>
        {
            var w = _boardCanvas?.Width ?? 2000;
            Canvas.SetLeft(el, Math.Clamp(Canvas.GetLeft(el) + e.Delta.Translation.X, -40, Math.Max(0, w - 120)));
            Canvas.SetTop(el, Math.Clamp(Canvas.GetTop(el) + e.Delta.Translation.Y, -20, BoardHeight - 120));
        };
        el.ManipulationCompleted += (_, _) => { _dragging = false; el.Scale = Vector3.One; dropped(); };
    }

    // the caption written a letter at a time
    void Write(TextBlock t, string text)
    {
        t.Text = "";
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(32);
        var i = 0;
        timer.Tick += (_, _) => { if (++i > text.Length) { timer.Stop(); _timers.Remove(timer); return; } t.Text = text[..i]; };
        _timers.Add(timer);
        timer.Start();
    }

    // (a timer nothing refers to can be collected before it fires: each is held until it has)
    readonly HashSet<Microsoft.UI.Dispatching.DispatcherQueueTimer> _timers = [];

    void After(int ms, Action a)
    {
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(Math.Max(1, ms));
        timer.IsRepeating = false;
        timer.Tick += (_, _) => { _timers.Remove(timer); a(); };
        _timers.Add(timer);
        timer.Start();
    }

    // every picture somewhere new along its stretch of the timeline, above or below the line, flying there with an overshoot
    void ShuffleBoard()
    {
        if (_boardCanvas is null || _gal is null) return;
        var look = Gallery.Look(_gal, _galAlbum);
        var rnd = new Random();
        var mid = BoardHeight / 2;
        var sb = new Storyboard();
        foreach (var (pic, card, i) in _boardCards)
        {
            var place = look.Places[pic.Path];
            place.X = 90 + i * Step + rnd.Next(-40, 40);
            place.Y = rnd.Next(2) == 0 ? rnd.Next(10, 70) : mid + rnd.Next(20, 80);
            place.Angle = rnd.Next(-10, 11);
            foreach (var (prop, to) in new[] { ("(Canvas.Left)", place.X), ("(Canvas.Top)", place.Y) })
            {
                var a = new DoubleAnimation { To = to, Duration = TimeSpan.FromMilliseconds(760), EnableDependentAnimation = true, EasingFunction = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.35 } };
                Storyboard.SetTarget(a, card);
                Storyboard.SetTargetProperty(a, prop);
                sb.Children.Add(a);
            }
            Tween(sb, ((Border)card).Child.RenderTransform, "Rotation", place.Angle * look.Tilt, 1100, new ElasticEase { EasingMode = EasingMode.EaseOut, Oscillations = 1, Springiness = 4 });
        }
        sb.Completed += (_, _) => { foreach (var (pic, card, _) in _boardCards) { Canvas.SetLeft(card, look.Places[pic.Path].X); Canvas.SetTop(card, look.Places[pic.Path].Y); } };
        sb.Begin();
        Gallery.Save(_gal);
    }

    // a new note where the board is being looked at
    void AddNote(string color)
    {
        if (_boardCanvas is null || _gal is null) return;
        var look = Gallery.Look(_gal, _galAlbum);
        var rnd = new Random();
        var left = (_boardScroll?.HorizontalOffset ?? 0) + (_boardScroll?.ViewportWidth ?? 800) / 2 - 90;
        var note = new StickyNote { Color = color, X = left + rnd.Next(-80, 80), Y = BoardHeight / 2 - 85 + rnd.Next(-60, 60), Angle = rnd.Next(-6, 7) };
        look.Notes.Add(note);
        Gallery.Save(_gal);
        _boardCanvas.Children.Add(Sticky(note, look, 0));
    }

    static readonly (string Glyph, string Name)[] AlbumIcons =
    [
        ("\uE709", "Trip"), ("\uEB51", "Love"), ("\uE80F", "Home"), ("\uE722", "Pictures"), ("\uE734", "Favourite"), ("\uE8F1", "Gift"),
        ("\uE7C1", "Family"), ("\uE909", "World"), ("\uE706", "Summer"), ("\uE708", "Night"), ("\uE8D6", "Music"), ("\uE7FC", "Games"),
        ("\uE913", "Places"), ("\uE7EE", "Celebration"), ("\uEA86", "Pets"), ("\uE825", "Build"),
    ];

    // a new album (album null) or a change to one: its name and its icon; the album, or null when cancelled
    async Task<CustomAlbum?> AlbumDialog(CustomAlbum? album, string? home = null)
    {
        var name = new TextBox { Header = T("Name"), Text = album?.Name ?? "", PlaceholderText = T("Trip to Egypt") };
        var grid = new GridView { SelectionMode = ListViewSelectionMode.Single, Margin = new Thickness(0, 14, 0, 0), MaxHeight = 220 };
        foreach (var (glyph, label) in AlbumIcons)
        {
            var cell = new Border { Width = 52, Height = 52, CornerRadius = new CornerRadius(10), Background = Res("ApexPillBrush"), Child = new FontIcon { Glyph = glyph, FontSize = 22 } };
            ToolTipService.SetToolTip(cell, T(label));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(cell, T(label));
            grid.Items.Add(cell);
        }
        grid.SelectedIndex = Math.Max(0, Array.FindIndex(AlbumIcons, i => i.Glyph == album?.Icon));
        var body = new StackPanel { Width = 400 };
        body.Children.Add(name);
        var house = new ComboBox { Header = T("Household"), HorizontalAlignment = HorizontalAlignment.Stretch, Margin = new Thickness(0, 14, 0, 0) };
        foreach (var v in _galSaves) house.Items.Add(v.Name);
        var was = album is null ? home ?? (_galSaves.Any(v => v.Name == _galAlbum) ? _galAlbum : null) : Gallery.HomeOf(album, _gal!, _galPics, _galSaves);
        house.SelectedIndex = Math.Max(0, _galSaves.ToList().FindIndex(v => string.Equals(v.Name, was, StringComparison.OrdinalIgnoreCase)));
        if (_galSaves.Count > 0) body.Children.Add(house);
        body.Children.Add(new TextBlock { Text = T("Icon"), Margin = new Thickness(0, 14, 0, 0) });
        body.Children.Add(grid);
        var d = new ContentDialog { Title = album is null ? T("New album") : T("Change the album"), Content = body, PrimaryButtonText = album is null ? T("Make it") : T("Save"), CloseButtonText = T("Cancel"),
            DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot, IsPrimaryButtonEnabled = name.Text.Trim().Length > 0 };
        name.TextChanged += (_, _) => d.IsPrimaryButtonEnabled = name.Text.Trim().Length > 0;
        if (await ShowDialog(d) != ContentDialogResult.Primary) return null;
        var made = album ?? new CustomAlbum();
        made.Name = name.Text.Trim();
        made.Icon = AlbumIcons[Math.Max(0, grid.SelectedIndex)].Glyph;
        made.Household = house.SelectedItem as string;
        if (album is null) _gal!.Custom.Add(made);
        Gallery.Save(_gal!);
        return made;
    }

    // ---------- the viewer ----------

    void OpenViewer(List<Picture> pics, int at)
    {
        if (pics.Count == 0 || Content.XamlRoot is not { } root) return;
        var popup = new Popup { XamlRoot = root, IsLightDismissEnabled = false };
        var shell = new Grid { Width = root.Size.Width, Height = root.Size.Height, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xEB, 0x05, 0x06, 0x08)), OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) }, Opacity = 0 };
        shell.ColumnDefinitions.Add(new ColumnDefinition());
        shell.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(340) });
        var stage = new Grid { Padding = new Thickness(48) };
        var image = new Image { Stretch = Stretch.Uniform, ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(360) }, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(260) } };
        image.SizeChanged += (_, e) => image.CenterPoint = new Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0);
        stage.Children.Add(image);
        Button Arrow(string glyph, string name, HorizontalAlignment side)
        {
            var b = new Button { Content = new FontIcon { Glyph = glyph, FontSize = 16 }, Width = 46, Height = 46, CornerRadius = new CornerRadius(23), HorizontalAlignment = side, VerticalAlignment = VerticalAlignment.Center,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x1F, 255, 255, 255)), BorderThickness = new Thickness(0), Margin = new Thickness(-28, 0, -28, 0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
            return b;
        }
        var prev = Arrow("", T("Previous picture"), HorizontalAlignment.Left);
        var next = Arrow("", T("Next picture"), HorizontalAlignment.Right);
        stage.Children.Add(prev);
        stage.Children.Add(next);
        shell.Children.Add(stage);

        var side = new StackPanel { Spacing = 16, Padding = new Thickness(26, 28, 26, 24), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1, 0, 0, 0) };
        Grid.SetColumn(side, 1);
        shell.Children.Add(side);
        popup.Child = shell;

        void Close() { shell.Opacity = 0; After(220, () => { popup.IsOpen = false; BuildGallery(); }); }
        void Fill()
        {
            var p = pics[at];
            var info = Gallery.Info(_gal!, p.Path);
            image.Opacity = 0;
            image.Scale = new Vector3(0.94f, 0.94f, 1);
            image.Source = new BitmapImage(new Uri(p.Path));
            DispatcherQueue.TryEnqueue(() => { image.Opacity = 1; image.Scale = Vector3.One; });
            side.Children.Clear();
            var top = new Grid();
            top.ColumnDefinitions.Add(new ColumnDefinition());
            top.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            top.Children.Add(new TextBlock { Text = Path.GetFileNameWithoutExtension(p.Path), FontSize = 15, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center });
            var x = new Button { Content = new FontIcon { Glyph = "", FontSize = 12 }, Width = 34, Height = 34, Padding = new Thickness(0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(x, T("Close"));
            x.Click += (_, _) => Close();
            Grid.SetColumn(x, 1);
            top.Children.Add(x);
            side.Children.Add(top);
            side.Children.Add(Muted(p.Taken.ToString("f") + "   ·   " + (at + 1) + " / " + pics.Count));
            var cap = new TextBox { Header = T("Caption"), Text = info.Caption ?? "", PlaceholderText = T("write something"), AcceptsReturn = false };
            cap.LostFocus += (_, _) => { info.Caption = cap.Text.Trim(); Gallery.Save(_gal!); };
            side.Children.Add(cap);
            var home = new ComboBox { Header = T("Household"), HorizontalAlignment = HorizontalAlignment.Stretch };
            var names = _galSaves.Select(s => s.Name).ToList();
            foreach (var n in names) home.Items.Add(n);
            var guess = Gallery.GuessHousehold(p, _galSaves);
            home.SelectedIndex = names.IndexOf(Gallery.HouseholdOf(p, _gal!, _galSaves) ?? "");
            home.SelectionChanged += (_, _) => { if (home.SelectedIndex < 0) return; var chosen = names[home.SelectedIndex]; info.Household = chosen == guess ? null : chosen; Gallery.Save(_gal!); };
            side.Children.Add(home);
            var fav = new ToggleButton { Content = info.Favourite ? T("★ In Favourites") : T("☆ Add to Favourites"), IsChecked = info.Favourite, HorizontalAlignment = HorizontalAlignment.Stretch };
            fav.Click += (_, _) => { info.Favourite = fav.IsChecked == true; fav.Content = info.Favourite ? T("★ In Favourites") : T("☆ Add to Favourites"); Gallery.Save(_gal!); };
            side.Children.Add(fav);
            var into = new DropDownButton { Content = T("Add to an album"), HorizontalAlignment = HorizontalAlignment.Stretch };
            var albums = new MenuFlyout();
            void FillAlbums()
            {
                albums.Items.Clear();
                foreach (var c in _gal!.Custom)
                {
                    var t = new ToggleMenuFlyoutItem { Text = c.Name, IsChecked = c.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase), Icon = new FontIcon { Glyph = c.Icon } };
                    var one = c;
                    t.Click += (_, _) => Gallery.Toggle(_gal, one, p.Path);
                    albums.Items.Add(t);
                }
                if (_gal.Custom.Count > 0) albums.Items.Add(new MenuFlyoutSeparator());
                var make = new MenuFlyoutItem { Text = T("New album…"), Icon = new FontIcon { Glyph = "\uE710" } };
                make.Click += async (_, _) => { popup.IsOpen = false; if (await AlbumDialog(null, Gallery.HouseholdOf(p, _gal, _galSaves)) is { } made) Gallery.Toggle(_gal, made, p.Path); popup.IsOpen = true; };
                albums.Items.Add(make);
            }
            albums.Opening += (_, _) => FillAlbums();
            into.Flyout = albums;
            side.Children.Add(into);
            var useHome = Button(T("Use on Home"), () => { _gal!.HomePicture = p.Path; Gallery.Save(_gal); SetInfo(GalleryInfo(), InfoBarSeverity.Success, T("Home opens on this picture now"), ""); }, accent: true);
            useHome.HorizontalAlignment = HorizontalAlignment.Stretch;
            side.Children.Add(useHome);
            var links = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 18, Margin = new Thickness(0, 10, 0, 0) };
            var show = Link(T("Show in folder"), () => Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{p.Path}\"") { UseShellExecute = true }));
            show.Padding = new Thickness(0);
            var bin = Link(T("Move to the Recycle Bin"), async () =>
            {
                if (!await Confirm(T("Move this picture to the Recycle Bin?"), T("You can put it back from the Recycle Bin."), T("Move it"))) return;
                try { Gallery.Remove(_gal!, p.Path); pics.RemoveAt(at); if (pics.Count == 0) { Close(); return; } at = Math.Min(at, pics.Count - 1); Fill(); }
                catch (Exception e) { App.Log(e); await Message(T("The picture was not moved"), ErrorText(e)); }
            });
            bin.Padding = new Thickness(0);
            bin.Foreground = Res("ApexErrorBrush");
            links.Children.Add(show);
            links.Children.Add(bin);
            side.Children.Add(links);
        }
        prev.Click += (_, _) => { at = (at - 1 + pics.Count) % pics.Count; Fill(); };
        next.Click += (_, _) => { at = (at + 1) % pics.Count; Fill(); };
        shell.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Escape) Close();
            else if (e.Key == Windows.System.VirtualKey.Left) { at = (at - 1 + pics.Count) % pics.Count; Fill(); }
            else if (e.Key == Windows.System.VirtualKey.Right) { at = (at + 1) % pics.Count; Fill(); }
        };
        Fill();
        popup.IsOpen = true;
        DispatcherQueue.TryEnqueue(() => { shell.Opacity = 1; next.Focus(FocusState.Programmatic); });
    }

    // a message line over the gallery (made once, kept on top of its screen)
    InfoBar? _galInfo;
    InfoBar GalleryInfo()
    {
        if (_galInfo is null) { _galInfo = new InfoBar { IsClosable = true }; }
        if (!ScreenGallery.Children.Contains(_galInfo)) ScreenGallery.Children.Insert(0, _galInfo);
        return _galInfo;
    }
}
