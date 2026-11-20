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
    static readonly string[] Hands =
        ["Ink Free", "Segoe Print", "Segoe Script", "MV Boli", "Gabriola", "Comic Sans MS", "Sitka Banner", "Georgia", "Constantia", "Candara", "Trebuchet MS", "Bahnschrift", "Courier New", "Cascadia Code", "Impact", "Segoe UI Variable Display"];
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
        _custDrawer = null;
        _boardPaper = null;
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
            if (Math.Abs(BoardFits() - BoardHeight) <= 8) return;
            if (_boardFrom == 0) _boardFrom = BoardHeight;
            RefreshBoard();
        };
        var book = new Grid { Background = BoardBrush(look) };
        _boardFrame = book;
        _boardLight = new Border { IsHitTestVisible = false, Background = LightBrush(look) };
        book.Children.Add(_boardLight);
        var paper = new Canvas { IsHitTestVisible = false };
        _boardPaper = paper;
        book.SizeChanged += (_, e) => DrawPaper(paper, Gallery.Look(_gal!, _galAlbum), e.NewSize.Width, e.NewSize.Height, 28);
        book.Children.Add(paper);
        book.Children.Add(column);
        book.Children.Add(CustomizeDrawer());
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
        if (_galBook) { _boardHint = hint; hint.Foreground = Ink(look); hint.Opacity = 0.7; hint.TextWrapping = TextWrapping.NoWrap; hint.TextTrimming = TextTrimming.CharacterEllipsis; _boardTitle.TextTrimming = TextTrimming.CharacterEllipsis; }
        titles.Children.Add(hint);
        g.Children.Add(titles);

        var tools = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        if (_galBook)
        {
            _boardTools.Clear();
            T Round<T>(T b, string glyph, string name) where T : Button
            {
                b.Content = new FontIcon { Glyph = glyph, FontSize = 16 };
                b.Width = 44; b.Height = 44; b.Padding = new Thickness(0); b.CornerRadius = new CornerRadius(22); b.BorderThickness = new Thickness(1);
                _boardTools.Add(b);
                InkTool(b, look);
                ToolTipService.SetToolTip(b, name);
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
                return b;
            }
            var custom = Round(new Button(), "\uE790", T("Customize"));
            custom.Click += (_, _) => ToggleCustomize();
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
            : shown.GroupBy(p => p.Taken.Date).Select(x => (Key: x.Key.ToString("D", LangCulture()), List: x.ToList()));
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
                wrap.Children.Add(Tile(p, el => OpenViewer(pics, at, el)));
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
    UIElement Tile(Picture p, Action<FrameworkElement> open)
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
            Child = new TextBlock { Text = p.Taken.ToString("t", LangCulture()) + "  ·  " + (Gallery.HouseholdOf(p, _gal!, _galSaves) ?? ""), FontSize = 12, Foreground = new SolidColorBrush(Microsoft.UI.Colors.White), TextTrimming = TextTrimming.CharacterEllipsis } };
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
        b.Click += (_, _) => open(b);
        return b;
    }

    // a storyboard step: one property of one element to a value, over a time, with an ease
    // (a finished storyboard otherwise keeps holding what it animated, and nothing set afterwards shows)
    static readonly Dictionary<Storyboard, List<(DependencyObject Target, string Property, double To)>> Ends = [];
    static void Play(Storyboard sb)
    {
        sb.Completed += (_, _) =>
        {
            if (!Ends.Remove(sb, out var ends)) return;
            foreach (var (t, p, v) in ends)
                switch (t)
                {
                    case CompositeTransform c when p == "TranslateX": c.TranslateX = v; break;
                    case CompositeTransform c when p == "TranslateY": c.TranslateY = v; break;
                    case CompositeTransform c when p == "Rotation": c.Rotation = v; break;
                    case CompositeTransform c when p == "ScaleX": c.ScaleX = v; break;
                    case CompositeTransform c when p == "ScaleY": c.ScaleY = v; break;
                    case UIElement e when p == "Opacity": e.Opacity = v; break;
                }
            sb.Stop();
        };
        sb.Begin();
    }

    static void Tween(Storyboard sb, DependencyObject target, string property, double to, int ms, EasingFunctionBase? ease = null, int begin = 0)
    {
        if (!Ends.TryGetValue(sb, out var ends)) Ends[sb] = ends = [];
        ends.Add((target, property, to));
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
        Play(sb);
        return t;
    }

    // a quick turn of a card (hover, shuffle)
    static void Turn(CompositeTransform t, double to, int ms = 320, EasingFunctionBase? ease = null)
    {
        var sb = new Storyboard();
        Tween(sb, t, "Rotation", to, ms, ease ?? new CubicEase { EasingMode = EasingMode.EaseOut });
        Play(sb);
    }

    // the tape's colour: bright, or the board's own hue made paler
    Brush TapeBrush(AlbumLook look, int order)
    {
        if (look.Tape == "match") { var c = Shade(BoardColor(look), 0.45); return new SolidColorBrush(Windows.UI.Color.FromArgb(0xB8, c.R, c.G, c.B)); }
        return new SolidColorBrush(Tapes[order % Tapes.Length]);
    }

    // ---------- Scrapbook ----------

    // The album as a timeline: the newest pictures on the left, going back in time to the right, a line through the board
    // with each day marked; it scrolls sideways (the mouse wheel too) and opens on the newest. Every album keeps its own colour (one
    // hue fading softly darker), handwriting and frames.
    const double Step = 270;
    double BoardHeight = 660, _boardScale = 1;
    Canvas? _boardCanvas;
    Panel? _boardFrame;
    ScrollViewer? _boardScroll;
    readonly List<(Picture Pic, FrameworkElement Card, int Index)> _boardCards = [];
    static readonly (string Key, string Name)[] Frames =
        [("polaroid", "Polaroid"), ("print", "Print"), ("washi", "Washi tape"), ("clip", "Paper clip"), ("rounded", "Rounded"), ("kraft", "Kraft"), ("deckle", "Vintage"), ("doodle", "Doodled"), ("stamp", "Stamp"), ("film", "Film")];

    // diagonal stripes, as on washi tape
    static Brush Stripes(Windows.UI.Color a, Windows.UI.Color b, double size) => new LinearGradientBrush
    {
        MappingMode = BrushMappingMode.Absolute, SpreadMethod = GradientSpreadMethod.Repeat, StartPoint = new(0, 0), EndPoint = new(size, size),
        GradientStops = { new GradientStop { Color = a, Offset = 0 }, new GradientStop { Color = a, Offset = 0.5 }, new GradientStop { Color = b, Offset = 0.5 }, new GradientStop { Color = b, Offset = 1 } },
    };
    static readonly (Windows.UI.Color A, Windows.UI.Color B)[] Washi =
        [(Rgb(0xF3, 0xA6, 0xC0), Rgb(0xFB, 0xD3, 0xE0)), (Rgb(0x9F, 0xD3, 0xC7), Rgb(0xD6, 0xF0, 0xEA)), (Rgb(0xF2, 0xC9, 0x6B), Rgb(0xFA, 0xE6, 0xB4)), (Rgb(0xB4, 0xAE, 0xF2), Rgb(0xDE, 0xDB, 0xFA))];

    // a strip of washi tape over one corner of a picture
    static Border WashiStrip(int corner, int order, double k)
    {
        var (a, b) = Washi[(order + (corner is 0 or 3 ? 0 : 1)) % Washi.Length];
        var left = corner % 2 == 0;
        var top = corner < 2;
        return new Border
        {
            Width = 58 * k, Height = 18 * k, Opacity = 0.9, Background = Stripes(a, b, 12 * k),
            HorizontalAlignment = left ? HorizontalAlignment.Left : HorizontalAlignment.Right, VerticalAlignment = top ? VerticalAlignment.Top : VerticalAlignment.Bottom,
            Margin = new Thickness(left ? -18 * k : 0, top ? -4 * k : 0, left ? 0 : -18 * k, top ? 0 : -4 * k),
            RenderTransform = new RotateTransform { Angle = left == top ? -38 : 38 }, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5),
        };
    }

    static Geometry Geo(string d) => (Geometry)Microsoft.UI.Xaml.Markup.XamlBindingHelper.ConvertValue(typeof(Geometry), d);
    const string DoodleA = "M6 8 Q60 3 120 7 T224 6 Q226 70 222 150 Q160 156 110 152 T4 154 Q2 90 6 8";
    const string DoodleB = "M10 4 Q90 9 218 3 Q221 80 226 148 Q120 158 8 150 Q6 70 10 4";
    const string Star = "M7 0 L9 5 L14 5.5 L10 9 L11 14 L7 11.5 L3 14 L4 9 L0 5.5 L5 5 Z";

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

    // ---------- Customize: a panel that slides in over the board's right side, the board changing behind it ----------

    Border? _custDrawer;
    double _boardBuiltSize = 1;

    // the pictures on the board turned to the tilt and scaled to the size being chosen, without laying the board again
    void LiveCards(AlbumLook look)
    {
        foreach (var (pic, card, _) in _boardCards)
            if (card is Border { Child: FrameworkElement { RenderTransform: CompositeTransform t } } && look.Places.TryGetValue(pic.Path, out var at))
            {
                t.Rotation = at.Angle * look.Tilt;
                t.ScaleX = t.ScaleY = look.Size / _boardBuiltSize;
            }
    }
    TextBlock? _boardHint;
    readonly List<Button> _boardTools = [];

    // a round tool on the board, its icon in the board's ink and its fill a breath of the same
    static void InkTool(Button b, AlbumLook look)
    {
        var c = IsLight(BoardColor(look)) ? Rgb(0x3A, 0x34, 0x2A) : Rgb(0xE8, 0xE2, 0xD4);
        var fill = new SolidColorBrush(Windows.UI.Color.FromArgb(0x1A, c.R, c.G, c.B));
        var over = new SolidColorBrush(Windows.UI.Color.FromArgb(0x2E, c.R, c.G, c.B));
        var ink = new SolidColorBrush(c);
        b.Foreground = ink;
        b.Background = fill;
        b.BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x30, c.R, c.G, c.B));
        b.Resources["ButtonBackgroundPointerOver"] = over;
        b.Resources["ButtonBackgroundPressed"] = over;
        b.Resources["ButtonForegroundPointerOver"] = ink;
        b.Resources["ButtonForegroundPressed"] = ink;
        b.Resources["ButtonBorderBrushPointerOver"] = new SolidColorBrush(Windows.UI.Color.FromArgb(0x50, c.R, c.G, c.B));
    }
    string _fontQuery = "";
    List<string>? _fonts;
    int _custTab;
    bool _custChanged;

    void ToggleCustomize(bool? open = null)
    {
        if (_custDrawer is null) return;
        var show = open ?? _custDrawer.Opacity < 0.5;
        _custDrawer.IsHitTestVisible = show;
        _custDrawer.Opacity = show ? 1 : 0;
        _custDrawer.Translation = new Vector3(show ? 0 : 420, 0, 40);
        if (show) FillCustomize();
        else if (_gal is not null)
        {
            Gallery.Save(_gal);
            if (_custChanged) { _custChanged = false; RefreshBoard(page: true); }
        }
    }

    Border CustomizeDrawer()
    {
        var d = new Border
        {
            Width = 372, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 14, 14, 14), CornerRadius = new CornerRadius(18),
            Background = new AcrylicBrush { TintColor = Rgb(0x17, 0x18, 0x1C), TintOpacity = 0.82, TintLuminosityOpacity = 0.9, FallbackColor = Rgb(0x1C, 0x1D, 0x22) },
            BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x26, 255, 255, 255)), BorderThickness = new Thickness(1),
            Shadow = new ThemeShadow(), Translation = new Vector3(420, 0, 40), Opacity = 0, IsHitTestVisible = false,
            TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(320) }, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) },
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(d, T("Customize"));
        _custDrawer = d;
        return d;
    }

    // a small header over a group of controls
    static TextBlock Caption(string t) => new() { Text = t, FontSize = 12, FontWeight = FontWeights.SemiBold, CharacterSpacing = 60, Opacity = 0.62, Margin = new Thickness(0, 6, 0, 0) };

    // a choice drawn as a card: its picture over its name, lit when chosen
    Button Choice(UIElement art, string name, bool on, Action pick, double height = 92)
    {
        var g = new Grid { RowSpacing = 6 };
        g.RowDefinitions.Add(new RowDefinition());
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        g.Children.Add(new Viewbox { Child = art, MaxHeight = height - 34, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Center });
        var label = new TextBlock { Text = name, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, Opacity = on ? 1 : 0.8 };
        Grid.SetRow(label, 1);
        g.Children.Add(label);
        var b = new Button { Content = g, Height = height, HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Padding = new Thickness(8, 10, 8, 8), CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(on ? 2 : 1),
            BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Windows.UI.Color.FromArgb(0x1A, 255, 255, 255)),
            Background = new SolidColorBrush(on ? Windows.UI.Color.FromArgb(0x2E, 0x7F, 0x77, 0xDD) : Windows.UI.Color.FromArgb(0x0C, 255, 255, 255)) };
        b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Windows.UI.Color.FromArgb(on ? (byte)0x3A : (byte)0x18, 0x9F, 0x97, 0xF0));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
        b.Click += (_, _) => pick();
        return b;
    }

    static Grid Columns(int n, double gap = 8)
    {
        var g = new Grid { ColumnSpacing = gap, RowSpacing = gap };
        for (int i = 0; i < n; i++) g.ColumnDefinitions.Add(new ColumnDefinition());
        return g;
    }
    static void PutAt(Grid g, UIElement e, int i, int n)
    {
        while (g.RowDefinitions.Count <= i / n) g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow((FrameworkElement)e, i / n);
        Grid.SetColumn((FrameworkElement)e, i % n);
        g.Children.Add(e);
    }

    static readonly (string Hex, string Name)[] Palette =
    [
        ("#16161D", "Ink"), ("#2A2D33", "Charcoal"), ("#1E2A4F", "Navy"), ("#2E5340", "Forest"), ("#4E2A55", "Plum"), ("#6E1F3A", "Wine"),
        ("#3E2A23", "Espresso"), ("#3C5A8A", "Denim"), ("#2B6F73", "Teal"), ("#6F7445", "Olive"), ("#B5573B", "Terracotta"), ("#C8566F", "Rose"),
        ("#6B4A35", "Walnut"), ("#4A5560", "Slate"), ("#93A68A", "Sage"), ("#D9A441", "Mustard"), ("#A99BD6", "Lavender"), ("#E7B8C0", "Blush"),
        ("#A57C55", "Cork"), ("#8DB5D6", "Sky"), ("#A9D5C1", "Mint"), ("#F0B48E", "Peach"), ("#D8C3A0", "Sand"), ("#EFE6D6", "Linen"),
    ];

    // hue and brightness to a colour: the brighter, the softer the colour, so a board never shouts
    static Windows.UI.Color FromHue(double hue, double bright, double? saturation = null)
    {
        var s = saturation ?? 0.62 - bright * 0.32;
        var c = bright * s;
        var x = c * (1 - Math.Abs(hue / 60 % 2 - 1));
        var m = bright - c;
        var (r, g, b) = hue switch { < 60 => (c, x, 0d), < 120 => (x, c, 0d), < 180 => (0d, c, x), < 240 => (0d, x, c), < 300 => (x, 0d, c), _ => (c, 0d, x) };
        return Windows.UI.Color.FromArgb(255, (byte)((r + m) * 255), (byte)((g + m) * 255), (byte)((b + m) * 255));
    }
    static (double Hue, double Bright, double Sat) HueOf(Windows.UI.Color col)
    {
        double r = col.R / 255.0, g = col.G / 255.0, b = col.B / 255.0, max = Math.Max(r, Math.Max(g, b)), min = Math.Min(r, Math.Min(g, b)), d = max - min;
        var h = d == 0 ? 0 : max == r ? 60 * ((g - b) / d % 6) : max == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
        return (h < 0 ? h + 360 : h, max, max == 0 ? 0 : d / max);
    }

    void FillCustomize()
    {
        if (_custDrawer is null || _gal is null) return;
        var look = Gallery.Look(_gal, _galAlbum);
        void Changed() { _custChanged = true; RefreshBoard(); }
        var root = new Grid();
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition());

        // the head: what is being dressed, and the way out
        var head = new Grid { Padding = new Thickness(22, 18, 12, 8) };
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var titles = new StackPanel { Spacing = 0 };
        titles.Children.Add(new TextBlock { Text = T("Customize"), FontSize = 20, FontWeight = FontWeights.SemiBold });
        titles.Children.Add(new TextBlock { Text = AlbumName(_galAlbum), FontSize = 12, Opacity = 0.6, TextTrimming = TextTrimming.CharacterEllipsis });
        head.Children.Add(titles);
        var x = new Button { Content = new FontIcon { Glyph = "", FontSize = 12 }, Width = 34, Height = 34, Padding = new Thickness(0), CornerRadius = new CornerRadius(17), BorderThickness = new Thickness(0),
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x14, 255, 255, 255)), VerticalAlignment = VerticalAlignment.Top };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(x, T("Close"));
        x.Click += (_, _) => ToggleCustomize(false);
        Grid.SetColumn(x, 1);
        head.Children.Add(x);
        root.Children.Add(head);

        // the three parts, as a sliding switch
        string[] tabs = [T("Board"), T("Frames"), T("Writing"), T("Stickers")];
        var track = new Grid { Margin = new Thickness(20, 4, 20, 10), Padding = new Thickness(3), CornerRadius = new CornerRadius(10), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x24, 0, 0, 0)) };
        for (int i = 0; i < tabs.Length; i++) track.ColumnDefinitions.Add(new ColumnDefinition());
        var pill = new Border { CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x30, 255, 255, 255)) };
        Grid.SetColumn(pill, _custTab);
        track.Children.Add(pill);
        for (int i = 0; i < tabs.Length; i++)
        {
            var n = i;
            var t = new Button { Content = tabs[i], HorizontalAlignment = HorizontalAlignment.Stretch, Height = 34, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0), FontSize = 13,
                FontWeight = i == _custTab ? FontWeights.SemiBold : FontWeights.Normal, Opacity = i == _custTab ? 1 : 0.7 };
            t.Click += (_, _) => { _custTab = n; FillCustomize(); };
            Grid.SetColumn(t, i);
            track.Children.Add(t);
        }
        Grid.SetRow(track, 1);
        root.Children.Add(track);

        var body = new StackPanel { Spacing = 10, Padding = new Thickness(22, 4, 22, 24) };
        var scroll = new ScrollViewer { Content = body, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        Grid.SetRow(scroll, 2);
        root.Children.Add(scroll);

        if (_custTab == 0)
        {
            // the colour: ready ones, or any colour from the wheel at any brightness
            body.Children.Add(Caption(T("COLOUR")));
            var swatches = Columns(6, 10);
            var now = BoardColor(look);
            for (int i = 0; i < Palette.Length; i++)
            {
                var c = Hex(Palette[i].Hex, now);
                var on = string.Equals(look.Color ?? Hex(now), Palette[i].Hex, StringComparison.OrdinalIgnoreCase);
                var dot = new Button { Width = 38, Height = 38, HorizontalAlignment = HorizontalAlignment.Center, CornerRadius = new CornerRadius(19), Padding = new Thickness(0),
                    BorderThickness = new Thickness(on ? 3 : 1), BorderBrush = on ? new SolidColorBrush(Microsoft.UI.Colors.White) : new SolidColorBrush(Windows.UI.Color.FromArgb(0x26, 255, 255, 255)),
                    Background = new LinearGradientBrush { StartPoint = new(0.2, 0), EndPoint = new(0.8, 1), GradientStops = { new GradientStop { Color = Shade(c, 0.1), Offset = 0 }, new GradientStop { Color = c, Offset = 0.5 }, new GradientStop { Color = Shade(c, -0.3), Offset = 1 } } } };
                dot.Resources["ButtonBackgroundPointerOver"] = dot.Background;
                dot.Resources["ButtonBackgroundPressed"] = dot.Background;
                dot.Resources["ButtonBorderBrushPointerOver"] = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB0, 255, 255, 255));
                ToolTipService.SetToolTip(dot, T(Palette[i].Name));
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(dot, T(Palette[i].Name));
                var hex = Palette[i].Hex;
                dot.Click += (_, _) => { look.Color = hex; if (_boardFrame is not null) _boardFrame.Background = BoardBrush(look); foreach (var t in _boardTools) InkTool(t, look); Changed(); FillCustomize(); };
                PutAt(swatches, dot, i, 6);
            }
            body.Children.Add(swatches);

            body.Children.Add(Caption(T("YOUR OWN")));
            var (hue, bright, sat) = HueOf(now);
            var wheel = new Microsoft.UI.Xaml.Controls.Primitives.ColorSpectrum
            {
                Shape = ColorSpectrumShape.Ring, Components = ColorSpectrumComponents.HueSaturation, Width = 196, Height = 196, HorizontalAlignment = HorizontalAlignment.Center,
                HsvColor = new System.Numerics.Vector4((float)hue, (float)sat, 1, 1),
            };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(wheel, T("Colour wheel"));
            var shade = new LinearGradientBrush { StartPoint = new(0, 0.5), EndPoint = new(1, 0.5), GradientStops = { new GradientStop { Color = FromHue(hue, 0.04, sat), Offset = 0 }, new GradientStop { Color = FromHue(hue, 1, sat), Offset = 1 } } };
            var brightness = new Slider { Minimum = 4, Maximum = 100, Value = bright * 100, StepFrequency = 0.5 };
            brightness.Resources["SliderTrackFill"] = shade;
            brightness.Resources["SliderTrackFillPointerOver"] = shade;
            brightness.Resources["SliderTrackValueFill"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            brightness.Resources["SliderTrackValueFillPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            brightness.Resources["SliderTrackValueFillPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(brightness, T("Brightness"));
            var hexText = new TextBlock { Text = Hex(now), FontSize = 12, FontFamily = new FontFamily("Cascadia Mono, Consolas"), HorizontalAlignment = HorizontalAlignment.Center, Opacity = 0.7 };
            void Mix()
            {
                var v = wheel.HsvColor;
                var c = FromHue(v.X % 360, brightness.Value / 100, v.Y);
                look.Color = Hex(c);
                hexText.Text = look.Color;
                shade.GradientStops[0].Color = FromHue(v.X % 360, 0.04, v.Y);
                shade.GradientStops[1].Color = FromHue(v.X % 360, 1, v.Y);
                if (_boardFrame is not null) _boardFrame.Background = BoardBrush(look);
                foreach (var t in _boardTools) InkTool(t, look);
                Changed();
            }
            var live = false;
            DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () => live = true);
            wheel.ColorChanged += (_, _) => { if (live) Mix(); };
            brightness.ValueChanged += (_, _) => { if (live) Mix(); };
            body.Children.Add(wheel);
            var shadeRow = new Grid { ColumnSpacing = 10 };
            shadeRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            shadeRow.ColumnDefinitions.Add(new ColumnDefinition());
            shadeRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            shadeRow.Children.Add(new FontIcon { Glyph = "\uE708", FontSize = 13, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center });
            Grid.SetColumn(brightness, 1);
            shadeRow.Children.Add(brightness);
            var sun = new FontIcon { Glyph = "\uE706", FontSize = 15, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(sun, 2);
            shadeRow.Children.Add(sun);
            body.Children.Add(shadeRow);
            body.Children.Add(hexText);

            // the light over the board, each shown on the board's own colour
            body.Children.Add(Caption(T("LIGHT")));
            var lights = Columns(3);
            (string Key, string Name)[] modes = [("soft", T("Soft")), ("spot", T("Spotlight")), ("flat", T("Flat"))];
            for (int i = 0; i < modes.Length; i++)
            {
                var key = modes[i].Key;
                var art = new Grid { Width = 80, Height = 50 };
                art.Children.Add(new Border { CornerRadius = new CornerRadius(6), Background = BoardBrush(look) });
                art.Children.Add(new Border { CornerRadius = new CornerRadius(6), Background = LightBrush(new AlbumLook { Light = key }) });
                PutAt(lights, Choice(art, modes[i].Name, look.Light == key, () => { look.Light = key; Changed(); FillCustomize(); }, 96), i, 3);
            }
            body.Children.Add(lights);

            // the paper under the pictures
            body.Children.Add(Caption(T("PAPER")));
            var papers = Columns(4);
            (string Key, string Name)[] sheets = [("none", T("Plain")), ("lines", T("Lined")), ("grid", T("Squared")), ("dots", T("Dotted"))];
            for (int i = 0; i < sheets.Length; i++)
            {
                var key = sheets[i].Key;
                var art = new Grid { Width = 56, Height = 44 };
                art.Children.Add(new Border { CornerRadius = new CornerRadius(5), Background = BoardBrush(look) });
                var c = new Canvas { Width = 56, Height = 44 };
                DrawPaper(c, new AlbumLook { Pattern = key, Color = look.Color, Background = look.Background }, 56, 44, 9);
                art.Children.Add(c);
                PutAt(papers, Choice(art, sheets[i].Name, look.Pattern == key, () => { look.Pattern = key; Changed(); FillCustomize(); }, 84), i, 4);
            }
            body.Children.Add(papers);
        }
        else if (_custTab == 3)
        {
            // the stickers: a click sticks one where the board is being looked at
            var hint = new TextBlock { Text = T("Click one to stick it on the board. Drag it around; right-click it to make it bigger, smaller or take it off."), FontSize = 12, Opacity = 0.65, TextWrapping = TextWrapping.Wrap };
            body.Children.Add(hint);
            foreach (var (group, set) in StickerSets)
            {
                body.Children.Add(Caption(T(group)));
                var grid = Columns(6, 6);
                for (int i = 0; i < set.Length; i++)
                {
                    var glyph = set[i];
                    var b = new Button { Content = new TextBlock { Text = glyph, FontFamily = new FontFamily("Segoe UI Emoji"), FontSize = 24, HorizontalAlignment = HorizontalAlignment.Center },
                        Height = 46, HorizontalAlignment = HorizontalAlignment.Stretch, Padding = new Thickness(0), CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(0),
                        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x0C, 255, 255, 255)) };
                    Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, T("Sticker"));
                    b.Click += (_, _) => AddSticker(glyph);
                    PutAt(grid, b, i, 6);
                }
                body.Children.Add(grid);
            }
            if (look.Stickers.Count > 0)
            {
                var clear = new HyperlinkButton { Content = T("Take every sticker off"), Margin = new Thickness(0, 8, 0, 0), Foreground = Res("ApexErrorBrush") };
                clear.Click += (_, _) => { look.Stickers.Clear(); Changed(); FillCustomize(); };
                body.Children.Add(clear);
            }
        }
        else if (_custTab == 1)
        {
            // the frames, each drawn small
            body.Children.Add(Caption(T("FRAME")));
            var frames = Columns(2);
            for (int i = 0; i < Frames.Length; i++)
            {
                var key = Frames[i].Key;
                PutAt(frames, Choice(FrameIcon(key), T(Frames[i].Name), look.Frame == key, () => { look.Frame = key; Changed(); FillCustomize(); }), i, 2);
            }
            body.Children.Add(frames);

            body.Children.Add(Caption(T("TAPE")));
            var tapes = Columns(3);
            (string Key, string Name)[] kinds = [("colour", T("Colourful")), ("match", T("Matching")), ("none", T("None"))];
            for (int i = 0; i < kinds.Length; i++)
            {
                var key = kinds[i].Key;
                var art = new Grid { Width = 70, Height = 40 };
                if (key != "none")
                {
                    var probe = new AlbumLook { Tape = key, Color = look.Color, Background = look.Background };
                    art.Children.Add(new Border { Width = 46, Height = 14, Background = TapeBrush(probe, 1), Rotation = -6, CenterPoint = new Vector3(23, 7, 0), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 6, 0, 0) });
                    art.Children.Add(new Border { Width = 40, Height = 12, Background = TapeBrush(probe, 3), Rotation = 5, CenterPoint = new Vector3(20, 6, 0), VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 0, 6) });
                }
                else art.Children.Add(new FontIcon { Glyph = "", FontSize = 18, Opacity = 0.5 });
                PutAt(tapes, Choice(art, kinds[i].Name, look.Tape == key, () => { look.Tape = key; Changed(); FillCustomize(); }, 80), i, 3);
            }
            body.Children.Add(tapes);

            Grid Range(string name, string left, string right, double min, double max, double value, Action<double> set)
            {
                var g = new Grid { ColumnSpacing = 10, Margin = new Thickness(0, 2, 0, 0) };
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                g.ColumnDefinitions.Add(new ColumnDefinition());
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                g.Children.Add(new TextBlock { Text = left, FontSize = 12, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center });
                var s = new Slider { Minimum = min, Maximum = max, StepFrequency = 0.05, Value = value };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(s, name);
                s.ValueChanged += (_, e) => { set(e.NewValue); _custChanged = true; LiveCards(look); };
                Grid.SetColumn(s, 1);
                g.Children.Add(s);
                var r = new TextBlock { Text = right, FontSize = 12, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center };
                Grid.SetColumn(r, 2);
                g.Children.Add(r);
                return g;
            }
            body.Children.Add(Caption(T("PICTURE SIZE")));
            body.Children.Add(Range(T("Picture size"), T("Small"), T("Large"), 0.75, 1.3, look.Size, v => look.Size = v));
            body.Children.Add(Caption(T("LAYOUT")));
            body.Children.Add(Range(T("Tilt: tidy to messy"), T("Tidy"), T("Messy"), 0, 1.6, look.Tilt, v => look.Tilt = v));
        }
        else
        {
            // the handwriting: a few chosen ones, and every font on this PC behind a search
            body.Children.Add(Caption(T("HANDWRITING")));
            var search = new TextBox { PlaceholderText = T("Search the fonts on this PC"), Text = _fontQuery, Margin = new Thickness(0, 0, 0, 4) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(search, T("Search fonts"));
            body.Children.Add(search);
            var list = new StackPanel { Spacing = 8 };
            body.Children.Add(list);
            var all = _fonts ??= Fonts.Installed();
            void Show(string q)
            {
                list.Children.Clear();
                var names = q.Length == 0
                    ? (Hands.Contains(look.Font) ? Hands : [look.Font, .. Hands]).ToList()
                    : all.Where(f => f.Contains(q, StringComparison.OrdinalIgnoreCase)).Take(40).ToList();
                foreach (var f in names)
                {
                    var on = look.Font == f;
                    var g = new Grid { ColumnSpacing = 12 };
                    g.ColumnDefinitions.Add(new ColumnDefinition());
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    var lines = new StackPanel { Spacing = 0 };
                    lines.Children.Add(new TextBlock { Text = AlbumName(_galAlbum), FontFamily = new FontFamily(f), FontSize = 24, TextTrimming = TextTrimming.CharacterEllipsis });
                    lines.Children.Add(new TextBlock { Text = f, FontSize = 11, Opacity = 0.55 });
                    g.Children.Add(lines);
                    if (on) { var tick = new FontIcon { Glyph = "\uE73E", FontSize = 14, Foreground = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center }; Grid.SetColumn(tick, 1); g.Children.Add(tick); }
                    var b = new Button { Content = g, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, Padding = new Thickness(14, 8, 14, 10), CornerRadius = new CornerRadius(12),
                        BorderThickness = new Thickness(on ? 2 : 1), BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Windows.UI.Color.FromArgb(0x1A, 255, 255, 255)),
                        Background = new SolidColorBrush(on ? Windows.UI.Color.FromArgb(0x2E, 0x7F, 0x77, 0xDD) : Windows.UI.Color.FromArgb(0x0C, 255, 255, 255)) };
                    Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, f);
                    var font = f;
                    b.Click += (_, _) => { look.Font = font; Changed(); FillCustomize(); };
                    list.Children.Add(b);
                }
                var more = q.Length == 0 ? T("{0} fonts on this PC: search to use any of them.", all.Count)
                    : names.Count == 0 ? T("No font with that name.") : names.Count == 40 ? T("The first 40; type more to narrow them.") : "";
                if (more.Length > 0) list.Children.Add(new TextBlock { Text = more, FontSize = 12, Opacity = 0.6, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(2, 2, 0, 0) });
            }
            search.TextChanged += (_, _) => { _fontQuery = search.Text.Trim(); Show(_fontQuery); };
            Show(_fontQuery);
        }
        _custDrawer.Child = root;
    }

    // a small drawing of each frame for its card
    static UIElement FrameIcon(string key)
    {
        var paper = new SolidColorBrush(Rgb(0xF2, 0xEF, 0xE8));
        Border Photo(double w, double h, double r = 0) => new() { Width = w, Height = h, CornerRadius = new CornerRadius(r),
            Background = new LinearGradientBrush { StartPoint = new(0, 0), EndPoint = new(1, 1), GradientStops = { new GradientStop { Color = Rgb(0x8F, 0xB4, 0xD8), Offset = 0 }, new GradientStop { Color = Rgb(0x4E, 0x7A, 0x5E), Offset = 1 } } } };
        switch (key)
        {
            case "rounded": return Photo(52, 38, 10);
            case "stamp":
                return new Grid { Width = 46, Height = 50, Children = { new Microsoft.UI.Xaml.Shapes.Rectangle { Fill = new SolidColorBrush(Rgb(0xFB, 0xF8, 0xF1)), Stroke = new SolidColorBrush(Rgb(0x9A, 0x90, 0x7E)), StrokeDashArray = [1.4, 1.2], StrokeThickness = 3 },
                    new Border { Margin = new Thickness(7, 7, 7, 13), Child = Photo(32, 30) } } };
            case "film":
            {
                var f = new StackPanel { Background = new SolidColorBrush(Rgb(0x14, 0x14, 0x14)), Padding = new Thickness(4, 3, 4, 3), Spacing = 3 };
                StackPanel Holes() { var r = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 }; for (int i = 0; i < 6; i++) r.Children.Add(new Border { Width = 5, Height = 3, Background = new SolidColorBrush(Rgb(0x55, 0x55, 0x55)) }); return r; }
                f.Children.Add(Holes()); f.Children.Add(Photo(50, 30)); f.Children.Add(Holes());
                return f;
            }
            case "kraft":
                return new Border { Background = new SolidColorBrush(Rgb(0xC4, 0xA2, 0x7A)), Padding = new Thickness(4, 4, 4, 12), Child = Photo(40, 32) };
            case "washi":
            {
                var g = new Grid { Width = 58, Height = 46 };
                g.Children.Add(new Border { Background = new SolidColorBrush(Microsoft.UI.Colors.White), Padding = new Thickness(3), Margin = new Thickness(4), Child = Photo(44, 32) });
                for (int c = 0; c < 4; c++) g.Children.Add(WashiStrip(c, 0, 0.32));
                return g;
            }
            case "clip":
            {
                var g = new Grid();
                g.Children.Add(new Border { Background = paper, Padding = new Thickness(3, 3, 3, 9), Margin = new Thickness(0, 6, 0, 0), Child = Photo(42, 30) });
                var silver = new SolidColorBrush(Rgb(0xB9, 0xBC, 0xC4));
                g.Children.Add(new Border { Width = 7, Height = 20, BorderBrush = silver, BorderThickness = new Thickness(1.5), CornerRadius = new CornerRadius(4), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(8, 0, 0, 0) });
                return g;
            }
            case "deckle":
            {
                var cream = new SolidColorBrush(Rgb(0xEF, 0xE6, 0xD0));
                var g = new Grid();
                g.Children.Add(new Border { Background = cream, Padding = new Thickness(5, 5, 5, 9), Child = Photo(40, 30) });
                for (int t = 0; t < 2; t++)
                {
                    var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = t == 0 ? VerticalAlignment.Top : VerticalAlignment.Bottom, Margin = new Thickness(0, t == 0 ? -2.5 : 0, 0, t == 0 ? 0 : -2.5) };
                    for (int c = 0; c < 10; c++) row.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 5, Height = 5, Fill = cream });
                    g.Children.Add(row);
                }
                return g;
            }
            case "doodle":
            {
                var ink = new SolidColorBrush(Rgb(0xE8, 0xE2, 0xD4));
                var g = new Grid { Padding = new Thickness(5), Width = 54, Height = 42 };
                g.Children.Add(Photo(44, 32));
                g.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = Geo(DoodleA), Stroke = ink, StrokeThickness = 1.4, Stretch = Stretch.Fill, Margin = new Thickness(-5) });
                g.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = Geo(Star), Fill = ink, Width = 8, Height = 8, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, -9, -9) });
                return g;
            }
            case "print":
                return new Border { BorderBrush = paper, BorderThickness = new Thickness(3), CornerRadius = new CornerRadius(5), Child = Photo(48, 34, 2) };
            default:
                return new Border { Background = paper, Padding = new Thickness(4, 4, 4, 12), Child = Photo(40, 34) };
        }
    }

    // ---------- the paper and the stickers ----------

    static string E(int code) => char.ConvertFromUtf32(code);
    static readonly (string Group, string[] Set)[] StickerSets =
    [
        ("NATURE", [E(0x1F338), E(0x1F33B), E(0x1F337), E(0x1F33F), E(0x1F342), E(0x1F335), E(0x1F334), E(0x1F308), E(0x2600), E(0x1F319), E(0x2B50), E(0x2728)]),
        ("LOVE AND PARTIES", [E(0x1F496), E(0x1F48C), E(0x1F48D), E(0x1F339), E(0x1F388), E(0x1F389), E(0x1F382), E(0x1F381), E(0x1F942), E(0x1F3B5), E(0x1F3C6), E(0x1F451)]),
        ("LIFE", [E(0x1F3E0), E(0x1F476), E(0x1F431), E(0x1F436), E(0x1F98B), E(0x2615), E(0x1F370), E(0x1F355), E(0x1F4F7), E(0x1F697), E(0x2708), E(0x1F3D6)]),
        ("FACES", [E(0x1F60A), E(0x1F602), E(0x1F60D), E(0x1F60E), E(0x1F622), E(0x1F631), E(0x1F914), E(0x1F634), E(0x1F973), E(0x1F47B), E(0x1F47D), E(0x1F525)]),
    ];

    // lined, squared or dotted paper drawn over a size, a line every step
    static void DrawPaper(Canvas c, AlbumLook look, double w, double h, double step)
    {
        c.Children.Clear();
        if (look.Pattern is not ("lines" or "grid" or "dots")) return;
        var ink = new SolidColorBrush(IsLight(BoardColor(look)) ? Windows.UI.Color.FromArgb(0x30, 0x3A, 0x34, 0x2A) : Windows.UI.Color.FromArgb(0x1C, 0xFF, 0xFF, 0xFF));
        if (look.Pattern == "dots")
        {
            // shortcut: one dot per cell is a few thousand elements on a large window; fine for a board, not for a wall
            for (double y = step; y < h; y += step)
                for (double x = step; x < w; x += step)
                {
                    var d = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 2.4, Height = 2.4, Fill = ink };
                    Canvas.SetLeft(d, x); Canvas.SetTop(d, y);
                    c.Children.Add(d);
                }
            return;
        }
        for (double y = step; y < h; y += step) { var l = new Border { Width = w, Height = 1, Background = ink }; Canvas.SetTop(l, y); c.Children.Add(l); }
        if (look.Pattern == "grid")
            for (double x = step; x < w; x += step) { var l = new Border { Width = 1, Height = h, Background = ink }; Canvas.SetLeft(l, x); c.Children.Add(l); }
        else
        {
            // (the notebook's margin line)
            var m = new Border { Width = 1.5, Height = h, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x55, 0xE0, 0x6A, 0x6A)) };
            Canvas.SetLeft(m, step * 3);
            c.Children.Add(m);
        }
    }

    // a sticker where the board is being looked at
    void AddSticker(string glyph)
    {
        if (_gal is null) return;
        var look = Gallery.Look(_gal, _galAlbum);
        var rnd = new Random();
        var left = (_boardScroll?.HorizontalOffset ?? 0) + (_boardScroll?.ViewportWidth ?? 800) * (0.25 + rnd.NextDouble() * 0.4);
        var s = new Sticker { Text = glyph, X = left, Y = BoardHeight * (0.15 + rnd.NextDouble() * 0.55), Angle = rnd.Next(-14, 15) };
        look.Stickers.Add(s);
        Gallery.Save(_gal);
        _boardCanvas?.Children.Add(StickerOn(s, look, 0));
    }

    // a sticker on the board: it lands like the rest, is dragged, and its menu makes it bigger, smaller or takes it off
    FrameworkElement StickerOn(Sticker s, AlbumLook look, int order)
    {
        var text = new TextBlock { Text = s.Text, FontFamily = new FontFamily("Segoe UI Emoji"), FontSize = 64 * s.Size * _boardScale };
        var tilt = new Border { Child = text };
        var outer = new Border { Child = tilt, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        Canvas.SetLeft(outer, s.X);
        Canvas.SetTop(outer, s.Y);
        Canvas.SetZIndex(outer, 700 + order);
        Land(tilt, s.Angle, 200 + order * 90, 0);
        Draggable(outer, () => { s.X = Canvas.GetLeft(outer); s.Y = Canvas.GetTop(outer); Gallery.Save(_gal!); });
        var menu = new MenuFlyout();
        void Resize(double by) { s.Size = Math.Clamp(s.Size * by, 0.5, 3); text.FontSize = 64 * s.Size * _boardScale; Gallery.Save(_gal!); }
        var bigger = new MenuFlyoutItem { Text = T("Bigger"), Icon = new FontIcon { Glyph = "\uE8A3" } };
        bigger.Click += (_, _) => Resize(1.25);
        var smaller = new MenuFlyoutItem { Text = T("Smaller"), Icon = new FontIcon { Glyph = "\uE71F" } };
        smaller.Click += (_, _) => Resize(0.8);
        var off = new MenuFlyoutItem { Text = T("Take it off"), Icon = new FontIcon { Glyph = "\uE74D" } };
        off.Click += (_, _) => { look.Stickers.Remove(s); Gallery.Save(_gal!); _boardCanvas?.Children.Remove(outer); };
        menu.Items.Add(bigger);
        menu.Items.Add(smaller);
        menu.Items.Add(off);
        outer.ContextFlyout = menu;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(outer, T("Sticker"));
        return outer;
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
                if (_boardPaper is not null && _boardFrame is not null) DrawPaper(_boardPaper, look, _boardFrame.ActualWidth, _boardFrame.ActualHeight, 28);
                if (_boardTitle is not null) { _boardTitle.FontFamily = new FontFamily(look.Font); _boardTitle.Foreground = Ink(look); }
                if (_boardHint is not null) _boardHint.Foreground = Ink(look);
                foreach (var b in _boardTools) InkTool(b, look);
            }
            finally { _boardCalm = false; }
        });
    }

    Border? _boardLight;
    Canvas? _boardPaper;
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

    double _boardRoom, _boardFrom;
    Grid? _galPage;
    double BoardFits() => Math.Clamp((_boardRoom > 0 ? _boardRoom : Scroller.ActualHeight - 120) - 62, 170, 1400);

    void FullBleed(bool on)
    {
        Scroller.Padding = on ? new Thickness(0) : new Thickness(32, 28, 32, 0);
        Screens.Margin = on ? new Thickness(0) : new Thickness(0, 0, 0, 32);
        Scroller.VerticalScrollBarVisibility = on ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto;
        Scroller.VerticalScrollMode = on ? ScrollMode.Disabled : ScrollMode.Enabled;
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
        if (look.Layout < 4) { look.Places.Clear(); look.Layout = 4; }
        var ink = Ink(look);
        // as tall as the window allows, the pictures sized with it
        BoardHeight = BoardFits();
        // (pictures and notes placed on a board of another height keep their place on this one)
        if (look.Height > 0 && Math.Abs(look.Height - BoardHeight) > 1)
        {
            var k = BoardHeight / look.Height;
            foreach (var pl in look.Places.Values) { pl.X *= k; pl.Y *= k; }
            foreach (var nt in look.Notes) { nt.X *= k; nt.Y *= k; }
            foreach (var sk in look.Stickers) { sk.X *= k; sk.Y *= k; }
        }
        look.Height = BoardHeight;
        var s = _boardScale = BoardHeight / 660;
        _boardBuiltSize = look.Size;
        var mid = BoardHeight / 2;
        // the newest sixty, laid newest first along a gentle wave around the line, a breath of room where a day begins
        var pics = InAlbum().Take(60).ToList();
        var xs = new double[pics.Count];
        var days = new List<(double X, DateTime Day)>();
        var x = 110 * s;
        double last = 0;
        for (int i = 0; i < pics.Count; i++)
        {
            if (i == 0 || pics[i].Taken.Date != pics[i - 1].Taken.Date) { if (i > 0) x += 90 * s; days.Add((x, pics[i].Taken.Date)); }
            xs[i] = x;
            last = ((i % 4 == 0 ? 290 : 230) * s * look.Size + 24);
            x += last * 0.95;
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
                Child = new TextBlock { Text = day.ToString("d MMM", LangCulture()), FontFamily = new FontFamily(look.Font), FontSize = 22 * Math.Max(0.85, s), Foreground = ink } };
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
            var delay = 120 + Math.Min(i, 10) * 130;
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
            // (a bracket drawn in the board's ink over the album's pictures, its name on a tag that opens it)
            var chapter = new Grid { Width = to - from, Height = 30 };
            chapter.Children.Add(new Border { Height = 12, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 14, 0, 0), BorderBrush = ink, BorderThickness = new Thickness(1.5, 1.5, 1.5, 0),
                CornerRadius = new CornerRadius(6, 6, 0, 0), Opacity = 0.5, IsHitTestVisible = false });
            var tagRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            tagRow.Children.Add(new FontIcon { Glyph = c.Icon, FontSize = 14, Foreground = ink, VerticalAlignment = VerticalAlignment.Center });
            var tagName = new TextBlock { Text = c.Name, FontFamily = new FontFamily(look.Font), FontSize = 20, Foreground = ink, VerticalAlignment = VerticalAlignment.Center };
            tagRow.Children.Add(tagName);
            tagRow.Children.Add(new TextBlock { Text = spots.Count.ToString(), FontSize = 12, Foreground = ink, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 2, 0, 0) });
            var tag = new HyperlinkButton { Content = tagRow, Padding = new Thickness(12, 0, 14, 2), Margin = new Thickness(18, 0, 0, 0), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
                Background = pill, CornerRadius = new CornerRadius(14), Height = 30 };
            tag.Resources["HyperlinkButtonBackgroundPointerOver"] = pill;
            tag.Resources["HyperlinkButtonBackgroundPressed"] = pill;
            tag.Resources["HyperlinkButtonForegroundPointerOver"] = ink;
            tag.Resources["HyperlinkButtonForegroundPressed"] = ink;
            tag.PointerEntered += (_, _) => tagName.TextDecorations = Windows.UI.Text.TextDecorations.Underline;
            tag.PointerExited += (_, _) => tagName.TextDecorations = Windows.UI.Text.TextDecorations.None;
            ToolTipService.SetToolTip(tag, T("Open the album {0}", c.Name));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(tag, T("Open the album {0}", c.Name));
            var key = c.Key;
            tag.Click += (_, _) => { _galAlbum = key; BuildGallery(); };
            chapter.Children.Add(tag);
            Canvas.SetLeft(chapter, from);
            Canvas.SetTop(chapter, 10 + lane * 40);
            Canvas.SetZIndex(chapter, 950);
            canvas.Children.Add(chapter);
            if (!_boardCalm)
            {
                chapter.Opacity = 0;
                var show = new Storyboard { BeginTime = TimeSpan.FromMilliseconds(700) };
                Tween(show, chapter, "Opacity", 1, 400);
                Play(show);
            }
        }
        var n = 0;
        foreach (var note in look.Notes) canvas.Children.Add(Sticky(note, look, n++));
        var st = 0;
        foreach (var sk in look.Stickers) canvas.Children.Add(StickerOn(sk, look, st++));

        var scroll = new ScrollViewer { Content = canvas, HorizontalScrollMode = ScrollMode.Enabled, HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden,
            VerticalScrollMode = ScrollMode.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled, ZoomMode = ZoomMode.Disabled };
        _boardScroll = scroll;
        // the mouse wheel moves along the timeline
        double heading = -1;
        scroll.AddHandler(UIElement.PointerWheelChangedEvent, new PointerEventHandler((_, e) =>
        {
            var w = e.GetCurrentPoint(scroll).Properties;
            var d = w.IsHorizontalMouseWheel ? -w.MouseWheelDelta : w.MouseWheelDelta;
            var from = heading >= 0 && Math.Abs(heading - scroll.HorizontalOffset) > 2 ? heading : scroll.HorizontalOffset;
            heading = Math.Clamp(from - d * 1.6, 0, scroll.ScrollableWidth);
            scroll.ChangeView(heading, null, null, false);
            e.Handled = true;
        }), true);
        var opened = false;
        var keep = _boardKeep;
        _boardKeep = null;
        var zoom = _boardCalm && _boardFrom > 0 ? _boardFrom / BoardHeight : 1;
        _boardFrom = 0;
        scroll.LayoutUpdated += (_, _) =>
        {
            if (opened || scroll.ScrollableWidth <= 0) return;
            opened = true;
            scroll.ChangeView((keep ?? 0) * scroll.ScrollableWidth, null, null, true);
            if (Math.Abs(zoom - 1) < 0.01) return;
            var z = new CompositeTransform { ScaleX = zoom, ScaleY = zoom, CenterX = (keep ?? 0) * scroll.ScrollableWidth + scroll.ViewportWidth / 2, CenterY = mid };
            canvas.RenderTransform = z;
            var grow = new Storyboard();
            Tween(grow, z, "ScaleX", 1, 520, new CubicEase { EasingMode = EasingMode.EaseOut });
            Tween(grow, z, "ScaleY", 1, 520, new CubicEase { EasingMode = EasingMode.EaseOut });
            Play(grow);
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
                var label = new TextBlock { Text = d.ToString("d MMM", LangCulture()), FontSize = 11, Foreground = ink, Opacity = 0.75 };
                Canvas.SetLeft(label, at + 5); Canvas.SetTop(label, 0);
                track.Children.Add(label);
                lastRight = at + 5 + d.ToString("d MMM", LangCulture()).Length * 6.5;
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
                mark.Children.Add(new TextBlock { Text = p.Taken.ToString("dd MMM\nyyyy", LangCulture()).ToUpperInvariant(), FontSize = 10, FontWeight = FontWeights.Bold, TextAlignment = TextAlignment.Center, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(0xC0, 0x6A, 0x4E, 0x8C)), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
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
            case "print":
                // (a print with a thin white border, the caption written on the board under it)
                var print = new Border { BorderBrush = new SolidColorBrush(Rgb(0xF7, 0xF4, 0xEE)), BorderThickness = new Thickness(6), CornerRadius = new CornerRadius(8),
                    Child = new Border { Width = w, Height = h, CornerRadius = new CornerRadius(3), Background = new ImageBrush { ImageSource = source, Stretch = Stretch.UniformToFill } } };
                print.Shadow = new ThemeShadow();
                stack.Children.Add(print);
                caption.Foreground = ink;
                caption.Margin = new Thickness(4, 10, 0, 0);
                stack.Children.Add(caption);
                body = stack;
                break;
            case "kraft":
                // (brown paper, taped on)
                stack.Children.Add(new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h });
                caption.Foreground = new SolidColorBrush(Rgb(0x3D, 0x2B, 0x1A));
                caption.Margin = new Thickness(2, 8, 2, 0);
                stack.Children.Add(caption);
                body = new Border { Background = new SolidColorBrush(Rgb(0xC4, 0xA2, 0x7A)), Padding = new Thickness(12, 12, 12, 13), Child = stack };
                tape = true;
                break;
            case "washi":
                // (a white border held by four strips of patterned washi tape, the caption on the board under it)
                var wash = new Grid();
                var washPaper = new Border { Background = new SolidColorBrush(Microsoft.UI.Colors.White), Padding = new Thickness(8), Child = new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h } };
                washPaper.Shadow = new ThemeShadow();
                wash.Children.Add(washPaper);
                for (int q = 0; q < 4; q++) wash.Children.Add(WashiStrip(q, order, _boardScale));
                stack.Children.Add(wash);
                caption.Foreground = ink;
                caption.Margin = new Thickness(4, 12, 0, 0);
                stack.Children.Add(caption);
                body = stack;
                break;
            case "clip":
                // (a white print with a silver paper clip over its top edge)
                var clipped = new Grid();
                var card = new StackPanel { Background = new SolidColorBrush(Rgb(0xFD, 0xFD, 0xFB)), Padding = new Thickness(9, 9, 9, 6) };
                card.Children.Add(new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h });
                caption.Foreground = dark;
                caption.Margin = new Thickness(3, 8, 0, 2);
                card.Children.Add(caption);
                clipped.Children.Add(card);
                var silver = new SolidColorBrush(Rgb(0xB9, 0xBC, 0xC4));
                var k = _boardScale;
                clipped.Children.Add(new Border
                {
                    Width = 18 * k, Height = 56 * k, BorderBrush = silver, BorderThickness = new Thickness(2.5), CornerRadius = new CornerRadius(9 * k),
                    HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(26 * k, -20 * k, 0, 0),
                    RenderTransform = new RotateTransform { Angle = 6 }, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5),
                    Child = new Border { Width = 8 * k, Height = 36 * k, BorderBrush = silver, BorderThickness = new Thickness(2.5), CornerRadius = new CornerRadius(6 * k), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(1.5, 5 * k, 0, 0) },
                });
                body = clipped;
                break;
            case "deckle":
                // (an old print: a cream border with a scalloped edge, a touch of sepia, the caption on the border)
                var cream = Rgb(0xEF, 0xE6, 0xD0);
                var aged = new Grid();
                var print2 = new StackPanel { Background = new SolidColorBrush(cream), Padding = new Thickness(12, 12, 12, 10) };
                var tinted = new Grid { Width = w, Height = h };
                tinted.Children.Add(new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h });
                tinted.Children.Add(new Border { Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x38, 0x8A, 0x5A, 0x20)) });
                print2.Children.Add(tinted);
                caption.Foreground = new SolidColorBrush(Rgb(0x6B, 0x5A, 0x3A));
                caption.Margin = new Thickness(2, 6, 0, 0);
                print2.Children.Add(caption);
                aged.Children.Add(print2);
                StackPanel Scallops(bool top)
                {
                    var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = top ? VerticalAlignment.Top : VerticalAlignment.Bottom, Margin = new Thickness(0, top ? -5 : 0, 0, top ? 0 : -5) };
                    for (int q = 0; q < (int)((w + 24) / 10); q++) row.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 10, Height = 10, Fill = new SolidColorBrush(cream) });
                    return row;
                }
                aged.Children.Add(Scallops(true));
                aged.Children.Add(Scallops(false));
                body = aged;
                break;
            case "doodle":
                // (no paper: a border drawn by hand round the picture, and a couple of stars, in the board's own ink)
                var drawn = new Grid { Padding = new Thickness(14), Width = w + 28, Height = h + 28 };
                drawn.Children.Add(new Image { Source = source, Stretch = Stretch.UniformToFill, Width = w, Height = h });
                drawn.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = Geo(DoodleA), Stroke = ink, StrokeThickness = 2, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round, Stretch = Stretch.Fill, Margin = new Thickness(-14) });
                drawn.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = Geo(DoodleB), Stroke = ink, StrokeThickness = 1.2, Opacity = 0.6, Stretch = Stretch.Fill, Margin = new Thickness(-14) });
                drawn.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = Geo(Star), Fill = ink, Width = 16, Height = 16, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, -22, -26) });
                drawn.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = Geo(Star), Fill = ink, Opacity = 0.7, Width = 11, Height = 11, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(-20, 0, 0, -22) });
                stack.Children.Add(drawn);
                caption.Foreground = ink;
                caption.HorizontalAlignment = HorizontalAlignment.Center;
                caption.Margin = new Thickness(0, 4, 0, 0);
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
        if (look.Frame is not ("film" or "print" or "washi" or "doodle")) body.Shadow = new ThemeShadow();
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
            Play(tapeIn);
        }
        if (_boardCalm) caption.Text = text; else After(delay + 820, () => Write(caption, text));

        // (under the pointer it lifts, grows a touch and nearly straightens; back to its tilt when left)
        outer.PointerEntered += (_, _) => { if (_dragging) return; outer.Translation = new Vector3(0, -12, 56); outer.Scale = new Vector3(1.04f, 1.04f, 1); Canvas.SetZIndex(outer, 900); Turn(turn, place.Angle * look.Tilt * 0.25); };
        outer.PointerExited += (_, _) => { if (_dragging) return; outer.Translation = new Vector3(0, 0, 32); outer.Scale = Vector3.One; Canvas.SetZIndex(outer, order); Turn(turn, place.Angle * look.Tilt, 420, new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 }); };
        Draggable(outer, () => { place.X = Canvas.GetLeft(outer); place.Y = Canvas.GetTop(outer); Gallery.Save(_gal!); });
        outer.Tapped += (_, e) => { e.Handled = true; OpenViewer(pics, pics.IndexOf(p), outer); };
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
            Play(away);
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
        Play(sb);
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

    // A picture large, with what can be done with it beside. Opened from a card or a tile it grows out of it.
    void OpenViewer(List<Picture> pics, int at, FrameworkElement? from = null)
    {
        if (pics.Count == 0 || Content.XamlRoot is not { } root) return;
        var culture = LangCulture();
        var popup = new Popup { XamlRoot = root, IsLightDismissEnabled = false };
        var shell = new Grid { Width = root.Size.Width, Height = root.Size.Height, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xEB, 0x05, 0x06, 0x08)), OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) }, Opacity = 0 };
        shell.ColumnDefinitions.Add(new ColumnDefinition());
        shell.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(372) });
        var stage = new Grid { Padding = new Thickness(56, 48, 56, 48) };
        var zt = new CompositeTransform();
        var image = new Image { Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            RenderTransform = zt, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5) };
        // (from a scale and an offset back to the picture's own place)
        void Settle(double scale, double dx, double dy, int ms)
        {
            zt.ScaleX = zt.ScaleY = scale;
            zt.TranslateX = dx;
            zt.TranslateY = dy;
            var sb = new Storyboard();
            var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
            Tween(sb, zt, "ScaleX", 1, ms, ease);
            Tween(sb, zt, "ScaleY", 1, ms, ease);
            Tween(sb, zt, "TranslateX", 0, ms, ease);
            Tween(sb, zt, "TranslateY", 0, ms, ease);
            Tween(sb, image, "Opacity", 1, 220);
            Play(sb);
        }
        stage.Children.Add(image);
        Button Round(string glyph, string name, double size = 46)
        {
            var b = new Button { Content = new FontIcon { Glyph = glyph, FontSize = 15 }, Width = size, Height = size, CornerRadius = new CornerRadius(size / 2), Padding = new Thickness(0),
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x1F, 255, 255, 255)), BorderThickness = new Thickness(0) };
            ToolTipService.SetToolTip(b, name);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
            return b;
        }
        var prev = Round("", T("Previous picture"));
        var next = Round("", T("Next picture"));
        prev.HorizontalAlignment = HorizontalAlignment.Left; prev.VerticalAlignment = VerticalAlignment.Center; prev.Margin = new Thickness(-36, 0, 0, 0);
        next.HorizontalAlignment = HorizontalAlignment.Right; next.VerticalAlignment = VerticalAlignment.Center; next.Margin = new Thickness(0, 0, -36, 0);
        stage.Children.Add(prev);
        stage.Children.Add(next);
        var close = Round("", T("Close"), 40);
        close.HorizontalAlignment = HorizontalAlignment.Right; close.VerticalAlignment = VerticalAlignment.Top; close.Margin = new Thickness(0, -32, -40, 0);
        stage.Children.Add(close);
        shell.Children.Add(stage);

        var panel = new Grid { Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1, 0, 0, 0) };
        panel.RowDefinitions.Add(new RowDefinition());
        panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var side = new StackPanel { Spacing = 20, Padding = new Thickness(28, 40, 28, 20) };
        panel.Children.Add(new ScrollViewer { Content = side, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        var foot = new Grid { Padding = new Thickness(28, 14, 20, 18), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        Grid.SetRow(foot, 1);
        panel.Children.Add(foot);
        Grid.SetColumn(panel, 1);
        shell.Children.Add(panel);
        popup.Child = shell;

        // (back on the scrapbook at the same stretch of the timeline, nothing falling in again; another album opens at its start)
        void Close(bool stay = true)
        {
            shell.Opacity = 0;
            var away = new Storyboard();
            Tween(away, zt, "ScaleX", 0.96, 220);
            Tween(away, zt, "ScaleY", 0.96, 220);
            Play(away);
            After(220, () => { popup.IsOpen = false; if (stay && _galBook) RefreshBoard(page: true); else BuildGallery(); });
        }
        close.Click += (_, _) => Close();

        // (the first picture grows out of what was clicked; the next ones settle in place)
        Windows.Foundation.Rect? grow = from is { ActualWidth: > 0 } f ? f.TransformToVisual(null).TransformBounds(new Windows.Foundation.Rect(0, 0, f.ActualWidth, f.ActualHeight)) : null;
        void Show(string path)
        {
            image.Opacity = 0;
            if (grow is not { } src)
            {
                image.Source = new BitmapImage(new Uri(path));
                DispatcherQueue.TryEnqueue(() => Settle(0.94, 0, 0, 360));
                return;
            }
            grow = null;
            RoutedEventHandler? opened = null;
            opened = (_, _) =>
            {
                image.ImageOpened -= opened;
                image.UpdateLayout();
                if (image.ActualWidth <= 0) { image.Opacity = 1; return; }
                var dst = image.TransformToVisual(null).TransformBounds(new Windows.Foundation.Rect(0, 0, image.ActualWidth, image.ActualHeight));
                var k = Math.Clamp(src.Width / dst.Width, 0.05, 1);
                image.Opacity = 1;
                Settle(k, src.X + src.Width / 2 - (dst.X + dst.Width / 2), src.Y + src.Height / 2 - (dst.Y + dst.Height / 2), 520);
            };
            image.ImageOpened += opened;
            image.Source = new BitmapImage(new Uri(path));
        }

        // an action tile: an icon over its name
        Button Action(string glyph, string name, bool on = false)
        {
            var g = new StackPanel { Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            var icon = new FontIcon { Glyph = glyph, FontSize = 20 };
            if (on) icon.Foreground = Res("ApexAccentBrush");
            g.Children.Add(icon);
            g.Children.Add(new TextBlock { Text = name, FontSize = 12, TextAlignment = TextAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
            var b = new Button { Content = g, HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, Height = 78, CornerRadius = new CornerRadius(12), Padding = new Thickness(6),
                Background = new SolidColorBrush(on ? Windows.UI.Color.FromArgb(0x33, 0x7F, 0x77, 0xDD) : Windows.UI.Color.FromArgb(0x0F, 255, 255, 255)),
                BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Windows.UI.Color.FromArgb(0x14, 255, 255, 255)), BorderThickness = new Thickness(1) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
            return b;
        }

        void Fill()
        {
            var p = pics[at];
            var info = Gallery.Info(_gal!, p.Path);
            Show(p.Path);
            side.Children.Clear();

            // when: the day large, the weekday and time above it, where it is in the album beside
            var when = new StackPanel { Spacing = 2 };
            when.Children.Add(new TextBlock { Text = (p.Taken.ToString("dddd", culture) + "  ·  " + p.Taken.ToString("t", culture)).ToUpper(culture), FontSize = 11, FontWeight = FontWeights.SemiBold, CharacterSpacing = 80, Foreground = Res("ApexMutedBrush") });
            var day = new TextBlock { FontSize = 26, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
            day.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = p.Taken.ToString("M", culture) });
            day.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "  " + p.Taken.Year, Foreground = Res("ApexMutedBrush"), FontWeight = FontWeights.Normal });
            when.Children.Add(day);
            side.Children.Add(when);

            // where it belongs: its place in the album, and its household (a menu to change it)
            var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, -6, 0, 0) };
            chips.Children.Add(new Border { CornerRadius = new CornerRadius(12), Padding = new Thickness(10, 3, 10, 4), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x14, 255, 255, 255)),
                Child = new TextBlock { Text = T("{0} of {1}", at + 1, pics.Count), FontSize = 12, Foreground = Res("ApexSoftTextBrush") } });
            var names = _galSaves.Select(s => s.Name).ToList();
            var guess = Gallery.GuessHousehold(p, _galSaves);
            var house = Gallery.HouseholdOf(p, _gal!, _galSaves);
            var who = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            who.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center });
            who.Children.Add(new TextBlock { Text = house ?? T("No household"), FontSize = 12 });
            var homeMenu = new MenuFlyout();
            foreach (var n in names)
            {
                var item = new ToggleMenuFlyoutItem { Text = n, IsChecked = string.Equals(n, house, StringComparison.OrdinalIgnoreCase) };
                var chosen = n;
                item.Click += (_, _) => { info.Household = chosen == guess ? null : chosen; Gallery.Save(_gal!); Fill(); };
                homeMenu.Items.Add(item);
            }
            var whoButton = new DropDownButton { Content = who, Flyout = homeMenu, CornerRadius = new CornerRadius(12), Padding = new Thickness(10, 2, 8, 3), MinHeight = 0, FontSize = 12 };
            ToolTipService.SetToolTip(whoButton, T("The household this picture belongs to"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(whoButton, T("Household"));
            chips.Children.Add(whoButton);
            side.Children.Add(chips);

            // the caption, written by hand as on the board
            var look = Gallery.Look(_gal!, _galAlbum);
            var cap = new TextBox { Text = info.Caption ?? "", PlaceholderText = T("Write something about this moment…"), AcceptsReturn = true, TextWrapping = TextWrapping.Wrap,
                FontFamily = new FontFamily(look.Font), FontSize = 20, MinHeight = 96, Padding = new Thickness(14, 10, 14, 10), CornerRadius = new CornerRadius(12) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(cap, T("Caption"));
            cap.LostFocus += (_, _) => { info.Caption = cap.Text.Trim(); Gallery.Save(_gal!); };
            side.Children.Add(cap);

            // what to do with it, as tiles
            var grid = new Grid { ColumnSpacing = 10, RowSpacing = 10 };
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.RowDefinitions.Add(new RowDefinition());
            grid.RowDefinitions.Add(new RowDefinition());
            var fav = Action(info.Favourite ? "" : "", info.Favourite ? T("In Favourites") : T("Favourite"), info.Favourite);
            fav.Click += (_, _) => { info.Favourite = !info.Favourite; Gallery.Save(_gal!); Fill(); };
            var inAlbums = _gal!.Custom.Where(c => c.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase)).ToList();
            var album = Action("", T("Add to an album"), inAlbums.Count > 0);
            var albums = new MenuFlyout();
            albums.Opening += (_, _) =>
            {
                albums.Items.Clear();
                foreach (var c in _gal.Custom)
                {
                    var t = new ToggleMenuFlyoutItem { Text = c.Name, IsChecked = c.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase), Icon = new FontIcon { Glyph = c.Icon } };
                    var one = c;
                    t.Click += (_, _) => { Gallery.Toggle(_gal, one, p.Path); Fill(); };
                    albums.Items.Add(t);
                }
                if (_gal.Custom.Count > 0) albums.Items.Add(new MenuFlyoutSeparator());
                var make = new MenuFlyoutItem { Text = T("New album…"), Icon = new FontIcon { Glyph = "" } };
                make.Click += async (_, _) => { popup.IsOpen = false; if (await AlbumDialog(null, Gallery.HouseholdOf(p, _gal, _galSaves)) is { } made) Gallery.Toggle(_gal, made, p.Path); popup.IsOpen = true; Fill(); };
                albums.Items.Add(make);
            };
            album.Flyout = albums;
            var cover = _gal.HomePicture == p.Path;
            var home = Action("", cover ? T("Home picture") : T("Use on Home"), cover);
            home.Click += (_, _) => { _gal.HomePicture = p.Path; Gallery.Save(_gal); Fill(); };
            var folder = Action("", T("Show in folder"));
            folder.Click += (_, _) => Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{p.Path}\"") { UseShellExecute = true });
            Grid.SetColumn(album, 1);
            Grid.SetRow(home, 1);
            Grid.SetRow(folder, 1); Grid.SetColumn(folder, 1);
            grid.Children.Add(fav); grid.Children.Add(album); grid.Children.Add(home); grid.Children.Add(folder);
            side.Children.Add(grid);

            // the albums it is in, each opening its album
            if (inAlbums.Count > 0)
            {
                var head = Eyebrow(T("IN ALBUMS"));
                head.Margin = new Thickness(0, 4, 0, -10);
                side.Children.Add(head);
                var wrap = new StackPanel { Spacing = 6 };
                foreach (var c in inAlbums)
                {
                    var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
                    row.Children.Add(new FontIcon { Glyph = c.Icon, FontSize = 14, Foreground = new SolidColorBrush(Rgb(0xB4, 0xAE, 0xF2)) });
                    row.Children.Add(new TextBlock { Text = c.Name, FontSize = 13 });
                    var go = new HyperlinkButton { Content = row, Padding = new Thickness(4, 2, 4, 2) };
                    var key = c.Key;
                    go.Click += (_, _) => { _galAlbum = key; Close(stay: false); };
                    wrap.Children.Add(go);
                }
                side.Children.Add(wrap);
            }

            // the file, and the way out of the gallery for it
            foot.Children.Clear();
            foot.ColumnDefinitions.Clear();
            foot.ColumnDefinitions.Add(new ColumnDefinition());
            foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var file = new TextBlock { Text = Path.GetFileName(p.Path), FontSize = 11, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
            ToolTipService.SetToolTip(file, p.Path);
            foot.Children.Add(file);
            var bin = new Button { Content = new FontIcon { Glyph = "", FontSize = 14, Foreground = Res("ApexErrorBrush") }, Width = 36, Height = 36, Padding = new Thickness(0), CornerRadius = new CornerRadius(18),
                Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0) };
            ToolTipService.SetToolTip(bin, T("Move to the Recycle Bin"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(bin, T("Move to the Recycle Bin"));
            bin.Click += async (_, _) =>
            {
                if (!await Confirm(T("Move this picture to the Recycle Bin?"), T("You can put it back from the Recycle Bin."), T("Move it"))) return;
                try { Gallery.Remove(_gal!, p.Path); pics.RemoveAt(at); if (pics.Count == 0) { Close(); return; } at = Math.Min(at, pics.Count - 1); Fill(); }
                catch (Exception e) { App.Log(e); await Message(T("The picture was not moved"), ErrorText(e)); }
            };
            Grid.SetColumn(bin, 1);
            foot.Children.Add(bin);
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
