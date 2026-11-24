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
    bool _galRestored;
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
        _spotlightOpen = false; // (laid again: no album stays open over it)
        var host = ScreenGallery;
        host.Children.Clear();
        if (_game is null) { host.Children.Add(Heading(T("Gallery"))); host.Children.Add(Muted(T("Choose the game first."))); return; }
        _gal ??= Gallery.Load();
        if (!_galRestored)
        {
            // (Home may have read the gallery already; the view and album come back once, on the first visit)
            _galRestored = true;
            _galBook = _gal.Book;
            if (_gal.LastAlbum is { Length: > 0 } last) _galAlbum = last;
        }
        // (an album since deleted opens All pictures instead; the album shown is remembered for next time)
        if (_galAlbum.StartsWith(Gallery.Mine, StringComparison.Ordinal) && Gallery.CustomOf(_gal, _galAlbum) is null) _galAlbum = Gallery.All;
        if (_gal.LastAlbum != _galAlbum) { _gal.LastAlbum = _galAlbum; _lastKept = null; _undoSteps.Clear(); _redoSteps.Clear(); Gallery.Save(_gal); }
        _lastKept ??= System.Text.Json.JsonSerializer.Serialize(_gal, SetupJson.Default.GalleryData);
        _galPics = Gallery.List(Docs);
        _galSaves = Saves.List(Docs);
        var page = new Grid { ColumnSpacing = 28 };
        page.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(248) }); // (the Mods pane's width)
        page.ColumnDefinitions.Add(new ColumnDefinition());
        var albums = new ScrollViewer { Content = GalleryAlbums(), VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        _galSide = albums;
        var sideColumn = new Grid();
        sideColumn.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        sideColumn.RowDefinitions.Add(new RowDefinition());
        sideColumn.Children.Add(GallerySearch());
        Grid.SetRow(albums, 1);
        sideColumn.Children.Add(albums);
        page.Children.Add(sideColumn);
        var side = 220.0;
        var main = new StackPanel { Spacing = 18 };
        var picker = new DropDownButton { Content = AlbumName(_galAlbum), Visibility = Visibility.Collapsed,
            Flyout = new Flyout { Content = new StackPanel { Width = 260, Children = { GallerySearch(), new ScrollViewer { Content = GalleryAlbums(), MaxHeight = 520 } } } } };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(picker, T("Albums"));
        main.Children.Add(picker);
        page.SizeChanged += (_, e) =>
        {
            var narrow = e.NewSize.Width < (_galBook ? 900 : 980);
            page.ColumnDefinitions[0].Width = new GridLength(narrow ? 0 : side);
            page.ColumnSpacing = narrow || _galBook ? 0 : 28;
            sideColumn.Visibility = narrow ? Visibility.Collapsed : Visibility.Visible;
            picker.Visibility = narrow ? Visibility.Visible : Visibility.Collapsed;
        };
        main.Children.Add(GalleryHead());
        FullBleed(_galBook);
        _custDrawer = null;
        _boardPaper = null;
        if (!_galBook)
        {
            _galRowsHost = new Border { Child = GalleryRows() };
            main.Children.Add(_galRowsHost);
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
        // (the search and the list under it on the same margins)
        sideColumn.Padding = new Thickness(22, 26, 14, 0);
        albums.Padding = new Thickness(0, 0, 0, 18);
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
            var want = BoardFits();
            if (Math.Abs(want - BoardHeight) <= 8) { LiveScale(BoardHeight); return; }
            LiveScale(want);
            RefreshBoard(settle: 260);
        };
        var book = new Grid { Background = BoardBrush(look) };
        HistoryKeys();
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

    List<Picture> InAlbum() => Found.Where(p => Gallery.InAlbum(p, _galAlbum, _gal!, _galSaves)).ToList();

    // the pictures the search finds (all of them with no search)
    string _galQuery = "";
    Border? _galRowsHost;
    IEnumerable<Picture> Found => _galQuery.Length == 0 ? _galPics : _galPics.Where(Matches);
    bool Matches(Picture p)
    {
        bool Has(string? text) => text is { Length: > 0 } && text.Contains(_galQuery, StringComparison.CurrentCultureIgnoreCase);
        var culture = LangCulture();
        return Has(Gallery.Peek(_gal!, p.Path).Caption) || Has(Gallery.HouseholdOf(p, _gal!, _galSaves)) || Has(Path.GetFileNameWithoutExtension(p.Path))
            || Has(p.Taken.ToString("D", culture)) || Has(p.Taken.ToString("MMMM yyyy", culture))
            || _gal!.Custom.Any(c => Has(c.Name) && c.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase));
    }

    // the search box over the albums: kept while the list and the pictures under it are laid again as it is typed in
    FrameworkElement GallerySearch()
    {
        var box = new TextBox { PlaceholderText = T("Search pictures"), Text = _galQuery, Margin = new Thickness(0, 0, 0, 12) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, T("Search pictures"));
        var typed = 0;
        box.TextChanged += (_, _) =>
        {
            var mine = ++typed;
            After(220, () =>
            {
                if (mine != typed || box.Text.Trim() == _galQuery) return;
                _galQuery = box.Text.Trim();
                if (_galSide is not null) _galSide.Content = GalleryAlbums();
                if (!_galBook && _galRowsHost is not null) { _galLimit = 60; _galRowsHost.Child = GalleryRows(); }
                else RefreshBoard();
            });
        };
        box.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Escape && box.Text.Length > 0) { box.Text = ""; e.Handled = true; } };
        return box;
    }
    string AlbumName(string a) => a == Gallery.All ? T("All pictures") : a == Gallery.Favourites ? T("Favourites")
        : Gallery.CustomOf(_gal!, a) is { } c ? c.Name : a;

    // ---------- the albums ----------

    static FrameworkElement SideHeading(string text, bool first = false)
    {
        var t = Eyebrow(text);
        t.VerticalAlignment = VerticalAlignment.Center;
        return new Grid { Children = { t }, Height = 30, Margin = new Thickness(10, first ? 0 : 12, 0, 2) };
    }

    UIElement GalleryAlbums()
    {
        var s = new StackPanel { Spacing = 2 };
        var found = Found.ToList();
        Button Album(string key, string name, int count, Windows.UI.Color dot, string? glyph = null)
        {
            var on = key == _galAlbum;
            var g = new Grid { ColumnSpacing = 10 };
            // (searching: what holds nothing found stays out of the list)
            var none = _galQuery.Length > 0 && count == 0 && !on;
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            // (a dot or an icon in a 16 px column, as the Mods pane's icons)
            g.Children.Add(glyph is null ? new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Margin = new Thickness(4, 0, 4, 0), Fill = new SolidColorBrush(dot), VerticalAlignment = VerticalAlignment.Center }
                : new FontIcon { Glyph = glyph, FontSize = 15, Width = 16, Foreground = new SolidColorBrush(dot), VerticalAlignment = VerticalAlignment.Center });
            var t = new TextBlock { Text = name, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(t, 1);
            g.Children.Add(t);
            if (count > 0) { var n = new TextBlock { Text = count.ToString(), FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center }; Grid.SetColumn(n, 2); g.Children.Add(n); }
            var b = new Button { Content = g, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, Height = 32, Padding = new Thickness(10, 0, 10, 0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(4),
                Background = on ? new SolidColorBrush(Pick(0xFF232323, 0xFFE8E8E8)) : new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Pick(0xFF1E1E1E, 0xFFF1F1F1));
            b.Click += (_, _) => { _galAlbum = key; _galLimit = 60; BuildGallery(); };
            if (none) b.Visibility = Visibility.Collapsed;
            return b;
        }
        Windows.UI.Color[] dots = [Rgb(0x7D, 0xBE, 0x9A), Rgb(0x57, 0xB6, 0xE8), Rgb(0xE0, 0xA8, 0x4F), Rgb(0xE8, 0x6F, 0xB8), Rgb(0xB4, 0xAE, 0xF2), Rgb(0x8E, 0x8E, 0x8E)];
        s.Children.Add(SideHeading(T("LIBRARY"), first: true));
        s.Children.Add(Album(Gallery.All, T("All pictures"), _galPics.Count, AccentInk));
        s.Children.Add(Album(Gallery.Favourites, T("Favourites"), found.Count(p => Gallery.InAlbum(p, Gallery.Favourites, _gal!, _galSaves)), Rgb(0xF2, 0xC9, 0x4C)));
        s.Children.Add(SideHeading(T("HOUSEHOLDS")));
        // the player's own albums (a trip, a family's story) under the household they belong to, each with its icon;
        // right-click one to rename or delete it, a household to make an album in it
        var homes = _gal!.Custom.ToDictionary(c => c, c => Gallery.HomeOf(c, _gal, _galPics, _galSaves));
        var i = 0;
        foreach (var save in _galSaves)
        {
            var hb = Album(save.Name, save.Name, found.Count(p => Gallery.InAlbum(p, save.Name, _gal!, _galSaves)), dots[i++ % dots.Length]);
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
            var b = Album(c.Key, c.Name, found.Count(p => c.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase)), AccentInk, c.Icon);
            if (inside) b.Margin = new Thickness(14, 0, 0, 0); // (one level in, as a folder inside a folder in Mods)
            var menu = new MenuFlyout();
            var rename = new MenuFlyoutItem { Text = T("Rename or change the icon"), Icon = new FontIcon { Glyph = "\uE70F" } };
            var one = c;
            rename.Click += async (_, _) => { if (await AlbumDialog(one) is not null) BuildGallery(); };
            var drop = new MenuFlyoutItem { Text = T("Delete the album"), Icon = new FontIcon { Glyph = "\uE74D" } };
            drop.Click += async (_, _) =>
            {
                if (!await Confirm(T("Delete {0}?", one.Name), T("Only the album is deleted. Its pictures stay in your Screenshots folder and other albums."), T("Delete"))) return;
                _gal.Custom.Remove(one);
                _gal.Albums.Remove(one.Key);
                Keep();
                if (_galAlbum == one.Key) _galAlbum = Gallery.All;
                BuildGallery();
            };
            menu.Items.Add(rename);
            menu.Items.Add(drop);
            b.ContextFlyout = menu;
            return b;
        }
        var mine = new Grid { Margin = new Thickness(10, 12, 2, 2), Height = 30 };
        mine.ColumnDefinitions.Add(new ColumnDefinition());
        mine.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var mineHead = Eyebrow(T("ALBUMS"));
        mineHead.VerticalAlignment = VerticalAlignment.Center;
        mine.Children.Add(mineHead);
        var make = new Button { Content = new FontIcon { Glyph = "\uE710", FontSize = 12 }, Width = 26, Height = 26, Padding = new Thickness(0), BorderThickness = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(make, T("New album…"));
        ToolTipService.SetToolTip(make, T("New album…"));
        make.Click += async (_, _) => { if (await AlbumDialog(null) is { } made) { _galAlbum = made.Key; BuildGallery(); } };
        Grid.SetColumn(make, 1);
        mine.Children.Add(make);
        s.Children.Add(mine);
        foreach (var c in loose) s.Children.Add(Mine(c, false));
        if (_gal.Custom.Count == 0)
        {
            var hint = new HyperlinkButton { Content = T("+ an album of your own, like a trip"), FontSize = 13, Padding = new Thickness(10, 4, 0, 4) };
            hint.Click += async (_, _) => { if (await AlbumDialog(null) is { } made) { _galAlbum = made.Key; BuildGallery(); } };
            s.Children.Add(hint);
        }
        var note = Muted(T("Pictures go to the household you saved right after taking them. To change it, open the picture. Albums are inside their household."));
        note.TextWrapping = TextWrapping.Wrap;
        note.FontSize = 12;
        note.Margin = new Thickness(10, 16, 10, 0);
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
        if (Gallery.CustomOf(_gal!, _galAlbum) is { } own) name.Children.Add(new FontIcon { Glyph = own.Icon, FontSize = _galBook ? 28 : 22, Foreground = new SolidColorBrush(AccentInk), VerticalAlignment = VerticalAlignment.Center });
        _boardTitle = _galBook
            ? new TextBlock { Text = AlbumName(_galAlbum), FontFamily = new FontFamily(look.Font), FontSize = 40, FontWeight = FontWeights.SemiBold, Foreground = Ink(look) }
            : new TextBlock { Text = AlbumName(_galAlbum), FontSize = 28, FontWeight = FontWeights.SemiBold };
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
            var custom = Round(new Button(), "\uE790", T("Customise"));
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
            var shuffle = Round(new Button(), "\uE8B1", T("Rearrange"));
            shuffle.Click += (_, _) => ShuffleBoard();
            tools.Children.Add(shuffle);
        }
        else
        {
            var by = new ComboBox { MinWidth = 150 };
            by.Items.Add(T("By day"));
            by.Items.Add(T("By household"));
            by.SelectedIndex = _galByHome ? 1 : 0;
            by.SelectionChanged += (_, _) => { _galByHome = by.SelectedIndex == 1; DispatcherQueue.TryEnqueue(BuildGallery); }; // (later: the build removes this box)
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(by, T("Group"));
            tools.Children.Add(by);
            tools.Children.Add(Button(T("Screenshots folder"), () => OpenFolder(Gallery.Folder(Docs))));
            if (_galPics.Any(pc => pc.Video)) tools.Children.Add(Button(T("Videos folder"), () => OpenFolder(Gallery.VideoFolder(Docs))));
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
        var pill = new Border { CornerRadius = new CornerRadius(7), Background = new SolidColorBrush(Pick(0xFF2C2C2C, 0xFFE5E5E5)), Width = 118, HorizontalAlignment = HorizontalAlignment.Left,
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
                Foreground = _galBook == book ? Res("TextFillColorPrimaryBrush") : Res("ApexSoftTextBrush") };
            b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, text);
            b.Click += (_, _) => { if (_galBook == book) return; _galBook = book; if (_gal is not null) { _gal.Book = book; KeepQuiet(); } pill.Translation = new Vector3(book ? 118 : 0, 0, 0); DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, BuildGallery); };
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
        if (pics.Count == 0 && _galQuery.Length > 0) { s.Children.Add(Muted(T("Nothing found for “{0}”.", _galQuery))); return s; }
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
            var wrap = new CardGrid { MinItemWidth = 250, Spacing = 10, Aspect = 9.0 / 16, ChildrenTransitions = new TransitionCollection { new EntranceThemeTransition { IsStaggeringEnabled = true, FromVerticalOffset = 30 } } };
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

    // A picture read only as large as it is shown: the game's pictures are 4K, and a board of sixty read whole is gigabytes.
    // (the width must be set before the file, or the whole picture is read)
    // The picture comes from a small JPEG made once and kept under the launcher's data (a 4K PNG read by an Image keeps its
    // whole file in memory, half a gigabyte for a board); until it is made the image is empty and fills in when ready.
    // (one size serves the grid, the board and Home, so each picture is read once; the viewer has its own, larger one)
    static readonly SemaphoreSlim ThumbMakers = new(Math.Clamp(Environment.ProcessorCount - 2, 2, 8));
    static string? ThumbFile(string path, int px)
    {
        try
        {
            var info = new FileInfo(path);
            return Path.Combine(Paths.Data, "thumbs", $"{Stable(path.ToLowerInvariant()) & 0x7fffffff:x8}-{info.Length:x}-{info.LastWriteTimeUtc.Ticks:x}-{px}.jpg");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    // (the last ones asked for, by picture and width in steps of 160, so the same few are asked for again)
    static readonly Dictionary<string, LinkedListNode<(string Key, BitmapImage Image)>> ThumbsKept = [];
    static readonly LinkedList<(string Key, BitmapImage Image)> ThumbsOrder = new();
    const int ThumbsKeptMax = 90;

    static BitmapImage Thumb(string path, double width)
    {
        var dw = (int)Math.Ceiling(Math.Max(64, width) / 160) * 160;
        var key = path + "|" + dw;
        // (large copies, the viewer's, are not kept: a few of them would hold more memory than all the small ones)
        if (dw > 640) return MakeThumbImage(path, width, dw);
        if (ThumbsKept.TryGetValue(key, out var node))
        {
            ThumbsOrder.Remove(node);
            ThumbsOrder.AddFirst(node);
            return node.Value.Image;
        }
        var b = MakeThumbImage(path, width, dw);
        ThumbsKept[key] = ThumbsOrder.AddFirst((key, b));
        while (ThumbsOrder.Count > ThumbsKeptMax) { ThumbsKept.Remove(ThumbsOrder.Last!.Value.Key); ThumbsOrder.RemoveLast(); }
        return b;
    }

    static BitmapImage MakeThumbImage(string path, double width, int dw)
    {
        var px = width * 1.5 <= 1100 ? 1100 : 2200;
        var b = new BitmapImage { DecodePixelType = DecodePixelType.Logical, DecodePixelWidth = dw };
        var video = Gallery.VideoKinds.Contains(Path.GetExtension(path).ToLowerInvariant());
        if (ThumbFile(path, px) is not { } file) { if (!video) b.UriSource = new Uri(path); return b; }
        if (File.Exists(file)) { b.UriSource = new Uri(file); return b; }
        var queue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        _ = Task.Run(async () =>
        {
            var made = await MakeThumb(path, file, px) ? file : video ? null : path;
            if (made is not null) queue.TryEnqueue(() => b.UriSource = new Uri(made));
        });
        return b;
    }

    // the pictures' small copies made in the background once the launcher is up, newest first, so the Gallery opens ready
    static bool _thumbsWarmed;
    void WarmThumbs()
    {
        if (_thumbsWarmed || _game is null) return;
        _thumbsWarmed = true;
        var docs = Docs;
        _ = Task.Run(async () =>
        {
            await Task.Delay(2500);
            var todo = Gallery.List(docs).Select(p => (p.Path, File: ThumbFile(p.Path, 1100))).Where(t => t.File is not null && !File.Exists(t.File)).ToList();
            // (two at a time; Parallel is trimmed out of the published launcher)
            await Task.WhenAll(Enumerable.Range(0, 2).Select(w => Task.Run(async () => { for (int i = w; i < todo.Count; i += 2) await MakeThumb(todo[i].Path, todo[i].File!, 1100); })));
        });
    }

    // a frame of a video as Windows' own thumbnail of it (none when Windows has no decoder for it: only its icon)
    static async Task<Windows.Storage.FileProperties.StorageItemThumbnail?> VideoFrame(string path, int px)
    {
        var file = await Windows.Storage.StorageFile.GetFileFromPathAsync(Path.GetFullPath(path));
        var thumb = await file.GetThumbnailAsync(Windows.Storage.FileProperties.ThumbnailMode.SingleItem, (uint)px, Windows.Storage.FileProperties.ThumbnailOptions.ResizeThumbnail);
        if (thumb is { Type: Windows.Storage.FileProperties.ThumbnailType.Image }) return thumb;
        thumb?.Dispose();
        return null;
    }

    // the round play mark over a video's picture
    static Border PlayMark(double size) => new()
    {
        Width = size, Height = size, CornerRadius = new CornerRadius(size / 2), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB3, 0x0C, 0x0C, 0x0C)), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x55, 255, 255, 255)), BorderThickness = new Thickness(1.5),
        IsHitTestVisible = false,
        Child = new FontIcon { Glyph = "\uF5B0", FontSize = size * 0.38, Foreground = new SolidColorBrush(Microsoft.UI.Colors.White), Margin = new Thickness(size * 0.06, 0, 0, 0) },
    };

    static async Task<bool> Blank(string file)
    {
        const int W = 320, H = 180;
        var px = new byte[W * H * 4];
        for (int i = 0; i < px.Length; i += 4) { px[i] = 0x2E; px[i + 1] = 0x2A; px[i + 2] = 0x28; px[i + 3] = 0xFF; }
        var temp = file + ".part";
        using (var target = File.Create(temp))
        {
            var encoder = await Windows.Graphics.Imaging.BitmapEncoder.CreateAsync(Windows.Graphics.Imaging.BitmapEncoder.JpegEncoderId, target.AsRandomAccessStream());
            encoder.SetPixelData(Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8, Windows.Graphics.Imaging.BitmapAlphaMode.Ignore, W, H, 96, 96, px);
            await encoder.FlushAsync();
        }
        File.Move(temp, file, true);
        return true;
    }

    static async Task<bool> MakeThumb(string path, string file, int px)
    {
        await ThumbMakers.WaitAsync();
        try
        {
            if (File.Exists(file)) return true;
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            var video = Gallery.VideoKinds.Contains(Path.GetExtension(path).ToLowerInvariant());
            using var source = video ? null : File.OpenRead(path);
            using var frame = video ? await VideoFrame(path, px) : null;
            // (a video Windows cannot take a frame from still gets a picture: a dark 16:9 one the play mark sits on)
            if (video && frame is null) return await Blank(file);
            var decoder = await Windows.Graphics.Imaging.BitmapDecoder.CreateAsync(frame ?? source!.AsRandomAccessStream());
            var scale = Math.Min(1.0, px / (double)decoder.PixelWidth);
            var transform = new Windows.Graphics.Imaging.BitmapTransform
            {
                ScaledWidth = (uint)Math.Max(1, decoder.PixelWidth * scale), ScaledHeight = (uint)Math.Max(1, decoder.PixelHeight * scale),
                InterpolationMode = Windows.Graphics.Imaging.BitmapInterpolationMode.Fant,
            };
            using var pixels = await decoder.GetSoftwareBitmapAsync(Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8, Windows.Graphics.Imaging.BitmapAlphaMode.Ignore, transform,
                Windows.Graphics.Imaging.ExifOrientationMode.RespectExifOrientation, Windows.Graphics.Imaging.ColorManagementMode.DoNotColorManage);
            var temp = file + ".part";
            using (var target = File.Create(temp))
            {
                var output = target.AsRandomAccessStream();
                var quality = new Windows.Graphics.Imaging.BitmapPropertySet { ["ImageQuality"] = new Windows.Graphics.Imaging.BitmapTypedValue(0.86, Windows.Foundation.PropertyType.Single) };
                var encoder = await Windows.Graphics.Imaging.BitmapEncoder.CreateAsync(Windows.Graphics.Imaging.BitmapEncoder.JpegEncoderId, output, quality);
                encoder.SetSoftwareBitmap(pixels);
                await encoder.FlushAsync();
            }
            File.Move(temp, file, true);
            return true;
        }
        catch (Exception e) { App.Log(e); return false; }
        finally { ThumbMakers.Release(); }
    }

    // a picture in the grid: it fades in once read, rises and grows a touch under the pointer with its time showing, a star
    // when it is a favourite
    UIElement Tile(Picture p, Action<FrameworkElement> open)
    {
        var g = new Grid { CornerRadius = new CornerRadius(12), Background = Res("ApexPillBrush") };
        var bmp = Thumb(p.Path, 420);
        var image = new Image { Stretch = Stretch.UniformToFill, Source = bmp, Opacity = bmp.PixelWidth > 0 ? 1 : 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(320) } };
        if (bmp.PixelWidth == 0) bmp.ImageOpened += (_, _) => image.Opacity = 1;
        if (p.Video) g.Children.Add(PlayMark(46));
        g.Children.Add(image);
        var when = new Border { VerticalAlignment = VerticalAlignment.Bottom, Padding = new Thickness(12, 18, 12, 9), Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(180) },
            Background = new LinearGradientBrush { StartPoint = new(0, 0), EndPoint = new(0, 1), GradientStops = { new GradientStop { Color = Windows.UI.Color.FromArgb(0, 0, 0, 0), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(0xB3, 0, 0, 0), Offset = 1 } } },
            Child = new TextBlock { Text = p.Taken.ToString("t", LangCulture()) + "  ·  " + (Gallery.HouseholdOf(p, _gal!, _galSaves) ?? ""), FontSize = 12, Foreground = new SolidColorBrush(Microsoft.UI.Colors.White), TextTrimming = TextTrimming.CharacterEllipsis } };
        g.Children.Add(when);
        if (Gallery.Peek(_gal!, p.Path).Favourite)
            g.Children.Add(new Border { Width = 26, Height = 26, CornerRadius = new CornerRadius(13), Margin = new Thickness(8), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB3, 0x0C, 0x0C, 0x0C)), Child = new FontIcon { Glyph = "", FontSize = 13, Foreground = new SolidColorBrush(Rgb(0xF2, 0xC9, 0x4C)) } });
        var b = new Button { Content = g, Padding = new Thickness(0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(12), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), Shadow = new ThemeShadow(), Translation = new Vector3(0, 0, 8) };
        b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, Path.GetFileNameWithoutExtension(p.Path));
        // (16:9, as wide as its column)
        b.VerticalAlignment = VerticalAlignment.Stretch;
        b.VerticalContentAlignment = VerticalAlignment.Stretch;
        b.SizeChanged += (_, e) => b.CenterPoint = new Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0);
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
    // How the pictures come onto the board, chosen per album: soft (the default) rises a little as it fades in, float
    // drifts down turning gently, zoom grows from the middle, slide comes in along the timeline, playful drops with a
    // small bounce, none is simply there.
    static readonly (string Key, string Name, string Glyph)[] Entrances =
        [("soft", "Soft", "\uE7B3"), ("float", "Float", "\uE753"), ("zoom", "Zoom", "\uE8A3"), ("slide", "Slide", "\uE76C"), ("playful", "Playful", "\uE76E"), ("none", "None", "\uE711")];

    // a little picture that comes in the way it names: once when shown, again under the pointer
    static FrameworkElement EntranceArt(string mode, bool chosen)
    {
        var card = new Border
        {
            Width = 96, Height = 66, Padding = new Thickness(6), CornerRadius = new CornerRadius(9),
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xE6, 0xEC, 0xE7, 0xDA)),
            Child = new Border { CornerRadius = new CornerRadius(5), Background = new LinearGradientBrush(new GradientStopCollection { new GradientStop { Color = Windows.UI.Color.FromArgb(0xFF, 0x8E, 0x86, 0xD8), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(0xFF, 0x4A, 0x55, 0x7A), Offset = 1 } }, 60) },
        };
        var room = new Grid { Width = 160, Height = 100, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        room.Children.Add(card);
        var art = new Viewbox { Width = 36, Height = 22, Child = room };
        void Go() { var calm = _boardCalm; _boardCalm = false; Land(card, -4, 0, -40, mode); _boardCalm = calm; }
        // (the chosen one plays when shown; any of them plays while the pointer is on its tile)
        art.Loaded += (_, _) =>
        {
            if (chosen) Go();
            for (var up = VisualTreeHelper.GetParent(art); up is not null; up = VisualTreeHelper.GetParent(up))
                if (up is Microsoft.UI.Xaml.Controls.Primitives.ButtonBase tile) { tile.PointerEntered += (_, _) => Go(); break; }
        };
        return art;
    }

    static CompositeTransform Land(FrameworkElement el, double angle, int delay, double fromX, string mode = "soft")
    {
        var t = new CompositeTransform { Rotation = angle };
        el.RenderTransform = t;
        el.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);
        if (_boardCalm || mode == "none") return t;
        el.Opacity = 0;
        var sb = new Storyboard { BeginTime = TimeSpan.FromMilliseconds(mode == "playful" ? delay : delay * 0.6) };
        var smooth = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 4 };
        var gentle = new CubicEase { EasingMode = EasingMode.EaseOut };
        switch (mode)
        {
            case "float":
                t.TranslateY = -70; t.Rotation = angle + 7;
                Tween(sb, t, "TranslateY", 0, 900, smooth);
                Tween(sb, t, "Rotation", angle, 1000, gentle);
                Tween(sb, el, "Opacity", 1, 500);
                break;
            case "zoom":
                t.ScaleX = t.ScaleY = 0.86;
                Tween(sb, t, "ScaleX", 1, 700, smooth);
                Tween(sb, t, "ScaleY", 1, 700, smooth);
                Tween(sb, el, "Opacity", 1, 420);
                break;
            case "slide":
                t.TranslateX = 110;
                Tween(sb, t, "TranslateX", 0, 760, smooth);
                Tween(sb, el, "Opacity", 1, 460);
                break;
            case "playful":
                t.TranslateY = -160; t.TranslateX = fromX * 0.6; t.Rotation = angle + 14; t.ScaleX = t.ScaleY = 1.08;
                Tween(sb, t, "TranslateY", 0, 820, new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 });
                Tween(sb, t, "TranslateX", 0, 700, gentle);
                Tween(sb, t, "Rotation", angle, 1000, new ElasticEase { EasingMode = EasingMode.EaseOut, Oscillations = 1, Springiness = 6 });
                Tween(sb, t, "ScaleX", 1, 600, gentle);
                Tween(sb, t, "ScaleY", 1, 600, gentle);
                Tween(sb, el, "Opacity", 1, 260);
                break;
            default:
                t.TranslateY = 22;
                Tween(sb, t, "TranslateY", 0, 760, smooth);
                Tween(sb, el, "Opacity", 1, 520, gentle);
                break;
        }
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
    readonly HashSet<string> _spread = [];
    readonly Dictionary<string, CustomAlbum> _boardPiled = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, Placed> _boardPlaces = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, double> _boardAnchor = new(StringComparer.OrdinalIgnoreCase);   // each picture's and pile's spot on the timeline
    Dictionary<string, (double X, double Y)>? _boardFlip;
    double? _boardKeepAt;
    FrameworkElement? _dropTarget;
    ScrollViewer? _galSide;
    static readonly double[] PileTurns = [-3, 7, -8, 4, -5, 9];
    static readonly (double X, double Y)[] PileShift = [(0, 0), (-14, -12), (16, -16), (-8, -24), (12, -6), (-16, -4)];

    // a number from a text that stays the same from one run to the next (string hashes don't)
    static int Stable(string text) { unchecked { var h = 17; foreach (var ch in text) h = h * 31 + ch; return h; } }

    // An album opened from its pile: the board dims and the album's pictures fly out of the pile to the middle, large;
    // a picture opens in the viewer, a click on the dim, the close button or Esc sends them back to the pile.
    bool _spotlightOpen, _boardStale; // an album open over the board; the board to lay again once it closes

    void Spotlight(CustomAlbum album, List<Picture> members, List<FrameworkElement> from)
    {
        _spotlightOpen = true;
        if (_boardFrame is not Grid stage || _gal is null || members.Count == 0) return;
        var look = Gallery.Look(_gal, _galAlbum);
        var w = stage.ActualWidth;
        var h = stage.ActualHeight;
        // (the open album is always on a dark veil, whatever the theme)
        var veil = new Grid { Background = new AcrylicBrush { TintColor = Rgb(0x0A, 0x0A, 0x0D), TintOpacity = 0.55, TintLuminosityOpacity = 0.62, FallbackColor = Windows.UI.Color.FromArgb(0xE6, 0x0A, 0x0A, 0x0D) }, Opacity = 0, RequestedTheme = ElementTheme.Dark };
        var room = new Canvas();
        var roomScroll = new ScrollViewer { Content = room, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollMode = ScrollMode.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto };
        veil.Children.Add(roomScroll);
        // the head: the album's name, how many, open it, close
        var head = new Grid { VerticalAlignment = VerticalAlignment.Top, Padding = new Thickness(40, 28, 28, 0) };
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var title = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        title.Children.Add(new FontIcon { Glyph = album.Icon, FontSize = 26, Foreground = new SolidColorBrush(Rgb(0xE8, 0xE2, 0xD4)), VerticalAlignment = VerticalAlignment.Center });
        title.Children.Add(new TextBlock { Text = album.Name, FontFamily = new FontFamily(look.Font), FontSize = 44, Foreground = new SolidColorBrush(Rgb(0xF4, 0xF0, 0xE6)) });
        title.Children.Add(new TextBlock { Text = members.Count == 1 ? T("1 picture") : T("{0} pictures", members.Count), FontSize = 14, Opacity = 0.65, Foreground = new SolidColorBrush(Rgb(0xE8, 0xE2, 0xD4)), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(4, 10, 0, 0) });
        head.Children.Add(title);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        var whole = new Button { Content = T("Open the album"), CornerRadius = new CornerRadius(18), Padding = new Thickness(16, 6, 16, 7) };
        var shut = new Button { Content = new FontIcon { Glyph = "\uE711", FontSize = 12 }, Width = 38, Height = 38, Padding = new Thickness(0), CornerRadius = new CornerRadius(19) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(shut, T("Close"));
        actions.Children.Add(whole);
        actions.Children.Add(shut);
        Grid.SetColumn(actions, 1);
        head.Children.Add(actions);
        veil.Children.Add(head);

        // the pictures in the board's own frames, large: up to three to a row (four as two by two); the room scrolls if they need it
        var cols = members.Count <= 3 ? members.Count : members.Count == 4 ? 2 : 3;
        var rows = (int)Math.Ceiling(members.Count / (double)cols);
        var gap = 48.0;
        var cw = Math.Clamp((w - 160 - gap * (cols - 1)) / cols, 220, 600);
        var keepScale = _boardScale;
        var keepCalm = _boardCalm;
        var light = new SolidColorBrush(Rgb(0xEC, 0xE7, 0xDA));
        var shown = new List<(FrameworkElement Card, CompositeTransform T, double X, double Y)>();
        var cards = new List<FrameworkElement>();
        double ch = 0;
        try
        {
            _boardScale = cw / 280 / look.Size;
            _boardCalm = true;
            var rnd = new Random(Stable(album.Id));
            for (int i = 0; i < members.Count; i++)
            {
                var card = Framed(members[i], new Placed { Angle = rnd.NextDouble() * 5 - 2.5 }, look, light, 1, 0, members, null, still: true);
                card.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
                ch = Math.Max(ch, card.DesiredSize.Height > 0 ? card.DesiredSize.Height : cw * 0.8);
                cards.Add(card);
            }
        }
        finally { _boardScale = keepScale; _boardCalm = keepCalm; }
        Windows.Foundation.Size Fit(FrameworkElement c) => c.DesiredSize.Width > 1 ? c.DesiredSize : new Windows.Foundation.Size(cw, ch);
        var top0 = 128.0;
        var left0 = Math.Max(40, (w - (cols * cw + (cols - 1) * gap)) / 2);
        var tall = top0 + rows * ch + (rows - 1) * gap + 60;
        if (tall < h) top0 += (h - tall) / 2;
        room.Height = Math.Max(h, top0 + rows * ch + (rows - 1) * gap + 60);
        room.Width = Math.Max(w, left0 * 2 + cols * cw + (cols - 1) * gap); // (the pictures keep their size: a narrow window scrolls sideways instead of cutting them)
        for (int i = 0; i < cards.Count; i++)
        {
            var card = cards[i];
            var x = left0 + (i % cols) * (cw + gap) + (cw - Fit(card).Width) / 2;
            var y = top0 + (i / cols) * (ch + gap);
            Canvas.SetLeft(card, x);
            Canvas.SetTop(card, y);
            room.Children.Add(card);
            if (card is Border { Child: FrameworkElement { RenderTransform: CompositeTransform t } }) shown.Add((card, t, x, y));
        }
        stage.Children.Add(veil);
        Grid.SetColumnSpan(veil, 1);

        // out of the pile: each picture starts where its card lies on the board, small, and flies to its place
        var rise = new Storyboard();
        Tween(rise, veil, "Opacity", 1, 220);
        for (int i = 0; i < shown.Count; i++)
        {
            var (card, t, x, y) = shown[i];
            var settle = t.Rotation;
            var size = Fit(card);
            if (i < from.Count && from[i].ActualWidth > 0)
            {
                var b0 = from[i].TransformToVisual(stage).TransformBounds(new Windows.Foundation.Rect(0, 0, from[i].ActualWidth, from[i].ActualHeight));
                var k = b0.Width / Math.Max(1, size.Width);
                t.ScaleX = t.ScaleY = k;
                t.TranslateX = b0.X + b0.Width / 2 - (x + size.Width / 2);
                t.TranslateY = b0.Y + b0.Height / 2 - (y + size.Height / 2);
                if (from[i] is Border { Child: FrameworkElement { RenderTransform: CompositeTransform was } }) t.Rotation = was.Rotation;
            }
            var pop = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.4 };
            var glide = new CubicEase { EasingMode = EasingMode.EaseOut };
            Tween(rise, t, "ScaleX", 1, 640, pop, i * 70);
            Tween(rise, t, "ScaleY", 1, 640, pop, i * 70);
            Tween(rise, t, "TranslateX", 0, 600, glide, i * 70);
            Tween(rise, t, "TranslateY", 0, 600, glide, i * 70);
            Tween(rise, t, "Rotation", settle, 800, new ElasticEase { EasingMode = EasingMode.EaseOut, Oscillations = 1, Springiness = 5 }, i * 70);
        }
        foreach (var f in from) f.Opacity = 0.25;
        Play(rise);

        var closing = false;
        void Close(bool open)
        {
            if (closing) return;
            closing = true;
            var fall = new Storyboard();
            Tween(fall, veil, "Opacity", 0, 300, null, 120);
            for (int i = 0; i < shown.Count; i++)
            {
                var (card, t, x, y) = shown[i];
                if (open || i >= from.Count) { Tween(fall, card, "Opacity", 0, 200); continue; }
                var b = from[i].TransformToVisual(stage).TransformBounds(new Windows.Foundation.Rect(0, 0, from[i].ActualWidth, from[i].ActualHeight));
                var size = Fit(card);
                var sy = roomScroll.VerticalOffset;
                var sx = roomScroll.HorizontalOffset;
                var ease = new CubicEase { EasingMode = EasingMode.EaseIn };
                Tween(fall, t, "ScaleX", b.Width / Math.Max(1, size.Width), 360, ease);
                Tween(fall, t, "ScaleY", b.Width / Math.Max(1, size.Width), 360, ease);
                Tween(fall, t, "TranslateX", b.X + b.Width / 2 - (x - sx + size.Width / 2), 360, ease);
                Tween(fall, t, "TranslateY", b.Y + b.Height / 2 - (y - sy + size.Height / 2), 360, ease);
            }
            fall.Completed += (_, _) =>
            {
                stage.Children.Remove(veil);
                foreach (var f in from) f.Opacity = 1;
                _spotlightOpen = false;
                if (open) { _boardStale = false; _galAlbum = album.Key; BuildGallery(); }
                else if (_boardStale) { _boardStale = false; RefreshBoard(page: true); }
            };
            Play(fall);
        }
        veil.Tapped += (_, _) => Close(false);
        shut.Click += (_, _) => Close(false);
        whole.Click += (_, _) => Close(true);
        veil.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Escape) { e.Handled = true; Close(false); } };
        DispatcherQueue.TryEnqueue(() => shut.Focus(FocusState.Programmatic));
    }

    // a right click on a pile or its tag: open it, rename it, or undo it (its pictures go back on the board; Undo brings it back)
    MenuFlyout PileMenu(CustomAlbum c)
    {
        var menu = new MenuFlyout();
        var open = new MenuFlyoutItem { Text = T("Open the album"), Icon = new FontIcon { Glyph = "" } };
        open.Click += (_, _) => { _galAlbum = c.Key; BuildGallery(); };
        var rename = new MenuFlyoutItem { Text = T("Rename or change the icon"), Icon = new FontIcon { Glyph = "" } };
        rename.Click += async (_, _) => { if (await AlbumDialog(c) is not null) BuildGallery(); };
        var undo = new MenuFlyoutItem { Text = T("Undo the album"), Icon = new FontIcon { Glyph = "" } };
        undo.Click += (_, _) =>
        {
            if (_gal is null) return;
            _gal.Custom.Remove(c);
            _gal.Albums.Remove(c.Key);
            Gallery.Look(_gal, _galAlbum).Places.Remove(c.Key);
            _spread.Remove(c.Key);
            Keep(step: true);
            if (_galSide is not null) _galSide.Content = GalleryAlbums();
            FlipBoard();
            ShowUndo(T("Album undone: its pictures are back on the board"), () => StepBack(true));
        };
        menu.Items.Add(open);
        menu.Items.Add(rename);
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(undo);
        return menu;
    }

    // the board laid again where it is, each picture sliding from where it lay to its new place
    void FlipBoard()
    {
        _boardFlip = new(StringComparer.OrdinalIgnoreCase);
        foreach (var (pic, card, _) in _boardCards) _boardFlip.TryAdd(pic.Path, (Canvas.GetLeft(card), Canvas.GetTop(card)));
        _boardKeepAt = _boardScroll?.HorizontalOffset;
        RefreshBoard();
    }

    // while a picture is dragged: the picture whose middle it is over grows a little, ready to make an album with it
    double Moved(FrameworkElement el) => Math.Abs(Canvas.GetLeft(el) - _dragFrom.X) + Math.Abs(Canvas.GetTop(el) - _dragFrom.Y);

    void MarkUnder(FrameworkElement? me)
    {
        FrameworkElement? hit = null;
        // (only a picture carried somewhere is put into an album, not one nudged where it lies)
        if (me is not null && Moved(me) > 40)
        {
            var cx = Canvas.GetLeft(me) + me.ActualWidth / 2;
            var cy = Canvas.GetTop(me) + me.ActualHeight / 2;
            var mine = _boardCards.FirstOrDefault(c => c.Card == me).Pic;
            var myPile = mine is null ? null : _boardPiled.GetValueOrDefault(mine.Path);
            var best = double.MaxValue;
            foreach (var (pic, card, _) in _boardCards)
            {
                if (card == me || card.ActualWidth <= 0 || card.Opacity < 0.5) continue;
                if (myPile is not null && _boardPiled.GetValueOrDefault(pic.Path) == myPile) continue;
                var dx = Math.Abs(cx - (Canvas.GetLeft(card) + card.ActualWidth / 2)) / card.ActualWidth;
                var dy = Math.Abs(cy - (Canvas.GetTop(card) + card.ActualHeight / 2)) / card.ActualHeight;
                if (dx < 0.4 && dy < 0.4 && dx + dy < best) { best = dx + dy; hit = card; }
            }
        }
        if (hit == _dropTarget) return;
        if (_dropTarget is not null) _dropTarget.Scale = Vector3.One;
        _dropTarget = hit;
        if (hit is not null) hit.Scale = new Vector3(1.08f, 1.08f, 1);
    }

    // a picture let go: on another's middle it joins that picture's album (or makes one with it); else it stays where it was put
    async void CardDropped(Picture p, Placed place, FrameworkElement me, CustomAlbum? pile)
    {
        var look = Gallery.Look(_gal!, _galAlbum);
        var target = _dropTarget;
        MarkUnder(null);
        if (target is not null && _boardCards.FirstOrDefault(c => c.Card == target) is { Pic: not null } hit)
        {
            var into = _boardPiled.GetValueOrDefault(hit.Pic.Path);
            if (into is null && await AlbumDialog(null, Gallery.HouseholdOf(hit.Pic, _gal!, _galSaves)) is { } made)
            {
                into = made;
                look.Places[made.Key] = new Placed { X = Canvas.GetLeft(target) - _boardAnchor.GetValueOrDefault(hit.Pic.Path), Y = Canvas.GetTop(target), Moved = true, On = hit.Pic.Path };
            }
            if (into is not null)
            {
                foreach (var path in new[] { hit.Pic.Path, p.Path })
                {
                    if (!into.Pictures.Contains(path, StringComparer.OrdinalIgnoreCase)) into.Pictures.Add(path);
                    look.Places.Remove(path);
                }
                _spread.Remove(into.Key);
                Keep();
                if (_galSide is not null) _galSide.Content = GalleryAlbums();
                FlipBoard();
                return;
            }
        }
        // (a picture taken off a pile slides back onto it)
        if (pile is not null) { FlipBoard(); return; }
        place.X = Canvas.GetLeft(me);
        place.Y = Canvas.GetTop(me);
        place.Moved = true;
        look.TopZ++;
        look.Places[p.Path] = new Placed { X = place.X - _boardAnchor.GetValueOrDefault(p.Path), Y = place.Y, Angle = place.Angle, Moved = true, Z = look.TopZ };
        _restZ[me] = 799;
        Canvas.SetZIndex(me, 799);
        Keep();
    }

    // the pictures on the board turned to the tilt and scaled to the size being chosen, without laying the board again
    void LiveCards(AlbumLook look)
    {
        foreach (var (pic, card, _) in _boardCards)
            if (card is Border { Child: FrameworkElement { RenderTransform: CompositeTransform t } } && _boardPlaces.TryGetValue(pic.Path, out var at))
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
            Keep();
            if (_custChanged) { _custChanged = false; RefreshBoard(page: true); }
        }
    }

    Border CustomizeDrawer()
    {
        var d = new Border
        {
            Width = 372, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 14, 14, 14), CornerRadius = new CornerRadius(18),
            Background = new AcrylicBrush { TintColor = Pick(0xFF181818, 0xFFF7F7F7), TintOpacity = 0.82, TintLuminosityOpacity = 0.9, FallbackColor = Pick(0xFF1E1E1E, 0xFFF7F7F7) },
            BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x26, 255, 255, 255)), BorderThickness = new Thickness(1),
            Shadow = new ThemeShadow(), Translation = new Vector3(420, 0, 40), Opacity = 0, IsHitTestVisible = false,
            TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(320) }, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) },
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(d, T("Customise"));
        _custDrawer = d;
        return d;
    }

    // a small header over a group of controls
    static TextBlock Caption(string t) => new() { Text = t, FontSize = 11, FontWeight = FontWeights.SemiBold, CharacterSpacing = 100, Foreground = Res("ApexMutedBrush"), Margin = new Thickness(0, 6, 0, 0) };

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
            BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Pick(0x1AFFFFFF, 0x1F000000)),
            Background = on ? Res("ApexAccentSoftBrush") : new SolidColorBrush(Pick(0x0CFFFFFF, 0x0A000000)) };
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
        void Changed() { _custChanged = true; Keep(); RefreshBoard(); }
        var root = new Grid();
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition());

        // the head: what is being dressed, and the way out
        var head = new Grid { Padding = new Thickness(22, 18, 12, 8) };
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var titles = new StackPanel { Spacing = 0 };
        titles.Children.Add(new TextBlock { Text = T("Customise"), FontSize = 20, FontWeight = FontWeights.SemiBold });
        titles.Children.Add(new TextBlock { Text = AlbumName(_galAlbum), FontSize = 12, Opacity = 0.6, TextTrimming = TextTrimming.CharacterEllipsis });
        head.Children.Add(titles);
        var x = new Button { Content = new FontIcon { Glyph = "", FontSize = 12 }, Width = 34, Height = 34, Padding = new Thickness(0), CornerRadius = new CornerRadius(17), BorderThickness = new Thickness(0),
            Background = new SolidColorBrush(Pick(0x14FFFFFF, 0x10000000)), VerticalAlignment = VerticalAlignment.Top };
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
            var now = BoardColor(look);

            body.Children.Add(Caption(T("COLOUR")));
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
            var sheets = Papers;
            for (int i = 0; i < sheets.Length; i++)
            {
                var key = sheets[i].Key;
                var art = new Grid { Width = 56, Height = 44 };
                art.Children.Add(new Border { CornerRadius = new CornerRadius(5), Background = BoardBrush(look) });
                var c = new Canvas { Width = 56, Height = 44 };
                DrawPaper(c, new AlbumLook { Pattern = key, Color = look.Color, Background = look.Background }, 56, 44, 9);
                art.Children.Add(c);
                art.Clip = new RectangleGeometry { Rect = new(0, 0, 56, 44) };
                PutAt(papers, Choice(art, T(sheets[i].Name), look.Pattern == key, () => { look.Pattern = key; Changed(); FillCustomize(); }, 84), i, 4);
            }
            body.Children.Add(papers);
            if (look.Pattern is not ("none" or null))
            {
                // how strong the paper shows, drawn again as it is dragged
                var row = new Grid { ColumnSpacing = 10, Margin = new Thickness(0, 6, 0, 0) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.Children.Add(new TextBlock { Text = T("Faint"), FontSize = 12, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center });
                var strength = new Slider { Minimum = 0.3, Maximum = 2.5, StepFrequency = 0.05, Value = look.PaperStrength, VerticalAlignment = VerticalAlignment.Center };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(strength, T("Paper strength"));
                Grid.SetColumn(strength, 1);
                row.Children.Add(strength);
                var strongText = new TextBlock { Text = T("Strong"), FontSize = 12, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center };
                Grid.SetColumn(strongText, 2);
                row.Children.Add(strongText);
                strength.ValueChanged += (_, e) =>
                {
                    look.PaperStrength = e.NewValue;
                    if (_boardPaper is not null && _boardFrame is not null) DrawPaper(_boardPaper, look, _boardFrame.ActualWidth, _boardFrame.ActualHeight, 28);
                    _custChanged = true;
                    Keep();
                };
                body.Children.Add(row);
            }
        }
        else if (_custTab == 3)
        {
            // the stickers: a click sticks one where the board is being looked at
            var hint = new TextBlock { Text = T("Click one to stick it on the board. Drag to move it. Right-click it to make it bigger, smaller or take it off."), FontSize = 12, Opacity = 0.65, TextWrapping = TextWrapping.Wrap };
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
                        Background = new SolidColorBrush(Pick(0x0CFFFFFF, 0x0A000000)) };
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

            Grid Range(string name, string left, string right, double min, double max, double value, Action<double> set, Action? live = null)
            {
                var g = new Grid { ColumnSpacing = 10, Margin = new Thickness(0, 2, 0, 0) };
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                g.ColumnDefinitions.Add(new ColumnDefinition());
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                g.Children.Add(new TextBlock { Text = left, FontSize = 12, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center });
                var s = new Slider { Minimum = min, Maximum = max, StepFrequency = 0.05, Value = value };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(s, name);
                s.ValueChanged += (_, e) => { set(e.NewValue); _custChanged = true; if (live is not null) live(); else LiveCards(look); };
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
            body.Children.Add(Caption(T("SPACING")));
            body.Children.Add(Range(T("Spacing: close to apart"), T("Close"), T("Apart"), 0.6, 1.7, look.Spacing, v => look.Spacing = v, FlipBoard));
            body.Children.Add(Caption(T("ENTRANCE")));
            var ways = Columns(3);
            for (int i = 0; i < Entrances.Length; i++)
            {
                var (key, name, glyph) = Entrances[i];
                var art = EntranceArt(key, look.Entrance == key);
                PutAt(ways, Choice(art, T(name), look.Entrance == key, () =>
                {
                    look.Entrance = key;
                    _custChanged = true;
                    Keep();
                    FillCustomize();
                    // (played straight away, the board kept where it is)
                    if (_boardHost is not null) { _boardKeep = _boardScroll is { ScrollableWidth: > 0 } sv ? sv.HorizontalOffset / sv.ScrollableWidth : null; _boardHost.Child = GalleryBoard(); }
                }, 60), i, 3);
            }
            body.Children.Add(ways);
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
                        BorderThickness = new Thickness(on ? 2 : 1), BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Pick(0x1AFFFFFF, 0x1F000000)),
                        Background = on ? Res("ApexAccentSoftBrush") : new SolidColorBrush(Pick(0x0CFFFFFF, 0x0A000000)) };
                    Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, f);
                    var font = f;
                    b.Click += (_, _) => { look.Font = font; Changed(); FillCustomize(); };
                    list.Children.Add(b);
                }
                var more = q.Length == 0 ? T("{0} fonts on this PC: search to use any of them.", all.Count)
                    : names.Count == 0 ? T("No font with that name.") : names.Count == 40 ? T("Showing the first 40. Type more to narrow the list.") : "";
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
    static string V(int code) => char.ConvertFromUtf32(code) + "\uFE0F";   // (the emoji form of a sign that also has a plain text form)
    static readonly (string Group, string[] Set)[] StickerSets =
    [
        ("NATURE", [E(0x1F338), E(0x1F33B), E(0x1F337), E(0x1F33F), E(0x1F342), E(0x1F335), E(0x1F334), E(0x1F308), E(0x2600), E(0x1F319), E(0x2B50), E(0x2728)]),
        ("LOVE AND PARTIES", [E(0x1F496), E(0x1F48C), E(0x1F48D), E(0x1F339), E(0x1F388), E(0x1F389), E(0x1F382), E(0x1F381), E(0x1F942), E(0x1F3B5), E(0x1F3C6), E(0x1F451)]),
        ("LIFE", [E(0x1F3E0), E(0x1F476), E(0x1F431), E(0x1F436), E(0x1F98B), E(0x2615), E(0x1F370), E(0x1F355), E(0x1F4F7), E(0x1F697), E(0x2708), E(0x1F3D6)]),
        ("WORK AND STUDY", [E(0x1F4BC), E(0x1F4BB), E(0x1F4DA), V(0x270F), E(0x1F4DD), E(0x1F393), E(0x1F52C), E(0x1F3A8), E(0x1F373), E(0x1F3E5), E(0x1F527), E(0x1F4C8)]),
        ("TREASURE AND FANTASY", [E(0x1F4B0), E(0x1F48E), V(0x1F5DD), V(0x1F5FA), E(0x1F9ED), E(0x1FA99), E(0x1F9D9), E(0x1F409), E(0x1F52E), E(0x1F984), V(0x2694), E(0x1F3F0)]),
        ("HOLIDAYS", [E(0x1F383), E(0x1F384), V(0x2744), V(0x2603), E(0x1F430), E(0x1F95A), E(0x1F386), V(0x1F56F), E(0x1F9E7), E(0x1F36C), E(0x1F49D), E(0x1F380)]),
        ("SPORTS AND HOBBIES", [E(0x26BD), E(0x1F3C0), E(0x1F3AE), E(0x1F3B8), E(0x1F3A3), E(0x1F3CA), E(0x1F6B4), E(0x1F9E9), E(0x1F3B2), E(0x1F4D6), E(0x1F9D8), V(0x265F)]),
        ("FACES", [E(0x1F60A), E(0x1F602), E(0x1F60D), E(0x1F60E), E(0x1F622), E(0x1F631), E(0x1F914), E(0x1F634), E(0x1F973), E(0x1F47B), E(0x1F47D), E(0x1F525)]),
    ];

    // lined, squared or dotted paper drawn over a size, a line every step
    // The paper patterns, by key, with their names in Customize
    static readonly (string Key, string Name)[] Papers =
    [
        ("none", "Plain"), ("lines", "Lined"), ("grid", "Squared"), ("dots", "Dotted"),
        ("graph", "Graph"), ("diagonal", "Diagonal"), ("crosshatch", "Hatched"), ("gingham", "Gingham"), ("polka", "Polka dots"),
        ("music", "Music"), ("isometric", "Isometric"), ("hexagons", "Hexagon"), ("waves", "Waves"), ("confetti", "Confetti"),
        ("hearts", "Hearts"), ("stars", "Stars"), ("flowers", "Flowers"), ("clouds", "Clouds"), ("scales", "Scallops"), ("sparkles", "Sparkles"),
    ];

    // The paper drawn as one shape (a few paths, never an element per line or dot), in the board's ink
    static void DrawPaper(Canvas c, AlbumLook look, double w, double h, double step)
    {
        c.Children.Clear();
        if (look.Pattern is "none" or null) return;
        var light = IsLight(BoardColor(look));
        // (the strength scales the ink: faint to strong)
        byte A(double a) => (byte)Math.Clamp(a * look.PaperStrength, 4, 255);
        var ink = new SolidColorBrush(light ? Windows.UI.Color.FromArgb(A(0x30), 0x3A, 0x34, 0x2A) : Windows.UI.Color.FromArgb(A(0x1C), 0xFF, 0xFF, 0xFF));
        var strong = new SolidColorBrush(light ? Windows.UI.Color.FromArgb(A(0x4A), 0x3A, 0x34, 0x2A) : Windows.UI.Color.FromArgb(A(0x30), 0xFF, 0xFF, 0xFF));
        var lines = new GeometryGroup();
        var fills = new GeometryGroup();
        void Line(double x1, double y1, double x2, double y2) => lines.Children.Add(new LineGeometry { StartPoint = new(x1, y1), EndPoint = new(x2, y2) });
        void Dot(double x, double y, double r) => fills.Children.Add(new EllipseGeometry { Center = new(x, y), RadiusX = r, RadiusY = r });
        void Stroke(Geometry g, Brush b, double t) => c.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = g, Stroke = b, StrokeThickness = t });
        static PathGeometry Poly(params (double X, double Y)[] p)
        {
            var fig = new PathFigure { StartPoint = new(p[0].X, p[0].Y), IsClosed = true, IsFilled = true };
            for (int i = 1; i < p.Length; i++) fig.Segments.Add(new LineSegment { Point = new(p[i].X, p[i].Y) });
            return new PathGeometry { Figures = { fig } };
        }
        switch (look.Pattern)
        {
            case "lines":
                for (double y = step; y < h; y += step) Line(0, y, w, y);
                // (the notebook's margin line)
                Stroke(new LineGeometry { StartPoint = new(step * 3, 0), EndPoint = new(step * 3, h) }, new SolidColorBrush(Windows.UI.Color.FromArgb(0x55, 0xE0, 0x6A, 0x6A)), 1.5);
                break;
            case "grid":
                for (double y = step; y < h; y += step) Line(0, y, w, y);
                for (double x = step; x < w; x += step) Line(x, 0, x, h);
                break;
            case "dots":
                for (double y = step; y < h; y += step) for (double x = step; x < w; x += step) Dot(x, y, 1.2);
                break;
            case "graph":
            {
                // (fine squares, every fifth line stronger, as engineering paper)
                var major = new GeometryGroup();
                var fine = step / 2;
                int i = 1;
                for (double y = fine; y < h; y += fine, i++) (i % 5 == 0 ? major : lines).Children.Add(new LineGeometry { StartPoint = new(0, y), EndPoint = new(w, y) });
                i = 1;
                for (double x = fine; x < w; x += fine, i++) (i % 5 == 0 ? major : lines).Children.Add(new LineGeometry { StartPoint = new(x, 0), EndPoint = new(x, h) });
                Stroke(major, strong, 1);
                break;
            }
            case "diagonal":
                for (double d = -h; d < w; d += step) Line(d, h, d + h, 0);
                break;
            case "crosshatch":
                for (double d = -h; d < w; d += step * 1.4) { Line(d, h, d + h, 0); Line(d, 0, d + h, h); }
                break;
            case "gingham":
            {
                // (see-through bands both ways: where they cross, the colour doubles)
                var band = step * 1.2;
                var across = new GeometryGroup();
                for (double y = 0; y < h; y += band * 2) across.Children.Add(new RectangleGeometry { Rect = new(0, y, w, band) });
                for (double x = 0; x < w; x += band * 2) fills.Children.Add(new RectangleGeometry { Rect = new(x, 0, band, h) });
                c.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = across, Fill = ink });
                c.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = fills, Fill = ink });
                return;
            }
            case "polka":
            {
                var gap = step * 2.4;
                int row = 0;
                for (double y = gap / 2; y < h + gap; y += gap * 0.5, row++)
                    for (double x = row % 2 == 0 ? gap / 2 : gap; x < w + gap; x += gap) Dot(x, y, step * 0.32);
                break;
            }
            case "music":
            {
                // (staves of five lines, a gap between them)
                var line = step * 0.55;
                for (double top = step * 1.5; top + line * 4 < h; top += line * 4 + step * 2.2)
                    for (int k = 0; k < 5; k++) Line(0, top + k * line, w, top + k * line);
                break;
            }
            case "isometric":
            {
                var dx = step * 1.2;
                var dy = dx * 0.866;
                int row = 0;
                for (double y = dy / 2; y < h; y += dy, row++)
                    for (double x = row % 2 == 0 ? dx / 2 : dx; x < w; x += dx) Dot(x, y, 1.2);
                break;
            }
            case "hexagons":
            {
                // (pointy-top hexagons sharing their sides)
                var r = step * 0.9;
                var hw = r * Math.Sqrt(3);
                int row = 0;
                for (double cy = 0; cy < h + r * 2; cy += r * 1.5, row++)
                    for (double cx = row % 2 == 0 ? 0 : hw / 2; cx < w + hw; cx += hw)
                    {
                        var fig = new PathFigure { StartPoint = new(cx, cy - r), IsClosed = true };
                        for (int k = 1; k < 6; k++)
                        {
                            var a = Math.PI / 3 * k - Math.PI / 2;
                            fig.Segments.Add(new LineSegment { Point = new(cx + r * Math.Cos(a), cy + r * Math.Sin(a)) });
                        }
                        lines.Children.Add(new PathGeometry { Figures = { fig } });
                    }
                break;
            }
            case "waves":
            {
                var amp = step * 0.28;
                var len = step * 2.4;
                for (double y = step; y < h + amp; y += step * 1.3)
                {
                    var fig = new PathFigure { StartPoint = new(0, y) };
                    for (double x = 0; x < w; x += len)
                        fig.Segments.Add(new BezierSegment { Point1 = new(x + len / 3, y - amp * 2), Point2 = new(x + len * 2 / 3, y + amp * 2), Point3 = new(x + len, y) });
                    lines.Children.Add(new PathGeometry { Figures = { fig } });
                }
                break;
            }
            case "confetti":
            {
                // (little dashes and dots thrown about, the same each time)
                var rnd = new Random(7);
                var n = (int)(w * h / (step * step * 1.6));
                for (int i = 0; i < n; i++)
                {
                    double x = rnd.NextDouble() * w, y = rnd.NextDouble() * h;
                    if (rnd.Next(3) == 0) Dot(x, y, step * 0.12);
                    else
                    {
                        var a = rnd.NextDouble() * Math.PI;
                        var l = step * 0.3;
                        Line(x - Math.Cos(a) * l, y - Math.Sin(a) * l, x + Math.Cos(a) * l, y + Math.Sin(a) * l);
                    }
                }
                Stroke(lines, strong, 1.6);
                if (fills.Children.Count > 0) c.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = fills, Fill = strong });
                return;
            }
            case "hearts":
            case "stars":
            case "flowers":
            case "sparkles":
            {
                // (little shapes on a staggered grid, small and faint so they stay behind the pictures)
                var gap = step * 2.6;
                var k = step * 0.32;
                int row = 0;
                fills.FillRule = FillRule.Nonzero;
                for (double y = gap / 2; y < h + gap; y += gap * 0.75, row++)
                    for (double x = row % 2 == 0 ? gap / 2 : gap; x < w + gap; x += gap)
                        switch (look.Pattern)
                        {
                            case "hearts":
                                // (two round tops and a point)
                                Dot(x - k * 0.5, y - k * 0.2, k * 0.55);
                                Dot(x + k * 0.5, y - k * 0.2, k * 0.55);
                                fills.Children.Add(Poly((x - k * 1.02, y), (x + k * 1.02, y), (x, y + k * 1.15)));
                                break;
                            case "stars":
                            {
                                var pts = new (double, double)[10];
                                for (int i = 0; i < 10; i++)
                                {
                                    var a = Math.PI / 5 * i - Math.PI / 2;
                                    var rr = i % 2 == 0 ? k * 1.1 : k * 0.45;
                                    pts[i] = (x + rr * Math.Cos(a), y + rr * Math.Sin(a));
                                }
                                fills.Children.Add(Poly(pts));
                                break;
                            }
                            case "flowers":
                                for (int i = 0; i < 5; i++)
                                {
                                    var a = Math.PI * 2 / 5 * i;
                                    Dot(x + k * 0.62 * Math.Cos(a), y + k * 0.62 * Math.Sin(a), k * 0.42);
                                }
                                break;
                            case "sparkles":
                                // (a four-pointed twinkle, every other one smaller)
                                var big = (row + (int)(x / gap)) % 2 == 0 ? k * 1.1 : k * 0.65;
                                fills.Children.Add(Poly((x, y - big), (x + big * 0.22, y - big * 0.22), (x + big, y), (x + big * 0.22, y + big * 0.22),
                                    (x, y + big), (x - big * 0.22, y + big * 0.22), (x - big, y), (x - big * 0.22, y - big * 0.22)));
                                break;
                        }
                break;
            }
            case "clouds":
            {
                // (soft puffs scattered about, the same each time)
                var rnd = new Random(11);
                fills.FillRule = FillRule.Nonzero;
                var n = Math.Max(1, (int)(w * h / (step * step * 26)));
                for (int i = 0; i < n; i++)
                {
                    double x = rnd.NextDouble() * w, y = rnd.NextDouble() * h, k = step * (0.5 + rnd.NextDouble() * 0.4);
                    Dot(x, y, k);
                    Dot(x - k * 0.95, y + k * 0.3, k * 0.7);
                    Dot(x + k * 0.95, y + k * 0.3, k * 0.75);
                    fills.Children.Add(new RectangleGeometry { Rect = new(x - k * 0.95, y + k * 0.2, k * 1.9, k * 0.8) });
                }
                break;
            }
            case "scales":
            {
                // (rows of half circles, each row half a step over: a scalloped edge)
                var rr = step * 0.7;
                int row = 0;
                for (double y = rr; y < h + rr; y += rr, row++)
                    for (double x = row % 2 == 0 ? 0 : rr; x < w + rr * 2; x += rr * 2)
                    {
                        var fig = new PathFigure { StartPoint = new(x - rr, y) };
                        fig.Segments.Add(new ArcSegment { Point = new(x + rr, y), Size = new(rr, rr), SweepDirection = SweepDirection.Clockwise });
                        lines.Children.Add(new PathGeometry { Figures = { fig } });
                    }
                break;
            }
            default:
                return;
        }
        if (lines.Children.Count > 0) Stroke(lines, ink, 1);
        // (the clouds' big puffs a shade lighter, so they stay soft)
        if (fills.Children.Count > 0) c.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = fills, Fill = ink, Opacity = look.Pattern == "clouds" ? 0.55 : 1 });
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
        Keep();
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
        Canvas.SetZIndex(outer, 3000 + order);   // (stickers over everything, a picture lifted under the pointer included)
        Land(tilt, s.Angle, 200 + order * 90, 0, look.Entrance);
        Draggable(outer, () => { s.X = Canvas.GetLeft(outer); s.Y = Canvas.GetTop(outer); Keep(); });
        var menu = new MenuFlyout();
        void Resize(double by) { s.Size = Math.Clamp(s.Size * by, 0.5, 3); text.FontSize = 64 * s.Size * _boardScale; Keep(); }
        var bigger = new MenuFlyoutItem { Text = T("Bigger"), Icon = new FontIcon { Glyph = "\uE8A3" } };
        bigger.Click += (_, _) => Resize(1.25);
        var smaller = new MenuFlyoutItem { Text = T("Smaller"), Icon = new FontIcon { Glyph = "\uE71F" } };
        smaller.Click += (_, _) => Resize(0.8);
        var off = new MenuFlyoutItem { Text = T("Take it off"), Icon = new FontIcon { Glyph = "\uE74D" } };
        off.Click += (_, _) => { look.Stickers.Remove(s); Keep(); _boardCanvas?.Children.Remove(outer); };
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
    // the board scaled to a height straight away, the layout left as it is until it is laid again
    Border? _boardSheet;
    double _boardBuiltHeight = 1;
    void LiveScale(double height)
    {
        if (_boardSheet is null || _boardCanvas is null) return;
        var k = height / _boardBuiltHeight;
        _boardSheet.Height = height;
        _boardCanvas.RenderTransform = Math.Abs(k - 1) < 0.001 ? null : new ScaleTransform { ScaleX = k, ScaleY = k, CenterX = _boardScroll?.HorizontalOffset ?? 0 };
    }

    void RefreshBoard(bool page = false, int settle = 120)
    {
        var mine = ++_boardRedraw;
        After(page ? 0 : settle, () =>
        {
            if (mine != _boardRedraw || !_galBook) return;
            // (an album open over the board flies its pictures back to these cards: the board is laid again once it closes)
            if (_spotlightOpen && !page) { _boardStale = true; return; }
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
        Scroller.Padding = on ? new Thickness(0) : new Thickness(24, 20, 24, 0);
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
            _galPage.Height = Math.Max(420, Scroller.ActualHeight);
        };
    }

    UIElement GalleryBoard()
    {
        var look = Gallery.Look(_gal!, _galAlbum);
        // (boards laid out before the timeline get the timeline's layout once)
        if (look.Layout < 6) { look.Places.Clear(); look.Gaps.Clear(); look.Layout = 6; }
        if (look.Layout < 7)
        {
            foreach (var (key, pl) in look.Places) if (key.StartsWith(Gallery.Mine, StringComparison.Ordinal) && pl.Moved) pl.X = 0;
            look.Layout = 7;
        }
        _boardAnchor.Clear();
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
            foreach (var g in look.Gaps) g.Width *= k;
            look.SeenX *= k;
        }
        look.Height = BoardHeight;
        var s = _boardScale = BoardHeight / 660;
        _boardBuiltSize = look.Size;
        var mid = BoardHeight / 2;
        // the newest sixty, laid newest first along a gentle wave around the line, a breath of room where a day begins;
        // an album gathered into a pile takes one place, where its newest picture would lie
        var pics = InAlbum().Take(60).ToList();
        var chapters = _galAlbum.StartsWith(Gallery.Mine, StringComparison.Ordinal) ? []
            : _gal!.Custom.Where(c => pics.Any(pc => c.Pictures.Contains(pc.Path, StringComparer.OrdinalIgnoreCase))).ToList();
        _boardPiled.Clear();
        foreach (var c in chapters)
            if (!_spread.Contains(c.Key))
                foreach (var path in c.Pictures) _boardPiled.TryAdd(path, c);
        // (pictures taken since the board was last laid; each one's look is counted from the oldest picture, so they
        // change no other picture's size or place on the wave)
        var fresh = look.Seen == default || _galQuery.Length > 0 ? 0 : pics.TakeWhile(pc => pc.Taken > look.Seen).Count();
        var lead = look.Lead + fresh;
        var firstOf = new Dictionary<CustomAlbum, int>();
        var xs = new double[pics.Count];
        var rawX = new double[pics.Count];
        var slotOf = new int[pics.Count];
        var days = new List<(double X, DateTime Day)>();
        double x = 0;
        var slot = 0;
        var freshSlots = 0;
        (double W, double H) Measured(int kind)
        {
            if (pics.Count == 0) return (0, 0);
            var calm = _boardCalm;
            _boardCalm = true;
            try
            {
                var probe = Framed(pics[0], new Placed(), look, ink, 0, 0, pics, null, still: true, kind: kind);
                probe.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
                return (probe.DesiredSize.Width, probe.DesiredSize.Height);
            }
            finally { _boardCalm = calm; }
        }
        var bigCard = Measured(0);
        var smallCard = Measured(1);
        // how wide a picture (or a pile) lies, turned as far as it may be
        double Footprint(int i, bool pile)
        {
            var (w, h) = Kind(i) == 0 ? bigCard : smallCard;
            if (w <= 0) { w = (Kind(i) == 0 ? 290 : 230) * s * look.Size; h = w * 0.8; }
            var turned = w + h * Math.Sin(Math.Min(10, 5 * look.Tilt) * Math.PI / 180);
            return pile ? turned + 32 * s + h * 0.12 : turned;
        }
        double Wide(int i) => Footprint(i, _boardPiled.ContainsKey(pics[i].Path)) + 24;
        void LayTimeline()
        {
            firstOf.Clear();
            days.Clear();
            x = 40 * s;
            slot = 0;
            DateTime? lastDay = null;
            for (int i = 0; i < pics.Count; i++)
            {
                // (room the player's arrangement needed when newer pictures arrived)
                foreach (var g in look.Gaps)
                    if (pics[i].Taken <= g.At && (i == 0 || pics[i - 1].Taken > g.At)) x += g.Width;
                var heap = _boardPiled.GetValueOrDefault(pics[i].Path);
                if (heap is not null && HeadOf(heap) != i) continue;
                if (heap is not null) firstOf[heap] = i;
                if (lastDay != pics[i].Taken.Date) { if (lastDay is not null) x += 40 * s * look.Spacing; days.Add((x, pics[i].Taken.Date)); x += 150 * s; lastDay = pics[i].Taken.Date; }
                if (heap is not null) _boardAnchor[heap.Key] = x;
                xs[i] = rawX[i] = x;
                if (i < fresh) freshSlots = slot + 1;
                slotOf[i] = slot++;
                var pinned = look.Places.TryGetValue(heap?.Key ?? pics[i].Path, out var pin) && pin.Moved;
                var next = x + Wide(i) * 0.95 * look.Spacing;
                if (look.Spacing >= 1 && (heap is not null || !pinned))
                    next = Math.Max(next, x + (pinned ? pin!.X : 12) + Footprint(i, heap is not null) + 18 * s * look.Spacing);
                x = next;
            }
            // (the other pictures of a pile lie on its place)
            for (int i = 0; i < pics.Count; i++)
                if (_boardPiled.GetValueOrDefault(pics[i].Path) is { } heap && firstOf.TryGetValue(heap, out var head) && head != i) { xs[i] = xs[head]; rawX[i] = rawX[head]; slotOf[i] = slotOf[head]; }
        }
        int Kind(int i) => ((i - lead) % 4 + 4) % 4;
        // a pile's own picture: the one it was made on when it is here, else its newest
        var headOf = new Dictionary<CustomAlbum, int>();
        for (int i = pics.Count - 1; i >= 0; i--)
            if (_boardPiled.GetValueOrDefault(pics[i].Path) is { } heap)
                if (!headOf.ContainsKey(heap) || !(look.Places.TryGetValue(heap.Key, out var pp) && string.Equals(pp.On, pics[headOf[heap]].Path, StringComparison.OrdinalIgnoreCase))) headOf[heap] = i;
        int HeadOf(CustomAlbum heap) => headOf[heap];
        LayTimeline();
        if (fresh > 0 && fresh < pics.Count)
        {
            // everything already on the board moved along with its pictures: the notes and stickers go with them
            var along = look.SeenX > 0 ? rawX[fresh] - look.SeenX : 0;
            foreach (var nt in look.Notes) nt.X += along;
            foreach (var sk in look.Stickers) sk.X += along;
            // and when something the player put there lies where the new pictures go, room opens for them in front of it
            // (the room ends where the next picture would lie on its own, so the new ones sit as far apart as any others)
            var newRight = Enumerable.Range(0, fresh).Max(i => xs[i] + Wide(i) * 0.95 * look.Spacing + 12);
            var oldLeft = double.MaxValue;
            for (int i = fresh; i < pics.Count; i++)
            {
                var heap = _boardPiled.GetValueOrDefault(pics[i].Path);
                if (heap is not null ? look.Places.TryGetValue(heap.Key, out var pl) && pl.Moved : look.Places.TryGetValue(pics[i].Path, out pl) && pl.Moved)
                    oldLeft = Math.Min(oldLeft, rawX[i] + pl.X);
            }
            foreach (var nt in look.Notes) oldLeft = Math.Min(oldLeft, nt.X);
            var need = newRight - oldLeft;
            if (need > 0)
            {
                look.Gaps.Add(new TimeGap { At = pics[fresh].Taken, Width = need });
                foreach (var nt in look.Notes) nt.X += need;
                foreach (var sk in look.Stickers) sk.X += need;
                LayTimeline();
            }
        }
        if (pics.Count > 0 && _galQuery.Length == 0)
        {
            look.Seen = pics.Max(pc => pc.Taken);
            look.SeenX = rawX[0];
            look.Lead += fresh;
            look.SlotLead += freshSlots;
        }
        var width = Math.Max(1100, pics.Count == 0 ? 0 : x + 110 * s);
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
            var empty = new TextBlock { Text = _galQuery.Length > 0 ? T("nothing found for “{0}”", _galQuery) : T("no pictures here yet"), FontFamily = new FontFamily(look.Font), FontSize = 30, Foreground = ink };
            Canvas.SetLeft(empty, 140); Canvas.SetTop(empty, mid - 90);
            canvas.Children.Add(empty);
        }
        var roof = 14.0;
        _boardPlaces.Clear();
        _restZ.Clear();
        var inPile = new Dictionary<CustomAlbum, int>();
        var pileBase = new Dictionary<CustomAlbum, Placed>();
        // where a picture lies by itself: along the wave of the timeline, a little jitter of its own
        Placed Auto(int i, bool piled)
        {
            var jitter = new Random(Stable(pics[i].Path) + look.Seed);
            var phase = (i - lead) * 1.1 + look.Seed % 7;
            var hh = ((Kind(i) == 0 ? 194 : 154) * s * look.Size) + 70;
            return new Placed
            {
                X = xs[i] + jitter.Next(-12, 12),
                Y = Math.Clamp(mid - hh / 2 + Math.Sin(phase) * mid * 0.5, roof, Math.Max(roof, BoardHeight - hh - (piled ? 70 : 14))),
                Angle = Math.Round(-Math.Cos(phase) * 5 + jitter.NextDouble() * 3 - 1.5, 1),
            };
        }
        for (int i = 0; i < pics.Count; i++)
        {
            var path = pics[i].Path;
            var heap = _boardPiled.GetValueOrDefault(path);
            var own = heap is null ? i : firstOf[heap];
            var h = ((Kind(own) == 0 ? 194 : 154) * s * look.Size) + 70;
            var auto = heap is null ? Auto(i, false) : null;
            Placed place;
            if (heap is not null)
            {
                var k = inPile.GetValueOrDefault(heap);
                inPile[heap] = k + 1;
                if (k == 0) pileBase[heap] = look.Places.TryGetValue(heap.Key, out var parked) && parked.Moved
                    ? new Placed { X = rawX[own] + parked.X, Y = Math.Clamp(parked.Y, roof, Math.Max(roof, BoardHeight - h - 70)) - 24 * s }
                    : Auto(own, true);
                var under = pileBase[heap];
                var shift = PileShift[k % PileShift.Length];
                place = new Placed { X = under.X + shift.X * s, Y = under.Y + shift.Y * s + 24 * s, Angle = PileTurns[k % PileTurns.Length] };
            }
            // (a picture put somewhere always lies whole on the board, whatever its height since)
            else place = look.Places.TryGetValue(path, out var put) && put.Moved ? new Placed { X = Math.Max(8, xs[i] + put.X), Y = Math.Clamp(put.Y, roof, Math.Max(roof, BoardHeight - h - 14)), Angle = put.Angle, Moved = true } : auto!;
            _boardAnchor[path] = xs[i];
            _boardPlaces[path] = place;
            // (the newest fall in first, the ones further back already lie there when scrolled to)
            var delay = 120 + Math.Min(slotOf[i], 10) * 130;
            var card = Framed(pics[i], place, look, ink, i, delay, pics, heap, bare: heap is not null && inPile[heap] > 1, kind: Kind(own));
            if (heap is not null) card.ContextFlyout = PileMenu(heap);
            if (heap is not null) Canvas.SetZIndex(card, firstOf[heap] * 10 + 9 - inPile[heap]);
            // (a picture the player put down lies over the ones it was put on, the last one put down on top)
            else if (place.Moved && look.Places.TryGetValue(path, out var top) && top.Z > 0) _restZ[card] = 600 + look.Places.Values.Count(o => o.Moved && o.Z > 0 && o.Z < top.Z);
            if (_restZ.TryGetValue(card, out var z)) Canvas.SetZIndex(card, z);
            canvas.Children.Add(card);
            _boardCards.Add((pics[i], card, i));
            // (laid again after an album was spread or gathered: each picture slides over from where it was)
            if (_boardFlip is not null && _boardFlip.TryGetValue(path, out var was) && card is Border { Child: FrameworkElement { RenderTransform: CompositeTransform moved } })
            {
                moved.TranslateX = was.X - place.X;
                moved.TranslateY = was.Y - place.Y;
                var slide = new Storyboard();
                var glide = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.3 };
                Tween(slide, moved, "TranslateX", 0, 640, glide);
                Tween(slide, moved, "TranslateY", 0, 640, glide);
                Play(slide);
            }
        }
        _boardFlip = null;

        // each album's tag: under its pile (fanned by the pointer, spread with a click), or over its first picture once spread
        foreach (var c in chapters)
        {
            var members = pics.Where(pc => c.Pictures.Contains(pc.Path, StringComparer.OrdinalIgnoreCase)).ToList();
            var gathered = members.Any(m => _boardPiled.GetValueOrDefault(m.Path) == c);
            // (a picture in two albums lies in one pile: the other album's tag goes with its own pictures, or not at all)
            if (gathered) members = members.Where(m => _boardPiled.GetValueOrDefault(m.Path) == c).ToList();
            else if (!_spread.Contains(c.Key)) continue;
            var anchor = _boardPlaces[members[0].Path];
            var at = pics.IndexOf(members[0]);
            var cardH = ((at % 4 == 0 ? 194 : 154) * s * look.Size) + 70;
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            row.Children.Add(new FontIcon { Glyph = c.Icon, FontSize = 14, Foreground = ink, VerticalAlignment = VerticalAlignment.Center });
            var tagName = new TextBlock { Text = c.Name, FontFamily = new FontFamily(look.Font), FontSize = 20, Foreground = ink, VerticalAlignment = VerticalAlignment.Center };
            row.Children.Add(tagName);
            row.Children.Add(new TextBlock { Text = members.Count.ToString(), FontSize = 12, Foreground = ink, Opacity = 0.6, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 2, 0, 0) });
            HyperlinkButton TagButton(UIElement content, Thickness pad)
            {
                var b = new HyperlinkButton { Content = content, Padding = pad, Background = pill, CornerRadius = new CornerRadius(15), Height = 30 };
                b.Resources["HyperlinkButtonBackgroundPointerOver"] = pill;
                b.Resources["HyperlinkButtonBackgroundPressed"] = pill;
                b.Resources["HyperlinkButtonForegroundPointerOver"] = ink;
                b.Resources["HyperlinkButtonForegroundPressed"] = ink;
                return b;
            }
            var toggle = TagButton(row, new Thickness(12, 0, 12, 2));
            var say = T("Look through {0}", c.Name);
            ToolTipService.SetToolTip(toggle, say);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(toggle, say);
            toggle.PointerEntered += (_, _) => tagName.TextDecorations = Windows.UI.Text.TextDecorations.Underline;
            toggle.PointerExited += (_, _) => tagName.TextDecorations = Windows.UI.Text.TextDecorations.None;
            var key = c.Key;
            var albumCards = members.Select(m => _boardCards.First(cc => cc.Pic.Path == m.Path).Card).ToList();
            toggle.Click += (_, _) => Spotlight(c, members, albumCards);
            var open = TagButton(new FontIcon { Glyph = "\uE8A7", FontSize = 12, Foreground = ink }, new Thickness(0));
            open.Width = 30;
            ToolTipService.SetToolTip(open, T("Open the album {0}", c.Name));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(open, T("Open the album {0}", c.Name));
            open.Click += (_, _) => { _galAlbum = key; BuildGallery(); };
            var tags = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            tags.Children.Add(toggle);
            tags.Children.Add(open);
            Canvas.SetLeft(tags, anchor.X + 8);
            Canvas.SetTop(tags, gathered ? Math.Min(anchor.Y + cardH + 34 * s, BoardHeight - 38) : Math.Max(6, anchor.Y - 42));
            Canvas.SetZIndex(tags, 950);
            tags.ContextFlyout = PileMenu(c);
            canvas.Children.Add(tags);
            // (under a pile the tag sits just below its top picture, whatever the frame's height)
            if (gathered && albumCards.Count > 0)
            {
                var top = albumCards[0];
                top.SizeChanged += (_, e) => Canvas.SetTop(tags, Math.Min(Canvas.GetTop(top) + e.NewSize.Height + 12, BoardHeight - 38));
            }
            if (!_boardCalm)
            {
                tags.Opacity = 0;
                var show = new Storyboard { BeginTime = TimeSpan.FromMilliseconds(700) };
                Tween(show, tags, "Opacity", 1, 400);
                Play(show);
            }
            if (!gathered) continue;
            // (the pointer over the pile or its tag fans the pictures out a little)
            var cards = members.Select(m => _boardCards.First(cc => cc.Pic.Path == m.Path).Card).ToList();
            var over = 0;
            void Fan(bool wide)
            {
                for (int k = 0; k < cards.Count; k++)
                    if (cards[k] is Border { Child: FrameworkElement { RenderTransform: CompositeTransform t } })
                    {
                        var sb = new Storyboard();
                        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
                        Tween(sb, t, "TranslateX", wide ? k * 54 * s : 0, 320, ease);
                        Tween(sb, t, "Rotation", wide ? -8 + k * 6 : PileTurns[k % PileTurns.Length] * look.Tilt, 320, ease);
                        Play(sb);
                    }
            }
            void Enter() { if (over++ == 0) Fan(true); }
            void Leave() { over = Math.Max(0, over - 1); After(140, () => { if (over == 0) Fan(false); }); }
            foreach (var card in cards) { card.PointerEntered += (_, _) => Enter(); card.PointerExited += (_, _) => Leave(); }
            tags.PointerEntered += (_, _) => Enter();
            tags.PointerExited += (_, _) => Leave();
            // (dragged by any of its pictures, the whole pile and its tag move together and stay where they are let go)
            foreach (var card in cards)
            {
                card.ManipulationMode = ManipulationModes.TranslateX | ManipulationModes.TranslateY;
                card.ManipulationStarted += (_, _) => { _dragging = true; Fan(false); foreach (var cc in cards) cc.Scale = new Vector3(1.04f, 1.04f, 1); };
                card.ManipulationDelta += (_, e) =>
                {
                    var dx = e.Delta.Translation.X;
                    var dy = Math.Clamp(e.Delta.Translation.Y, -Canvas.GetTop(cards[0]) - 10, BoardHeight - 120 - Canvas.GetTop(cards[0]));
                    foreach (var cc in cards) { Canvas.SetLeft(cc, Canvas.GetLeft(cc) + dx); Canvas.SetTop(cc, Canvas.GetTop(cc) + dy); }
                    Canvas.SetLeft(tags, Canvas.GetLeft(tags) + dx);
                    Canvas.SetTop(tags, Canvas.GetTop(tags) + dy);
                };
                card.ManipulationCompleted += (_, _) =>
                {
                    _dragging = false;
                    foreach (var cc in cards) cc.Scale = Vector3.One;
                    look.Places[c.Key] = new Placed { X = Canvas.GetLeft(cards[0]) - _boardAnchor.GetValueOrDefault(c.Key), Y = Canvas.GetTop(cards[0]), Moved = true };
                    Keep();
                };
                card.Tapped += (_, e) => { e.Handled = true; Spotlight(c, members, cards); };
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
        var keepAt = _boardKeepAt;
        _boardKeep = null;
        _boardKeepAt = null;
        var zoom = _boardCalm && _boardFrom > 0 ? _boardFrom / BoardHeight : 1;
        _boardFrom = 0;
        scroll.LayoutUpdated += (_, _) =>
        {
            if (opened || scroll.ScrollableWidth <= 0) return;
            opened = true;
            scroll.ChangeView(Math.Min(keepAt ?? (keep ?? 0) * scroll.ScrollableWidth, scroll.ScrollableWidth), null, null, true);
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
        _boardSheet = frame;
        _boardBuiltHeight = BoardHeight;

        // the strip of dates under the board: each day where it lies, the part in view lit; a click goes there
        var strip = new Grid { Height = 40, Margin = new Thickness(32, 4, 32, 18), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        var track = new Canvas();
        strip.Children.Add(track);
        var window = new Border { Height = 8, CornerRadius = new CornerRadius(4), Background = Res("ApexAccentBrush"), Opacity = 0.9 };
        Canvas.SetTop(window, 23);
        void Lay()
        {
            var tw = strip.ActualWidth;
            if (tw <= 0) return;
            track.Children.Clear();
            var rule = new Border { Width = tw, Height = 4, CornerRadius = new CornerRadius(2), Background = ink, Opacity = 0.16 };
            Canvas.SetTop(rule, 25);
            track.Children.Add(rule);
            var lastRight = -1000.0;
            foreach (var (dx, d) in days)
            {
                var at = dx / width * tw;
                var tick = new Border { Width = 2, Height = 8, CornerRadius = new CornerRadius(1), Background = ink, Opacity = 0.5 };
                Canvas.SetLeft(tick, at); Canvas.SetTop(tick, 23);
                track.Children.Add(tick);
                if (at < lastRight + 8) continue;  // (labels that would touch stay ticks)
                var label = new TextBlock { Text = d.ToString("d MMM", LangCulture()), FontSize = 11, Foreground = ink, Opacity = 0.75 };
                Canvas.SetLeft(label, at - 2); Canvas.SetTop(label, 2);
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
        KeepQuiet();
        return all;
    }

    // a picture in the album's frame: it falls in turning and bounces, its tape sticks, its caption writes itself; dragged
    // to move, lifted under the pointer, its caption written on with a click, opened with a double-click
    FrameworkElement Framed(Picture p, Placed place, AlbumLook look, Brush ink, int order, int delay, List<Picture> pics, CustomAlbum? pile = null, bool still = false, bool bare = false, double? read = null, bool shut = false, double captionMax = double.MaxValue, int? kind = null)
    {
        var info = Gallery.Peek(_gal!, p.Path);
        var big = (kind ?? order) % 4 == 0;
        var style = kind ?? order;
        double w = (big ? 290 : 230) * _boardScale * look.Size, h = (big ? 194 : 154) * _boardScale * look.Size;
        var source = Thumb(p.Path, read ?? w);
        var dark = new SolidColorBrush(Rgb(0x2B, 0x2A, 0x26));
        var caption = new TextBlock { FontFamily = new FontFamily(look.Font), FontSize = Math.Min(captionMax, (big ? 24 : 20) * Math.Max(0.85, _boardScale)), TextTrimming = TextTrimming.CharacterEllipsis, TextWrapping = TextWrapping.Wrap, MaxLines = 2, MaxWidth = w + 8, MinHeight = 28, HorizontalAlignment = HorizontalAlignment.Left };
        if (info.Caption is { Length: > 40 } longText) ToolTipService.SetToolTip(caption, longText);
        var stack = new StackPanel();
        FrameworkElement body;
        FrameworkElement? capBox = null;   // the tag a caption sits on, when the frame gives it one
        var tape = false;
        switch (look.Frame)
        {
            case "rounded":
                stack.Children.Add(new Border { Width = w, Height = h, CornerRadius = new CornerRadius(20), Background = new ImageBrush { ImageSource = source, Stretch = Stretch.UniformToFill } });
                caption.Foreground = dark;
                stack.Children.Add(capBox = new Border { Child = caption, Margin = new Thickness(10, -18, 0, 0), Padding = new Thickness(12, 2, 12, 2), CornerRadius = new CornerRadius(14), HorizontalAlignment = HorizontalAlignment.Left,
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
                if (!bare) for (int q = 0; q < 4; q++) wash.Children.Add(WashiStrip(q, style, _boardScale));
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
        if (tape && look.Tape != "none" && !bare)
        {
            strip = new Border { Width = 96, Height = 22, Background = TapeBrush(look, style), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, -22, 0, 0),
                RenderTransform = new CompositeTransform { Rotation = (style % 3 - 1) * 4, ScaleX = _boardCalm ? 1 : 0 }, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5) };
            holder.Children.Add(strip);
        }
        if (p.Video) holder.Children.Add(PlayMark(Math.Max(34, 52 * _boardScale)));
        var outer = new Border { Child = holder };
        CardPaths.AddOrUpdate(outer, p.Path);
        Canvas.SetLeft(outer, place.X);
        Canvas.SetTop(outer, place.Y);
        Canvas.SetZIndex(outer, order * 10);
        outer.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(220) };
        outer.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(240) };
        outer.Translation = new Vector3(0, 0, 32);
        outer.SizeChanged += (_, e) => outer.CenterPoint = new Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0);
        var angle = place.Angle * look.Tilt;
        var turn = Land(holder, angle, delay, style % 2 == 0 ? -50 : 50, look.Entrance);
        var text = info.Caption is { Length: > 0 } c ? c : T("+ write something");
        if (strip is not null && !_boardCalm)
        {
            var tapeIn = new Storyboard { BeginTime = TimeSpan.FromMilliseconds(delay + 640) };
            Tween(tapeIn, strip.RenderTransform, "ScaleX", 1, 420, new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.6 });
            Play(tapeIn);
        }
        if (_boardCalm) caption.Text = text; else After(delay + 820, () => Write(caption, text));
        var blank = info.Caption is not { Length: > 0 };
        var editing = false;
        // (an empty caption, and its tag on a rounded frame, shows only while the picture is under the pointer)
        var capHost = capBox ?? caption;
        capHost.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
        if (blank) capHost.Opacity = 0;
        if (blank && !bare && !shut) { outer.PointerEntered += (_, _) => capHost.Opacity = 1; outer.PointerExited += (_, _) => { if (!editing && Gallery.Peek(_gal!, p.Path).Caption is not { Length: > 0 }) capHost.Opacity = 0; }; }

        // (under the pointer it lifts, grows a touch and nearly straightens; back to its tilt when left)
        outer.PointerEntered += (_, _) => { if (_dragging || pile is not null || still) return; outer.Translation = new Vector3(0, -12, 56); outer.Scale = new Vector3(1.04f, 1.04f, 1); Canvas.SetZIndex(outer, 900); Turn(turn, place.Angle * look.Tilt * 0.25); };
        outer.PointerExited += (_, _) => { if (_dragging || pile is not null || still) return; outer.Translation = new Vector3(0, 0, 32); outer.Scale = Vector3.One; Canvas.SetZIndex(outer, _restZ.GetValueOrDefault(outer, order * 10)); Turn(turn, place.Angle * look.Tilt, 420, new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 }); };
        if (pile is null && !still) Draggable(outer, () => CardDropped(p, place, outer, pile), () => MarkUnder(outer), shut ? null : () => OpenViewer(pics, pics.IndexOf(p), outer));
        if (pile is null && !shut) outer.Tapped += (_, e) => { e.Handled = true; OpenViewer(pics, pics.IndexOf(p), outer); };
        caption.Tapped += (_, e) =>
        {
            e.Handled = true;
            if (shut || editing) return;
            void Put(UIElement was, UIElement now)
            {
                switch (was is FrameworkElement { Parent: var parent } ? parent : null)
                {
                    case Panel panel: panel.Children[panel.Children.IndexOf(was)] = now; break;
                    case Border border: border.Child = now; break;
                }
            }
            if (caption.Parent is not (Panel or Border)) return;
            editing = true;
            var box = new TextBox { Text = info.Caption ?? "", FontFamily = caption.FontFamily, FontSize = caption.FontSize, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0, 0, 0, 1),
                Foreground = caption.Foreground, PlaceholderText = T("write something"), MinWidth = 160, Margin = caption.Margin };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, T("Caption"));
            box.Resources["TextControlBackgroundFocused"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            box.Resources["TextControlForegroundFocused"] = caption.Foreground;
            var line = caption.Foreground is SolidColorBrush cf ? new SolidColorBrush(Windows.UI.Color.FromArgb(0x66, cf.Color.R, cf.Color.G, cf.Color.B)) : caption.Foreground;
            foreach (var key in new[] { "TextControlBorderBrush", "TextControlBorderBrushPointerOver", "TextControlBorderBrushFocused" }) box.Resources[key] = line;
            box.Resources["TextControlBorderThemeThicknessFocused"] = new Thickness(0, 0, 0, 1);
            box.Resources["TextControlBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            box.Padding = new Thickness(0, 0, 0, 2);
            Put(caption, box);
            box.Focus(FocusState.Programmatic);
            void Done()
            {
                if (!editing) return;
                editing = false;
                var keep = Gallery.Info(_gal!, p.Path);
                keep.Caption = box.Text.Trim();
                info = keep;
                Keep();
                caption.Text = keep.Caption.Length > 0 ? keep.Caption : T("+ write something");
                Put(box, caption);
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
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, T("Add a note"));
        box.Resources["TextControlBackgroundFocused"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        box.Resources["TextControlBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        box.Resources["TextControlForegroundFocused"] = new SolidColorBrush(Rgb(0x2B, 0x2A, 0x26));
        box.Resources["TextControlBorderBrushFocused"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        box.LostFocus += (_, _) => { note.Text = box.Text; Keep(); };
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
        Canvas.SetZIndex(outer, 2000 + order);
        Land(tilt, note.Angle, 160 + order * 140, 0, look.Entrance);
        outer.PointerEntered += (_, _) => close.Opacity = 1;
        outer.PointerExited += (_, _) => close.Opacity = 0;
        close.Click += (_, _) =>
        {
            look.Notes.Remove(note);
            Keep();
            var away = new Storyboard();
            Tween(away, outer, "Opacity", 0, 260);
            Tween(away, tilt.RenderTransform, "TranslateY", 60, 300, new CubicEase { EasingMode = EasingMode.EaseIn });
            Tween(away, tilt.RenderTransform, "Rotation", note.Angle + 14, 300);
            away.Completed += (_, _) => _boardCanvas?.Children.Remove(outer);
            Play(away);
        };
        Draggable(outer, () => { note.X = Canvas.GetLeft(outer); note.Y = Canvas.GetTop(outer); Keep(); });
        return outer;
    }

    bool _dragging;
    static readonly System.Runtime.CompilerServices.ConditionalWeakTable<FrameworkElement, string> CardPaths = new();
    readonly Dictionary<FrameworkElement, int> _restZ = [];
    // moved with the pointer (or a finger), lifted while it moves, kept where it is let go
    Windows.Foundation.Point _dragFrom;
    void Draggable(FrameworkElement el, Action dropped, Action? moving = null, Action? tapped = null)
    {
        el.ManipulationMode = ManipulationModes.TranslateX | ManipulationModes.TranslateY;
        Microsoft.UI.Dispatching.DispatcherQueueTimer? edge = null;
        el.ManipulationStarted += (_, _) =>
        {
            _dragFrom = new Windows.Foundation.Point(Canvas.GetLeft(el), Canvas.GetTop(el));
            _dragging = true;
            Canvas.SetZIndex(el, Math.Max(Canvas.GetZIndex(el), 1000));
            el.Scale = new Vector3(1.06f, 1.06f, 1);
            if (_boardScroll is not { } sv) return;
            edge = DispatcherQueue.CreateTimer();
            edge.Interval = TimeSpan.FromMilliseconds(16);
            edge.Tick += (_, _) =>
            {
                var left = Canvas.GetLeft(el) - sv.HorizontalOffset;
                var right = left + el.ActualWidth - sv.ViewportWidth;
                // (faster the nearer the edge)
                var step = left < 70 ? -Math.Clamp((70 - left) / 4, 2, 22) : right > -70 ? Math.Clamp((right + 70) / 4, 2, 22) : 0;
                var to = Math.Clamp(sv.HorizontalOffset + step, 0, sv.ScrollableWidth);
                if (Math.Abs(to - sv.HorizontalOffset) < 0.5) return;
                Canvas.SetLeft(el, Canvas.GetLeft(el) + (to - sv.HorizontalOffset));
                sv.ChangeView(to, null, null, true);
                moving?.Invoke();
            };
            _timers.Add(edge);
            edge.Start();
        };
        el.ManipulationDelta += (_, e) =>
        {
            var w = _boardCanvas?.Width ?? 2000;
            // (kept whole on the board)
            Canvas.SetLeft(el, Math.Clamp(Canvas.GetLeft(el) + e.Delta.Translation.X, 8, Math.Max(8, w - el.ActualWidth - 8)));
            Canvas.SetTop(el, Math.Clamp(Canvas.GetTop(el) + e.Delta.Translation.Y, 8, Math.Max(8, BoardHeight - el.ActualHeight - 8)));
            moving?.Invoke();
        };
        el.ManipulationCompleted += (_, _) =>
        {
            _dragging = false;
            el.Scale = Vector3.One;
            if (edge is not null) { edge.Stop(); _timers.Remove(edge); edge = null; }
            // (a press that barely moved is a click: the card goes back where it was and opens)
            if (Moved(el) < 8)
            {
                Canvas.SetLeft(el, _dragFrom.X);
                Canvas.SetTop(el, _dragFrom.Y);
                MarkUnder(null);
                if (tapped is not null) { tapped(); return; }
            }
            dropped();
        };
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
    // the timeline laid again along another wave: pictures the player moved go back to it, piles too; Undo puts them back
    void ShuffleBoard()
    {
        if (_boardCanvas is null || _gal is null) return;
        var look = Gallery.Look(_gal, _galAlbum);
        look.Seed = Random.Shared.Next(1, 10000);
        look.Places.Clear();
        look.Gaps.Clear();
        look.Lead = look.SlotLead = 0;
        Keep();
        FlipBoard();
        ShowUndo(T("Board rearranged"), () => StepBack(true));
    }

    // The gallery's history: each save remembers how things were before it (saves within a moment of each other count as
    // one step, so dragging a slider is one), and Ctrl+Z / Ctrl+Y walk back and forth through it.
    readonly Stack<string> _undoSteps = new();
    readonly Stack<string> _redoSteps = new();
    string? _lastKept;
    long _lastKeptAt;
    // step: always a step of its own (an album undone, a rearrange), never folded into a change made just before
    void Keep(bool step = false)
    {
        if (_gal is null) return;
        Gallery.Save(_gal); // (first: it drops the empty records, which would otherwise read as a change next time)
        var now = System.Text.Json.JsonSerializer.Serialize(_gal, SetupJson.Default.GalleryData);
        if (_lastKept is not null && now != _lastKept)
        {
            if (step || Environment.TickCount64 - _lastKeptAt > 700 || _undoSteps.Count == 0) _undoSteps.Push(_lastKept);
            _redoSteps.Clear();
            while (_undoSteps.Count > 60) { var keep = _undoSteps.ToArray()[..60]; _undoSteps.Clear(); foreach (var k in keep.Reverse()) _undoSteps.Push(k); }
        }
        if (now != _lastKept) _lastKeptAt = Environment.TickCount64;
        _lastKept = now;
    }

    // Ctrl+Z back, Ctrl+Y and Ctrl+Shift+Z forward, wherever the focus is while the Gallery shows; a text being typed
    // keeps its own Ctrl+Z
    bool _historyKeys;
    void HistoryKeys()
    {
        if (_historyKeys || Content is not UIElement window) return;
        _historyKeys = true;
        void Shortcut(Windows.System.VirtualKey key, Windows.System.VirtualKeyModifiers mods, bool undo)
        {
            var k = new Microsoft.UI.Xaml.Input.KeyboardAccelerator { Key = key, Modifiers = mods };
            k.Invoked += (_, e) =>
            {
                if (ScreenGallery.Visibility != Visibility.Visible || FocusManager.GetFocusedElement(window.XamlRoot) is TextBox) return;
                e.Handled = true;
                StepBack(undo);
            };
            window.KeyboardAccelerators.Add(k);
        }
        Shortcut(Windows.System.VirtualKey.Z, Windows.System.VirtualKeyModifiers.Control, true);
        Shortcut(Windows.System.VirtualKey.Y, Windows.System.VirtualKeyModifiers.Control, false);
        Shortcut(Windows.System.VirtualKey.Z, Windows.System.VirtualKeyModifiers.Control | Windows.System.VirtualKeyModifiers.Shift, false);
        window.KeyboardAcceleratorPlacementMode = KeyboardAcceleratorPlacementMode.Hidden;
    }

    // saved without becoming a step (the board laying itself out again)
    void KeepQuiet()
    {
        if (_gal is null) return;
        Gallery.Save(_gal);
        _lastKept = System.Text.Json.JsonSerializer.Serialize(_gal, SetupJson.Default.GalleryData);
    }

    void StepBack(bool undo)
    {
        var from = undo ? _undoSteps : _redoSteps;
        var to = undo ? _redoSteps : _undoSteps;
        if (_gal is null || from.Count == 0) return;
        to.Push(_lastKept ?? System.Text.Json.JsonSerializer.Serialize(_gal, SetupJson.Default.GalleryData));
        var state = from.Pop();
        if (System.Text.Json.JsonSerializer.Deserialize(state, SetupJson.Default.GalleryData) is not { } back) return;
        back.Book = _galBook; back.LastAlbum = _galAlbum; // (undo takes back board changes, not the view or the album you are in)
        _gal = back;
        _lastKept = state;
        _lastKeptAt = 0;
        Gallery.Save(_gal);
        var look = Gallery.Look(_gal, _galAlbum);
        if (_boardFrame is not null) _boardFrame.Background = BoardBrush(look);
        foreach (var t in _boardTools) InkTool(t, look);
        if (_galSide is not null) _galSide.Content = GalleryAlbums();
        if (_custDrawer is { Opacity: > 0.5 }) FillCustomize();
        if (_galBook) FlipBoard(); else BuildGallery(); // (the Albums view has no board to flip)
        ShowUndo(undo ? T("Undone") : T("Redone"), () => StepBack(!undo), undo ? T("Redo") : T("Undo"));
    }

    // a note at the foot of the board with an Undo (Ctrl+Z too), gone after a few seconds
    Action? _undo;
    int _undoShown;
    void ShowUndo(string what, Action undo, string? action = null)
    {
        if (_boardFrame is not Grid stage) return;
        _undo = undo;
        var mine = ++_undoShown;
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        row.Children.Add(new TextBlock { Text = what, VerticalAlignment = VerticalAlignment.Center, Foreground = new SolidColorBrush(Rgb(0xEC, 0xE7, 0xDA)) });
        var back = new HyperlinkButton { Content = action ?? T("Undo"), Padding = new Thickness(8, 2, 8, 3), FontWeight = FontWeights.SemiBold };
        row.Children.Add(back);
        var note = new Border
        {
            Child = row, Padding = new Thickness(18, 8, 10, 8), CornerRadius = new CornerRadius(20), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(0, 0, 0, 74), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xEE, 0x1E, 0x1F, 0x25)), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x30, 255, 255, 255)),
            BorderThickness = new Thickness(1), Shadow = new ThemeShadow(), Translation = new Vector3(0, 30, 24), Opacity = 0, RequestedTheme = ElementTheme.Dark, // (a dark pill in both themes: its link reads on it)
            TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(260) }, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(200) },
        };
        foreach (var old in stage.Children.OfType<Border>().Where(x => x.Tag as string == "undo").ToList()) stage.Children.Remove(old);
        note.Tag = "undo";
        stage.Children.Add(note);
        DispatcherQueue.TryEnqueue(() => { note.Opacity = 1; note.Translation = new Vector3(0, 0, 24); });
        void Gone() { note.Opacity = 0; note.Translation = new Vector3(0, 30, 24); After(260, () => stage.Children.Remove(note)); if (mine == _undoShown) _undo = null; }
        back.Click += (_, _) => { if (_undo is { } u) { _undo = null; u(); } Gone(); };
        After(8000, () => { if (mine == _undoShown) Gone(); });
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
        Keep();
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
        var d = new ContentDialog { Title = album is null ? T("New album") : T("Change the album"), Content = body, PrimaryButtonText = album is null ? T("Create album") : T("Save"), CloseButtonText = T("Cancel"),
            DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot, IsPrimaryButtonEnabled = name.Text.Trim().Length > 0 };
        name.TextChanged += (_, _) => d.IsPrimaryButtonEnabled = name.Text.Trim().Length > 0;
        if (await ShowDialog(d) != ContentDialogResult.Primary) return null;
        var made = album ?? new CustomAlbum();
        made.Name = name.Text.Trim();
        made.Icon = AlbumIcons[Math.Max(0, grid.SelectedIndex)].Glyph;
        made.Household = house.SelectedItem as string;
        if (album is null) _gal!.Custom.Add(made);
        Keep();
        return made;
    }

    // ---------- the viewer ----------

    // the picture inside a card (an Image, or a border painted with it); the card itself when there is none
    static FrameworkElement PhotoOf(FrameworkElement card)
    {
        var todo = new Queue<DependencyObject>([card]);
        while (todo.Count > 0)
        {
            var e = todo.Dequeue();
            if (e is Image or Border { Background: ImageBrush }) return (FrameworkElement)e;
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(e); i++) todo.Enqueue(VisualTreeHelper.GetChild(e, i));
        }
        return card;
    }
    static Windows.Foundation.Rect Bounds(FrameworkElement e) => e.TransformToVisual(null).TransformBounds(new Windows.Foundation.Rect(0, 0, e.ActualWidth, e.ActualHeight));
    static double CardTurn(FrameworkElement card) => card is Border { Child: FrameworkElement { RenderTransform: CompositeTransform t } } ? t.Rotation : 0;

    // A picture large, with what can be done with it beside. Opened from a card or a tile it grows out of it.
    bool _viewing;
    void OpenViewer(List<Picture> pics, int at, FrameworkElement? from = null)
    {
        // (one viewer at a time: a click can arrive both as a tap and as a press that barely moved)
        if (_viewing || at < 0 || at >= pics.Count || Content.XamlRoot is not { } root) return;
        _viewing = true;
        var removed = false; // a picture went to the Recycle Bin from here
        // (the board is laid again on closing only when something was changed here, so an open album stays open)
        var opened = _gal is null ? null : System.Text.Json.JsonSerializer.Serialize(_gal, SetupJson.Default.GalleryData);
        var culture = LangCulture();
        var popup = new Popup { XamlRoot = root, IsLightDismissEnabled = false };
        var shell = new Grid { Width = root.Size.Width, Height = root.Size.Height, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) }, Opacity = 0 };
        var dim = new Border { Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xEB, 0x05, 0x06, 0x08)) };
        Grid.SetColumnSpan(dim, 2);
        shell.Children.Add(dim);
        shell.ColumnDefinitions.Add(new ColumnDefinition());
        shell.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(372) });
        var stage = new Grid { Padding = new Thickness(56, 48, 56, 48) };
        var zt = new CompositeTransform();
        var image = new Image { Stretch = Stretch.Uniform };
        var sharp = new Image { Stretch = Stretch.Uniform, Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(220) } };
        var photo = new Grid { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, RenderTransform = zt, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5) };
        photo.Children.Add(image);
        photo.Children.Add(sharp);
        // (from a scale and an offset back to the picture's own place)
        void Settle(double scale, double dx, double dy, int ms, double turn = 0)
        {
            zt.ScaleX = zt.ScaleY = scale;
            zt.TranslateX = dx;
            zt.TranslateY = dy;
            zt.Rotation = turn;
            var sb = new Storyboard();
            var ease = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 5 };
            Tween(sb, zt, "ScaleX", 1, ms, ease);
            Tween(sb, zt, "ScaleY", 1, ms, ease);
            Tween(sb, zt, "TranslateX", 0, ms, ease);
            Tween(sb, zt, "TranslateY", 0, ms, ease);
            Tween(sb, zt, "Rotation", 0, ms, ease);
            Tween(sb, photo, "Opacity", 1, 160);
            Play(sb);
        }
        var zoomer = new ScrollViewer
        {
            Content = photo, ZoomMode = ZoomMode.Enabled, MinZoomFactor = 1, MaxZoomFactor = 6, HorizontalScrollMode = ScrollMode.Enabled, VerticalScrollMode = ScrollMode.Enabled,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden, VerticalScrollBarVisibility = ScrollBarVisibility.Hidden,
        };
        // (the picture fits the room at 1x; zoomed, it is scrolled within it)
        zoomer.SizeChanged += (_, e) => { photo.MaxWidth = e.NewSize.Width; photo.MaxHeight = e.NewSize.Height; photo.MinHeight = e.NewSize.Height; photo.MinWidth = e.NewSize.Width; };
        image.HorizontalAlignment = sharp.HorizontalAlignment = HorizontalAlignment.Center;
        image.VerticalAlignment = sharp.VerticalAlignment = VerticalAlignment.Center;
        void ZoomTo(float factor, Windows.Foundation.Point? at = null)
        {
            factor = Math.Clamp(factor, 1, 6);
            var c = at ?? new Windows.Foundation.Point(zoomer.ViewportWidth / 2, zoomer.ViewportHeight / 2);
            var old = zoomer.ZoomFactor;
            var x = (zoomer.HorizontalOffset + c.X) / old * factor - c.X;
            var y = (zoomer.VerticalOffset + c.Y) / old * factor - c.Y;
            zoomer.ChangeView(Math.Max(0, x), Math.Max(0, y), factor, false);
        }
        zoomer.AddHandler(UIElement.PointerWheelChangedEvent, new PointerEventHandler((_, e) =>
        {
            var pt = e.GetCurrentPoint(zoomer);
            ZoomTo(zoomer.ZoomFactor * (pt.Properties.MouseWheelDelta > 0 ? 1.2f : 1 / 1.2f), pt.Position);
            e.Handled = true;
        }), true);
        photo.Tapped += (_, e) => e.Handled = true;
        zoomer.Background = stage.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        photo.DoubleTapped += (_, e) => { e.Handled = true; ZoomTo(zoomer.ZoomFactor > 1.05f ? 1 : 2, e.GetPosition(zoomer)); };
        Windows.Foundation.Point? grab = null;
        double gx = 0, gy = 0;
        photo.PointerPressed += (_, e) => { if (zoomer.ZoomFactor <= 1.01f) return; grab = e.GetCurrentPoint(zoomer).Position; gx = zoomer.HorizontalOffset; gy = zoomer.VerticalOffset; photo.CapturePointer(e.Pointer); };
        photo.PointerMoved += (_, e) => { if (grab is not { } g) return; var q = e.GetCurrentPoint(zoomer).Position; zoomer.ChangeView(gx - (q.X - g.X), gy - (q.Y - g.Y), null, true); };
        photo.PointerReleased += (_, e) => { grab = null; photo.ReleasePointerCapture(e.Pointer); };
        stage.Children.Add(zoomer);
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
        var island = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, Padding = new Thickness(6), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 0, -30) };
        var islandFrame = new Border
        {
            Child = island, CornerRadius = new CornerRadius(26), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 0, -30),
            Background = new AcrylicBrush { TintColor = Rgb(0x16, 0x17, 0x1B), TintOpacity = 0.7, TintLuminosityOpacity = 0.85, FallbackColor = Rgb(0x1E, 0x1E, 0x1E) },
            BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x24, 255, 255, 255)), BorderThickness = new Thickness(1), Shadow = new ThemeShadow(), Translation = new Vector3(0, 0, 32),
        };
        island.Margin = new Thickness(0);
        Button Tool(string glyph, string name)
        {
            var b = new Button { Content = new FontIcon { Glyph = glyph, FontSize = 15 }, Width = 40, Height = 40, Padding = new Thickness(0), CornerRadius = new CornerRadius(20), BorderThickness = new Thickness(0),
                Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            ToolTipService.SetToolTip(b, name);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
            island.Children.Add(b);
            return b;
        }
        var zoomOut = Tool("\uE71F", T("Zoom out"));
        var zoomLabel = new Button { Content = "100%", MinWidth = 64, Height = 40, CornerRadius = new CornerRadius(20), BorderThickness = new Thickness(0), FontSize = 13, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        ToolTipService.SetToolTip(zoomLabel, T("Fit to the window"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(zoomLabel, T("Fit to the window"));
        island.Children.Add(zoomLabel);
        var zoomIn = Tool("\uE8A3", T("Zoom in"));
        island.Children.Add(new Border { Width = 1, Height = 22, Margin = new Thickness(6, 0, 6, 0), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x30, 255, 255, 255)), VerticalAlignment = VerticalAlignment.Center });
        var play = Tool("\uE768", T("Slideshow"));
        var full = Tool("\uE740", T("Full screen"));
        var photos = Tool("\uE8A7", T("Open in Photos"));
        zoomOut.Click += (_, _) => ZoomTo(zoomer.ZoomFactor / 1.4f);
        zoomIn.Click += (_, _) => ZoomTo(zoomer.ZoomFactor * 1.4f);
        zoomLabel.Click += (_, _) => ZoomTo(1);
        zoomer.ViewChanged += (_, _) => zoomLabel.Content = $"{Math.Round(zoomer.ZoomFactor * 100)}%";
        stage.Children.Add(islandFrame);
        var close = Round("", T("Close"), 40);
        // (the controls over the dark room keep the dark look in a light theme)
        foreach (var c in new FrameworkElement[] { prev, next, close, islandFrame }) c.RequestedTheme = ElementTheme.Dark;
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
        var flight = new Canvas { IsHitTestVisible = false };
        Grid.SetColumnSpan(flight, 2);
        shell.Children.Add(flight);
        popup.Child = shell;

        // (back on the scrapbook at the same stretch of the timeline, nothing falling in again; another album opens at its start)
        FrameworkElement? shown = null;
        var first = pics[at].Path;
        var closing = false;
        Microsoft.UI.Dispatching.DispatcherQueueTimer? show = null;
        var wide = false;
        // a video that was playing stops (its file let go) when another is shown or the viewer closes
        void StopVideo()
        {
            foreach (var v in photo.Children.OfType<MediaPlayerElement>().ToList())
            {
                v.MediaPlayer?.Pause();
                (v.Source as IDisposable)?.Dispose();
                v.Source = null;
            }
        }
        void Close(bool stay = true)
        {
            if (closing) return;
            closing = true;
            StopVideo();
            root.Changed -= Resized;
            show?.Stop();
            if (wide) Wide(false);
            zoomer.ChangeView(0, 0, 1, true);
            var away = new Storyboard();
            var home = pics[at].Path == first ? from
                : (from?.Parent as Panel)?.Children.OfType<FrameworkElement>().FirstOrDefault(c => CardPaths.TryGetValue(c, out var cp) && string.Equals(cp, pics[at].Path, StringComparison.OrdinalIgnoreCase) && c.Visibility == Visibility.Visible);
            if (home != from && from is not null) from.Opacity = 1;
            if (home is not null && home != from) home.Opacity = 0;
            var back = home is { ActualWidth: > 0 } && shown is { ActualWidth: > 0 } ? Bounds(home) : (Windows.Foundation.Rect?)null;
            panel.Translation = new Vector3(372, 0, 0);
            islandFrame.Translation = new Vector3(0, 40, 32);
            if (back is { } b)
            {
                // (the picture shrinks back into the card it came from; the rest of the viewer fades around it)
                Fly(pics[at], Bounds(shown!), b, CardTurn(home!), false, 380, null);
                photo.Opacity = 0;
                Tween(away, flight, "Opacity", 1, 400);
                Tween(away, panel, "Opacity", 0, 260);
                foreach (var c in new UIElement[] { prev, next, close, islandFrame }) Tween(away, c, "Opacity", 0, 140);
                Tween(away, dim, "Opacity", 0, 340);
            }
            else
            {
                shell.Opacity = 0;
                Tween(away, zt, "ScaleX", 0.96, 220);
                Tween(away, zt, "ScaleY", 0.96, 220);
            }
            away.Completed += (_, _) =>
            {
                if (from is not null) from.Opacity = 1;
                if (home is not null) home.Opacity = 1;
                popup.IsOpen = false;
                _viewing = false;
                // (leaving for an album, or a picture gone, always lays the gallery again)
                if (stay && !removed && _gal is not null && opened == System.Text.Json.JsonSerializer.Serialize(_gal, SetupJson.Default.GalleryData)) return;
                // (an album open over the board stays open: the board is laid again when it closes; with a picture gone it
                // closes, for its card would still be there)
                if (stay && !removed && _spotlightOpen) _boardStale = true;
                else if (stay && _galBook) RefreshBoard(page: true); else BuildGallery();
            };
            Play(away);
        }
        close.Click += (_, _) => Close();
        stage.Tapped += (_, e) => { if (!e.Handled) Close(); };
        foreach (var c in new UIElement[] { islandFrame, prev, next, close }) c.Tapped += (_, e) => e.Handled = true;
        void Resized(XamlRoot sender, XamlRootChangedEventArgs e) { shell.Width = root.Size.Width; shell.Height = root.Size.Height; }
        root.Changed += Resized;

        // the card for a picture, as large as the room allows
        FrameworkElement CardFor(Picture pic)
        {
                var light = new SolidColorBrush(Rgb(0xEC, 0xE7, 0xDA));
                var keepScale = _boardScale;
                var keepCalm = _boardCalm;
                FrameworkElement card;
                var look = Gallery.Look(_gal!, _galAlbum);
                try
                {
                    // (measured once at the board's scale, then made again at the scale that fills the room)
                    _boardCalm = true;
                    _boardScale = 1;
                    var probe = Framed(pic, new Placed(), look, light, 1, 0, pics, null, still: true, read: 64, shut: true);
                    probe.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
                    var nat = probe.DesiredSize.Width > 1 ? probe.DesiredSize : new Windows.Foundation.Size(260, 230);
                    var room = new Windows.Foundation.Size(Math.Max(200, zoomer.ActualWidth > 0 ? zoomer.ActualWidth : root.Size.Width - 480), Math.Max(200, zoomer.ActualHeight > 0 ? zoomer.ActualHeight : root.Size.Height - 100));
                    _boardScale = Math.Min(room.Width * 0.94 / nat.Width, room.Height * 0.92 / nat.Height);
                    card = Framed(pic, new Placed(), look, light, 1, 0, pics, null, still: true, read: 700, shut: true, captionMax: 28);
                }
                finally { _boardScale = keepScale; _boardCalm = keepCalm; }
            return card;
        }
        // a copy of a picture's card flown between its place in the room (big) and a card's place (small), above all
        void Fly(Picture pic, Windows.Foundation.Rect big, Windows.Foundation.Rect small, double turn, bool open, int ms, Action? done, BitmapImage? sharp = null)
        {
            var copy = CardFor(pic);
            // (its photo is found once it is in the window: before that it has no visual children to look in)
            if (sharp is { PixelWidth: > 0 })
                copy.Loaded += (_, _) =>
                {
                    switch (PhotoOf(copy))
                    {
                        case Image i: i.Source = sharp; break;
                        case Border { Background: ImageBrush ib }: ib.ImageSource = sharp; break;
                    }
                };
            // (the copy carries no empty caption: that only shows under the pointer)
            void Hush(DependencyObject d)
            {
                for (int i = 0; i < VisualTreeHelper.GetChildrenCount(d); i++)
                {
                    var c = VisualTreeHelper.GetChild(d, i);
                    if (c is TextBlock { Text: var tx } tb && tx == T("+ write something")) ((tb.Parent as UIElement) ?? tb).Visibility = Visibility.Collapsed;
                    else Hush(c);
                }
            }
            copy.Loaded += (_, _) => Hush(copy);
            copy.HorizontalAlignment = HorizontalAlignment.Center;
            copy.VerticalAlignment = VerticalAlignment.Center;
            var t = new CompositeTransform();
            var wing = new Grid { Width = big.Width, Height = big.Height, RenderTransform = t, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5) };
            wing.Children.Add(copy);
            Canvas.SetLeft(wing, big.X);
            Canvas.SetTop(wing, big.Y);
            flight.Children.Add(wing);
            var k = small.Width / Math.Max(1, big.Width);
            var dx = small.X + small.Width / 2 - (big.X + big.Width / 2);
            var dy = small.Y + small.Height / 2 - (big.Y + big.Height / 2);
            if (open) { t.ScaleX = t.ScaleY = k; t.TranslateX = dx; t.TranslateY = dy; t.Rotation = turn; }
            EasingFunctionBase ease = open ? new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 5 } : new ExponentialEase { EasingMode = EasingMode.EaseInOut, Exponent = 4 };
            var sb = new Storyboard();
            Tween(sb, t, "ScaleX", open ? 1 : k, ms, ease);
            Tween(sb, t, "ScaleY", open ? 1 : k, ms, ease);
            Tween(sb, t, "TranslateX", open ? 0 : dx, ms, ease);
            Tween(sb, t, "TranslateY", open ? 0 : dy, ms, ease);
            Tween(sb, t, "Rotation", open ? 0 : turn, ms, ease);
            sb.Completed += (_, _) => { done?.Invoke(); flight.Children.Remove(wing); };
            Play(sb);
        }
        // (the first picture grows out of what was clicked; the next ones settle in place)
        var grow = from is { ActualWidth: > 0 };
        var dir = 0;
        void Show(string path)
        {
            if (zoomer.ZoomFactor > 1.01f) zoomer.ChangeView(0, 0, 1, true);
            var look = Gallery.Look(_gal!, _galAlbum);
            var pic = pics[at];
            StopVideo();
            // (the zoom island is for pictures: a video has its own controls there)
            islandFrame.Visibility = pic.Video ? Visibility.Collapsed : Visibility.Visible;
            if (pic.Video && wide)
            {
                // (full screen: the video plays at once in the room, with the player's own controls; the zoom stays at rest)
                zt.ScaleX = zt.ScaleY = 1; zt.TranslateX = zt.TranslateY = 0; zt.Rotation = 0;
                var player = new MediaPlayerElement { AreTransportControlsEnabled = true, AutoPlay = true, Stretch = Stretch.Uniform,
                    Source = Windows.Media.Core.MediaSource.CreateFromUri(new Uri(path)), HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch };
                player.TransportControls.IsZoomButtonVisible = false;
                photo.Children.Clear();
                photo.Children.Add(player);
                shown = player;
                photo.Opacity = 1;
                if (grow && from is not null) from.Opacity = 0;
                grow = false;
                return;
            }
            if (wide)
            {
                // (full screen: the picture alone, as large as the screen, the cached copy first and the sharp one over it)
                var alone = new Grid();
                var quick = new Image { Source = Thumb(path, 900), Stretch = Stretch.Uniform };
                var crispSource = Thumb(path, root.Size.Width);
                var crisp = new Image { Source = crispSource, Stretch = Stretch.Uniform, Opacity = crispSource.PixelWidth > 0 ? 1 : 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(200) } };
                if (crispSource.PixelWidth == 0) crisp.ImageOpened += (_, _) => crisp.Opacity = 1;
                alone.Children.Add(quick);
                alone.Children.Add(crisp);
                var before = shown;
                photo.Children.Add(alone);
                shown = alone;
                alone.Opacity = 0;
                var fade = new Storyboard();
                Tween(fade, alone, "Opacity", 1, 220);
                if (before is not null) { Tween(fade, before, "Opacity", 0, 200); fade.Completed += (_, _) => photo.Children.Remove(before); }
                Play(fade);
                photo.Opacity = 1;
                grow = false;
                return;
            }
            var card = CardFor(pic);
            // a video opens as its card does, then the player takes the card's place where it landed
            void PlayIn()
            {
                if (shown != card || closing) return;
                var player = new MediaPlayerElement { AreTransportControlsEnabled = true, AutoPlay = true, Stretch = Stretch.Uniform, Opacity = 0,
                    OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(260) },
                    Source = Windows.Media.Core.MediaSource.CreateFromUri(new Uri(path)), HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch };
                player.TransportControls.IsZoomButtonVisible = false;
                photo.Children.Add(player);
                shown = player;
                DispatcherQueue.TryEnqueue(() => player.Opacity = 1);
                After(320, () => photo.Children.Remove(card));
            }
            card.HorizontalAlignment = HorizontalAlignment.Center;
            card.VerticalAlignment = VerticalAlignment.Center;
            var old = shown;
            if (grow || old is null) photo.Children.Clear();
            photo.Children.Add(card);
            shown = card;
            // (once in place the cached copy gives way to a sharp one, read off screen first so nothing blinks)
            // (the picture itself, read at the size it is shown: a cached copy may not be made yet, and then the view opened
            // on the small one; a video keeps its cached frame)
            var sharpCopy = pic.Video ? Thumb(path, root.Size.Width)
                : new BitmapImage { DecodePixelType = DecodePixelType.Logical, DecodePixelWidth = (int)Math.Max(640, root.Size.Width), UriSource = new Uri(path) };
            var loader = new Image { Source = sharpCopy, Width = 1, Height = 1, Opacity = 0.01, IsHitTestVisible = false, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
            void Sharpen()
            {
                stage.Children.Remove(loader);
                if (shown != card) return;
                void Apply()
                {
                    switch (PhotoOf(card))
                    {
                        case Image i: i.Source = sharpCopy; break;
                        case Border { Background: ImageBrush ib }: ib.ImageSource = sharpCopy; break;
                    }
                }
                // (a card not in the window yet has no photo to find: it takes the sharp one as it arrives)
                if (card.IsLoaded) Apply(); else card.Loaded += (_, _) => Apply();
            }
            if (sharpCopy.PixelWidth > 0) Sharpen();
            else { stage.Children.Add(loader); loader.ImageOpened += (_, _) => Sharpen(); }
            if (!grow)
            {
                // (the next picture waits until it is read, then slides in over the last one, which slides out the other way)
                card.Opacity = 0;
                var swapped = false;
                void Swap()
                {
                    if (swapped) return;
                    swapped = true;
                    var sb = new Storyboard();
                    var ease = new ExponentialEase { EasingMode = EasingMode.EaseOut, Exponent = 4 };
                    if (card is Border { Child: FrameworkElement { RenderTransform: CompositeTransform nt } }) { nt.TranslateX = dir * 120; Tween(sb, nt, "TranslateX", 0, 440, ease); }
                    Tween(sb, card, "Opacity", 1, 260);
                    if (old is not null && old != card)
                    {
                        if (old is Border { Child: FrameworkElement { RenderTransform: CompositeTransform ot } }) Tween(sb, ot, "TranslateX", -dir * 120, 440, ease);
                        Tween(sb, old, "Opacity", 0, 220);
                        sb.Completed += (_, _) => photo.Children.Remove(old);
                    }
                    if (pic.Video) sb.Completed += (_, _) => PlayIn();
                    Play(sb);
                }
                DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
                {
                    switch (PhotoOf(card))
                    {
                        case Image { Source: BitmapImage bi } when bi.PixelWidth > 0: Swap(); break;
                        case Image { Source: BitmapImage bi }: bi.ImageOpened += (_, _) => Swap(); break;
                        case Border { Background: ImageBrush { ImageSource: BitmapImage bb } } when bb.PixelWidth > 0: Swap(); break;
                        case Border { Background: ImageBrush ib }: ib.ImageOpened += (_, _) => Swap(); break;
                    }
                    After(350, Swap);
                });
                return;
            }
            photo.Opacity = 0;
            grow = false;
            var src = Bounds(from!);
            var turn = CardTurn(from!);
            void Rise()
            {
                card.UpdateLayout();
                if (card.ActualWidth <= 0) { photo.Opacity = 1; return; }
                var dst = Bounds(card);
                from!.Opacity = 0;
                photo.Opacity = 0;
                Fly(pic, dst, src, turn, true, 600, () => { photo.Opacity = 1; if (pic.Video) PlayIn(); }, sharpCopy);
            }
            void Start()
            {
                if (card.ActualWidth > 0) Rise(); else { void Once(object o, SizeChangedEventArgs e) { card.SizeChanged -= Once; Rise(); } card.SizeChanged += Once; }
            }
            // (the sharp picture is read before the card grows, so it never opens on the small copy and sharpens later;
            // a slow read waits at most 0.8 s, then the card grows and sharpens when ready)
            if (sharpCopy.PixelWidth > 0 || pic.Video) Start();
            else
            {
                var started = false;
                void Go() { if (started || closing) return; started = true; Start(); }
                loader.ImageOpened += (_, _) => Go();
                loader.ImageFailed += (_, _) => Go();
                After(800, Go);
            }
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
                Background = on ? Res("ApexAccentSoftBrush") : new SolidColorBrush(Pick(0x0FFFFFFF, 0x0C000000)),
                BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Pick(0x14FFFFFF, 0x1A000000)), BorderThickness = new Thickness(1) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
            return b;
        }

        void Fill()
        {
            var p = pics[at];
            // (read only: a record is made when something is set, and fetched then, for a save drops the empty ones)
            var info = Gallery.Peek(_gal!, p.Path);
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
            chips.Children.Add(new Border { CornerRadius = new CornerRadius(12), Padding = new Thickness(10, 3, 10, 4), Background = new SolidColorBrush(Pick(0x14FFFFFF, 0x10000000)),
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
                item.Click += (_, _) => { Gallery.Info(_gal!, p.Path).Household = chosen == guess ? null : chosen; Keep(); Fill(); };
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
            cap.LostFocus += (_, _) => { Gallery.Info(_gal!, p.Path).Caption = cap.Text.Trim(); Keep(); };
            side.Children.Add(cap);

            // what to do with it, as tiles
            var grid = new Grid { ColumnSpacing = 10, RowSpacing = 10 };
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.RowDefinitions.Add(new RowDefinition());
            grid.RowDefinitions.Add(new RowDefinition());
            var fav = Action(info.Favourite ? "" : "", info.Favourite ? T("In Favourites") : T("Favourite"), info.Favourite);
            fav.Click += (_, _) => { var i = Gallery.Info(_gal!, p.Path); i.Favourite = !i.Favourite; Keep(); Fill(); };
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
            home.Click += (_, _) => { _gal.HomePicture = p.Path; Keep(); Fill(); };
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
                    row.Children.Add(new FontIcon { Glyph = c.Icon, FontSize = 14, Foreground = new SolidColorBrush(AccentInk) });
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
                try { Gallery.Remove(_gal!, p.Path); removed = true; pics.RemoveAt(at); if (pics.Count == 0) { Close(); return; } at = Math.Min(at, pics.Count - 1); Fill(); }
                catch (Exception e) { App.Log(e); await Message(T("Couldn't move the picture"), ErrorText(e)); }
            };
            Grid.SetColumn(bin, 1);
            foot.Children.Add(bin);
        }
        // (a slideshow moves on every four seconds until stopped or the viewer closes)
        void Slides(bool on)
        {
            show?.Stop();
            show = null;
            ((FontIcon)play.Content).Glyph = on ? "\uE769" : "\uE768";
            ToolTipService.SetToolTip(play, on ? T("Stop the slideshow") : T("Slideshow"));
            if (!on) return;
            show = DispatcherQueue.CreateTimer();
            show.Interval = TimeSpan.FromSeconds(4);
            show.Tick += (_, _) => { if (closing) { show?.Stop(); return; } dir = 1; at = (at + 1) % pics.Count; ZoomTo(1); Fill(); };
            show.Start();
        }
        play.Click += (_, _) => Slides(show is null);
        // (full screen: the window fills the screen and the picture the window, the side panel set aside)
        void Wide(bool on)
        {
            wide = on;
            shell.ColumnDefinitions[1].Width = new GridLength(on ? 0 : 372);
            panel.Visibility = on ? Visibility.Collapsed : Visibility.Visible;
            stage.Padding = on ? new Thickness(0) : new Thickness(56, 48, 56, 48);
            dim.Background = new SolidColorBrush(on ? Microsoft.UI.Colors.Black : Windows.UI.Color.FromArgb(0xEB, 0x05, 0x06, 0x08));
            close.Margin = on ? new Thickness(0, 64, 24, 0) : new Thickness(0, -32, -40, 0);
            prev.Margin = on ? new Thickness(16, 0, 0, 0) : new Thickness(-36, 0, 0, 0);
            next.Margin = on ? new Thickness(0, 0, 16, 0) : new Thickness(0, 0, -36, 0);
            islandFrame.Margin = on ? new Thickness(0, 0, 0, 24) : new Thickness(0, 0, 0, -30);
            Refit();
            ((FontIcon)full.Content).Glyph = on ? "\uE73F" : "\uE740";
            ToolTipService.SetToolTip(full, on ? T("Leave full screen") : T("Full screen"));
        }
        full.Click += (_, _) => Wide(!wide);
        void Refit() { DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () => { if (closing) return; grow = false; dir = 0; Show(pics[at].Path); }); }
        var fitting = 0;
        zoomer.SizeChanged += (_, e) =>
        {
            if (e.PreviousSize.Width <= 0 || Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) < 24) return;
            var mine = ++fitting;
            After(180, () => { if (mine == fitting) Refit(); });
        };
        photos.Click += (_, _) => { try { Process.Start(new ProcessStartInfo(pics[at].Path) { UseShellExecute = true }); } catch (Exception e) { App.Log(e); } };
        prev.Click += (_, _) => { dir = -1; at = (at - 1 + pics.Count) % pics.Count; Fill(); };
        next.Click += (_, _) => { dir = 1; at = (at + 1) % pics.Count; Fill(); };
        shell.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Escape) { if (wide) Wide(false); else Close(); }
            else if (e.Key == Windows.System.VirtualKey.Left) { dir = -1; at = (at - 1 + pics.Count) % pics.Count; Fill(); }
            else if (e.Key == Windows.System.VirtualKey.Right) { dir = 1; at = (at + 1) % pics.Count; Fill(); }
        };
        Fill();
        panel.Translation = new Vector3(372, 0, 0);
        panel.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(420) };
        islandFrame.Translation = new Vector3(0, 40, 32);
        islandFrame.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(420) };
        popup.IsOpen = true;
        DispatcherQueue.TryEnqueue(() =>
        {
            shell.Opacity = 1;
            panel.Translation = Vector3.Zero;
            islandFrame.Translation = new Vector3(0, 0, 32);
            next.Focus(FocusState.Programmatic);
        });
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
