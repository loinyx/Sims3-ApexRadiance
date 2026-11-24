using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Home, like a game's own page: the picture as a wide rounded card, the game's name low in its corner with Play (and a "more"
// menu for the rarer choices), health as a small glass note in the other corner, and rows of the newest saves, each with its
// own Play, and the newest pictures under it. Play on top opens the game's main menu; a save starts only from its own button. The picture is the player's choice in Settings: a gallery picture,
// the household's own photo, or none.
public sealed partial class MainWindow
{
    DispatcherTimer? _homeTick; // the playing time, ticking while the game runs

    void BuildHome()
    {
        WarmThumbs();
        var host = ScreenHome;
        host.Children.Clear();
        host.Spacing = 0;
        _homeTick?.Stop();
        if (_game is null)
        {
            host.Spacing = 12;
            host.Children.Add(Heading(T("Home")));
            host.Children.Add(Card(Body(T("The launcher can't find The Sims 3. Choose the game folder in Settings.")), Button(T("Choose the game"), () => Show(GameStep))));
            return;
        }
        // (the picture runs to the window's edges; every other screen gets the page margins back in Show)
        Scroller.Padding = new Thickness(0);
        Screens.Margin = new Thickness(0, 0, 0, 32);
        var saves = Saves.List(Docs);
        host.Children.Add(HomeTop(saves.FirstOrDefault()));
        var below = new StackPanel { Spacing = 28, Padding = new Thickness(40, 28, 40, 0) };
        if (_healthState == HealthState.Blocked)
            below.Children.Add(Banner(InfoBarSeverity.Error, T("Apex Radiance can't start"), T("The game still starts, but without Apex Radiance until this is fixed."), T("Open Health"), () => Show(HealthScreen)));
        if (_offline) below.Children.Add(Banner(InfoBarSeverity.Warning, T("No internet"), T("You can still play. Updates wait until you're back online."), T("Try again"), async () => await LoadReleasesAsync()));
        // the last session shows only when it went wrong: straight back into the newest save
        if (!_running && SessionHistory.List().FirstOrDefault() is { End: SessionEnd.Crash or SessionEnd.LowMemory } last && Saves.Newest(Docs) is { } newest)
            below.Children.Add(Banner(InfoBarSeverity.Warning, last.End == SessionEnd.Crash ? T("The game closed on its own") : T("Ran out of memory"),
                Played(last.Played) + " · " + T("{0} at its peak", ModRow.SizeText(last.PeakBytes)), T("Open this save again"), async () => await PlayAsync(newest.Name)));
        if (saves.Count > 0) below.Children.Add(HomeSaves(saves.Take(4).ToList()));
        below.Children.Add(HomePictures());
        host.Children.Add(below);
        if (!_homeFitHooked) { _homeFitHooked = true; Scroller.SizeChanged += (_, _) => { if (_step == Home) FitHome(); }; }
        FitHome();
    }

    // a row's head on Home: its title and a quiet link on the right
    static UIElement Head(string title, string? link, Action? go)
    {
        var g = new Grid();
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new TextBlock { Text = title, Style = (Style)Application.Current.Resources["ApexSection"], VerticalAlignment = VerticalAlignment.Center });
        if (link is not null && go is not null)
        {
            var l = Link(link, go);
            l.Padding = new Thickness(0);
            l.Foreground = Res("ApexSoftTextBrush");
            l.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(l, 1);
            g.Children.Add(l);
        }
        return g;
    }

    // when a save was made: in words while it is recent, else its day
    string SavedWhen(DateTime t)
    {
        var c = LangCulture();
        var days = (DateTime.Today - t.Date).Days;
        return days <= 0 ? T("Saved today at {0}", t.ToString("HH:mm", c))
            : days == 1 ? T("Saved yesterday at {0}", t.ToString("HH:mm", c))
            : t.ToString("d MMM", c);
    }

    // how long a session was, in words
    string Played(TimeSpan t) => t.TotalHours >= 1 ? T("{0} h {1} min played", (int)t.TotalHours, t.Minutes) : T("{0} min played", Math.Max(1, (int)t.TotalMinutes));

    static Windows.UI.Color A(Windows.UI.Color c, byte a) => Windows.UI.Color.FromArgb(a, c.R, c.G, c.B);
    static LinearGradientBrush Fade(Windows.UI.Color c, (double At, byte A)[] stops, bool across = false)
    {
        var b = new LinearGradientBrush { StartPoint = across ? new(0, 0) : new(0, 1), EndPoint = across ? new(1, 0) : new(0, 0) };
        foreach (var (at, a) in stops) b.GradientStops.Add(new GradientStop { Color = A(c, a), Offset = at });
        return b;
    }

    // ---------- the top: the picture, the game and Play ----------

    // newest: the save whose household stands on the plain card
    UIElement HomeTop(Save? newest)
    {
        var mode = _settings.HomePicture;
        var shot = mode == "gallery" ? HomePicture() : null;
        var onPhoto = _homeOnPhoto = shot is not null;
        var g = _homeHero = new Grid { Height = onPhoto ? 520 : double.NaN };
        // on the picture the words are white on every theme; on the plain card they take the theme's own colours
        Brush text = onPhoto ? new SolidColorBrush(Microsoft.UI.Colors.White) : Res("TextFillColorPrimaryBrush");
        Brush soft = onPhoto ? new SolidColorBrush(Windows.UI.Color.FromArgb(0xE6, 255, 255, 255)) : Res("ApexSoftTextBrush");

        var pic = new Grid();
        if (shot is not null)
        {
            // the picture as a rounded card inside the page's margins, at full strength unless Settings softens it, with a soft
            // shade in the corner where the words sit so they read on any picture without a box over it
            pic.Children.Add(new Image { Source = Thumb(shot, 2000), Stretch = Stretch.UniformToFill, VerticalAlignment = VerticalAlignment.Center, Opacity = Math.Clamp(_settings.HomePictureStrength, 30, 100) / 100.0 });
            var black = Windows.UI.Color.FromArgb(255, 8, 8, 8);
            pic.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(black, [(0, 0xC8), (0.3, 0x66), (0.6, 0)]) });
            pic.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(black, [(0, 0x8C), (0.35, 0x26), (0.55, 0)], across: true) });
        }
        else
        {
            // no picture: a plain card, lit softly from the right by the accent, with the household standing there when chosen
            var accent = ((SolidColorBrush)Res("ApexAccentBrush")).Color;
            pic.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(accent, [(0, 0), (0.55, 0), (1, 0x1C)], across: true) });
            if (mode == "household" && newest is not null)
            {
                // (low on the right, under the health note; the game stores these photos small, so they stay small)
                var photo = new Grid { HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Height = 170, Margin = new Thickness(0, 0, 96, 12) };
                pic.Children.Add(photo);
                _ = Task.Run(() => Saves.Photo(newest)).ContinueWith(async t =>
                {
                    if (!t.IsFaulted && await SaveCard.SimsOf(t.Result) is { } img) photo.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Bottom });
                }, TaskScheduler.FromCurrentSynchronizationContext());
            }
        }
        g.Children.Add(new Border { Child = pic, CornerRadius = new CornerRadius(12), Margin = new Thickness(40, 40, 40, 0), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexCardBrush") });

        // the game, low in the corner of the picture or centred on the plain card
        var words = new StackPanel { Spacing = 6, HorizontalAlignment = HorizontalAlignment.Left };
        var kick = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        if (_state?.ApexVersion is { Length: > 0 } apex)
            kick.Children.Add(new Border { Background = onPhoto ? new SolidColorBrush(Windows.UI.Color.FromArgb(0x26, 255, 255, 255)) : Res("ApexPillBrush"), CornerRadius = new CornerRadius(11), Padding = new Thickness(9, 3, 9, 3),
                Child = new TextBlock { Text = "Apex Radiance " + apex, FontSize = 12, Foreground = onPhoto ? text : soft } });
        words.Children.Add(kick);
        _homeWords = words;
        words.Children.Add(_homeName = new TextBlock
        {
            Text = "The Sims 3", FontFamily = new FontFamily("Segoe UI Variable Display"), FontSize = 48, FontWeight = FontWeights.SemiBold,
            TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 4, 0, 0), Foreground = text,
        });
        words.Children.Add(new TextBlock { Text = _game is { } game ? game.Label + (game.Version.Length > 0 ? " \u00b7 " + game.Version : "") : "", FontSize = 14, Foreground = soft });
        var acts = HomeActions();
        acts.Margin = new Thickness(0, 16, 0, 0);
        words.Children.Add(acts);
        words.VerticalAlignment = onPhoto ? VerticalAlignment.Bottom : VerticalAlignment.Center;
        words.Margin = onPhoto ? new Thickness(72, 0, 40, 44) : new Thickness(72, 80, 40, 40);
        g.Children.Add(words);

        var chip = HealthChip();
        chip.HorizontalAlignment = HorizontalAlignment.Right;
        chip.VerticalAlignment = VerticalAlignment.Top;
        chip.Margin = new Thickness(0, 56, 56, 0);
        g.Children.Add(chip);
        return g;
    }

    Grid? _homeHero;
    StackPanel? _homeWords;
    TextBlock? _homeName;
    bool _homeFitHooked, _homeOnPhoto;

    // the top keeps the picture's own wide shape as the window grows, within the share of the window Settings' picture
    // height allows (a plain card takes the height of its words); the name grows a little with it
    void FitHome()
    {
        double w = Scroller.ActualWidth, h = Scroller.ActualHeight;
        if (_homeHero is null || w <= 0 || h <= 0) return;
        var share = Math.Clamp(_settings.HomePictureHeight / 480.0 * 0.9, 0.42, 0.88);
        if (_homeOnPhoto) _homeHero.Height = Math.Clamp(w * 0.44, 360, Math.Max(360, h * share));
        if (_homeName is not null) { _homeName.FontSize = Math.Clamp(w / 32, 40, 64); _homeName.LineHeight = _homeName.FontSize * 1.08; }
        if (_homeWords is not null) _homeWords.MaxWidth = Math.Max(560, w * 0.42);
    }

    // Play opens the game's main menu; the "more" menu holds playing without the updates and all the saves
    StackPanel HomeActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        // (on the picture: white on frosted white; on the plain card: the theme's own fills)
        Brush glass = _homeOnPhoto ? new SolidColorBrush(Windows.UI.Color.FromArgb(0x1F, 255, 255, 255)) : Res("ApexPillBrush");
        Brush ink = _homeOnPhoto ? new SolidColorBrush(Microsoft.UI.Colors.White) : Res("TextFillColorPrimaryBrush");
        Brush inkSoft = _homeOnPhoto ? new SolidColorBrush(Windows.UI.Color.FromArgb(0xCC, 255, 255, 255)) : Res("ApexSoftTextBrush");
        if (_running)
        {
            var playing = new TextBlock { FontWeight = FontWeights.SemiBold, Foreground = ink, VerticalAlignment = VerticalAlignment.Center };
            var pill = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            pill.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center });
            pill.Children.Add(playing);
            row.Children.Add(new Border { Child = pill, Background = glass, CornerRadius = new CornerRadius(8), Padding = new Thickness(18, 0, 20, 0), Height = 56 });
            var mem = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            var bar = new ProgressBar { Width = 160, Maximum = 1, VerticalAlignment = VerticalAlignment.Center };
            var used = new TextBlock { FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Foreground = inkSoft };
            mem.Children.Add(new TextBlock { Text = T("Memory"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Foreground = inkSoft });
            mem.Children.Add(bar);
            mem.Children.Add(used);
            row.Children.Add(mem);
            // the time and the memory the game uses, every second (the memory is the session's own sample, taken every 5 s)
            void Tick()
            {
                playing.Text = _session is { } s ? T("Playing · {0}", Duration(s.Played)) : T("Playing");
                var bytes = _session?.MemoryBytes ?? 0;
                mem.Visibility = bytes > 0 ? Visibility.Visible : Visibility.Collapsed;
                bar.Value = Math.Min(1, bytes / (double)Session.LowMemoryBytes);
                used.Text = T("{0} of {1}", ModRow.SizeText(bytes), ModRow.SizeText(Session.LowMemoryBytes));
            }
            Tick();
            _homeTick ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _homeTick.Tick -= OnHomeTick;
            _homeTick.Tick += OnHomeTick;
            _homeTickAction = Tick;
            _homeTick.Start();
            return row;
        }
        var ups = WelcomeUpdates();
        var update = ups.Count > 0 && _settings.InstallUpdatesOnPlay;
        var blocked = _healthState == HealthState.Blocked;
        var play = new Button { Height = 56, Padding = new Thickness(22, 0, 28, 0), CornerRadius = new CornerRadius(8), IsEnabled = !_busy && !Maintenance };
        if (!blocked) play.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        var pc = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        pc.Children.Add(new FontIcon { Glyph = "", FontSize = 14, VerticalAlignment = VerticalAlignment.Center });
        var label = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        var title = blocked ? T("Play anyway") : T("Play");
        label.Children.Add(new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeights.SemiBold });
        var sub = blocked ? T("The game starts without Apex Radiance") : update ? (ups.Count == 1 ? T("1 update installs first") : T("{0} updates install first", ups.Count)) : null;
        if (sub is not null) label.Children.Add(new TextBlock { Text = sub, FontSize = 12, Opacity = 0.75 });
        pc.Children.Add(label);
        play.Content = pc;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(play, "PlayButton");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(play, title);
        if (update) ToolTipService.SetToolTip(play, string.Join("\n", ups.Select(c => Name(c) + " " + (ReleaseOf(c)?.Version ?? ""))));
        play.Click += async (_, _) => await PlayAsync(null, update: update);
        row.Children.Add(play);

        var menu = new MenuFlyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedLeft };
        if (update && !blocked)
        {
            var asIs = new MenuFlyoutItem { Text = T("Play without updating"), Icon = new FontIcon { Glyph = "" }, IsEnabled = !_busy && !Maintenance };
            asIs.Click += async (_, _) => await PlayAsync(null);
            menu.Items.Add(asIs);
            menu.Items.Add(new MenuFlyoutSeparator());
        }
        var all = new MenuFlyoutItem { Text = T("All saves"), Icon = new FontIcon { Glyph = "" } };
        all.Click += (_, _) => Show(SavesScreen);
        menu.Items.Add(all);
        var more = new Button
        {
            Width = 56, Height = 56, Padding = new Thickness(0), CornerRadius = new CornerRadius(8), Background = glass, BorderThickness = new Thickness(0), Flyout = menu,
            Content = new FontIcon { Glyph = "", FontSize = 16, Foreground = ink },
        };
        ToolTipService.SetToolTip(more, T("More"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(more, T("More"));
        row.Children.Add(more);
        return row;
    }

    Action? _homeTickAction;
    void OnHomeTick(object? sender, object e)
    {
        if (!_running || _step != Home) { _homeTick?.Stop(); return; }
        _homeTickAction?.Invoke();
    }

    // ---------- health: a small note in the picture's corner ----------

    FrameworkElement HealthChip()
    {
        var crystal = new Crystal(24);
        crystal.SetLevel(HealthLevel, animate: false);
        // (only what stops Apex gets its own words here; the rest waits calmly in Health)
        var first = _healthState == HealthState.Blocked ? _health.FirstOrDefault(h => h.Finding.Severity == Severity.Error) : null;
        var note = first is not null ? Sentences(T(first.Finding.Text, first.Finding.Args)).Item1
            : _healthState == HealthState.NeedsYou ? T("Nothing stops the game from starting.") : T("Everything is in place.");
        var white = new SolidColorBrush(Microsoft.UI.Colors.White);
        var g = new Grid { ColumnSpacing = 12, Padding = new Thickness(16, 10, 18, 10), MaxWidth = 360 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        crystal.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(crystal);
        var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = HealthTitle(), FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = _healthState == HealthState.Blocked ? Res("ApexErrorBrush") : white, TextTrimming = TextTrimming.CharacterEllipsis });
        words.Children.Add(new TextBlock { Text = note, FontSize = 12, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(0xBF, 255, 255, 255)), TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(words, 1);
        g.Children.Add(words);
        var chev = Lucide.Icon("chevron-right", 12, new SolidColorBrush(Windows.UI.Color.FromArgb(0xBF, 255, 255, 255)));
        chev.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(chev, 2);
        g.Children.Add(chev);
        var b = new Button
        {
            Content = g, Padding = new Thickness(0), CornerRadius = new CornerRadius(20), BorderThickness = new Thickness(1),
            Background = Glass(0.28), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x2E, 255, 255, 255)),
        };
        // (frosted glass that blurs the picture behind it, a little lighter under the pointer)
        b.Resources["ButtonBackgroundPointerOver"] = Glass(0.16);
        b.Resources["ButtonBackgroundPressed"] = Glass(0.4);
        b.Resources["ButtonBorderBrushPointerOver"] = new SolidColorBrush(Windows.UI.Color.FromArgb(0x47, 255, 255, 255));
        b.Click += (_, _) => Show(HealthScreen);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, T("Open Health"));
        return b;
    }

    static Microsoft.UI.Xaml.Media.AcrylicBrush Glass(double tint) => new()
    {
        TintColor = Windows.UI.Color.FromArgb(255, 20, 20, 20), TintOpacity = tint, TintLuminosityOpacity = 0.25,
        FallbackColor = Windows.UI.Color.FromArgb(0xC8, 20, 20, 20),
    };

    // ---------- the newest saves: a row of tiles, each with its household and its own Play ----------

    UIElement HomeSaves(List<Save> saves)
    {
        var s = new StackPanel { Spacing = 12 };
        s.Children.Add(Head(T("Your saves"), T("All saves"), () => Show(SavesScreen)));
        var grid = new Grid { ColumnSpacing = 16, RowSpacing = 16 };
        for (int i = 0; i < 4; i++) grid.ColumnDefinitions.Add(new ColumnDefinition());
        var docs = Docs;
        var white = new SolidColorBrush(Microsoft.UI.Colors.White);
        var rows = new List<(Save Save, Grid Photo, TextBlock Meta)>();
        var tiles = new List<Grid>();
        for (int i = 0; i < saves.Count; i++)
        {
            var save = saves[i];
            var name = save.Name;
            // a stage for the household: a soft light behind them, a shadow at their feet, the name at the foot
            var tile = new Grid { Background = Fade(((SolidColorBrush)Res("ApexRaisedBrush")).Color, [(0, 0xFF), (1, 0x80)]) };
            var photo = new Grid();
            tile.Children.Add(photo);
            var foot = new StackPanel { VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(16, 0, 72, 14), Spacing = 1 };
            foot.Children.Add(new TextBlock { Text = name, FontSize = 15, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
            var day = save.LastPlayed.Date == DateTime.Today ? T("Today") : save.LastPlayed.ToString("d MMM", LangCulture());
            var meta = Muted(day);
            meta.TextWrapping = TextWrapping.NoWrap;
            meta.TextTrimming = TextTrimming.CharacterEllipsis;
            foot.Children.Add(meta);
            tile.Children.Add(foot);
            // Play in the corner, straight into this save (the updates first, as Play on top)
            var play = new Button
            {
                Height = PlaySize, CornerRadius = new CornerRadius(PlaySize / 2), Padding = new Thickness(0), BorderThickness = new Thickness(0),
                IsEnabled = !_busy && !Maintenance && !_running,
            };
            ToolTipService.SetToolTip(play, T("Play this save"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(play, T("Play {0}", name));
            play.Click += async (_, _) => await PlayAsync(name, update: _settings.InstallUpdatesOnPlay);
            var cell = new Grid();
            cell.Children.Add(new Border { Child = tile, CornerRadius = new CornerRadius(12), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) });
            var (corner, over) = PlayCorner(play);
            cell.Children.Add(corner);
            ToolTipService.SetToolTip(tile, SavedWhen(save.LastPlayed));
            cell.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(200) };
            cell.PointerEntered += (_, _) => { cell.Translation = new(0, -4, 0); over(true); };
            cell.PointerExited += (_, _) => { cell.Translation = default; over(false); };
            Grid.SetColumn(cell, i);
            grid.Children.Add(cell);
            tiles.Add(tile);
            rows.Add((save, photo, meta));
        }
        // tiles keep their shape as the window grows; a narrow window: two a row
        grid.SizeChanged += (_, e) =>
        {
            var two = e.NewSize.Width < 760;
            while (grid.RowDefinitions.Count < 2) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            for (int i = 2; i < 4; i++) grid.ColumnDefinitions[i].Width = two ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
            var cols = two ? 2 : 4;
            var tw = (e.NewSize.Width - 16 * (cols - 1)) / cols;
            foreach (var t in tiles) t.Height = Math.Clamp(tw * 0.6, 200, 250);
            for (int i = 0; i < grid.Children.Count; i++) { var el = (FrameworkElement)grid.Children[i]; Grid.SetColumn(el, two ? i % 2 : i); Grid.SetRow(el, two ? i / 2 : 0); }
        };
        s.Children.Add(grid);
        var backup = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Visibility = Visibility.Collapsed };
        s.Children.Add(backup);

        // the photos, sizes and backups read off the UI thread
        _ = Task.Run(() => (SaveHealth.Check(docs, record: false), saves.Select(Saves.Photo).ToList())).ContinueWith(async t =>
        {
            if (t.IsFaulted) return;
            var (reports, photos) = t.Result;
            var bare = new List<Save>();
            for (int i = 0; i < rows.Count; i++)
            {
                var (save, photo, meta) = rows[i];
                if (reports.FirstOrDefault(r => r.Save.Dir == save.Dir) is { } r)
                {
                    meta.Text += " · " + ModRow.SizeText(r.Bytes);
                    if (r.LastBackup is null) bare.Add(save);
                }
                if (await SaveCard.SimsOf(photos[i]) is { } img) photo.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 16, 0, 58) });
            }
            BackupLine(backup, bare, docs);
        }, TaskScheduler.FromCurrentSynchronizationContext());
        return s;
    }

    const double PlaySize = 44;

    // a save's round Play in its card's corner: an outline in the accent. Under the pointer anywhere on the card, or with the
    // keyboard on it, the accent opens from the middle until it fills the circle and the triangle turns dark; leaving, it
    // closes back. Without a wait when Windows' animation effects are off.
    static (FrameworkElement Corner, Action<bool> Over) PlayCorner(Button play)
    {
        const float size = (float)PlaySize;
        var none = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        foreach (var state in new[] { "", "PointerOver", "Pressed", "Disabled" }) play.Resources["ButtonBackground" + state] = none;
        play.Background = none;
        Microsoft.UI.Xaml.Shapes.Ellipse Disc() => new() { Width = size, Height = size };
        FrameworkElement Tri(Brush fill)
        {
            var path = new Microsoft.UI.Xaml.Shapes.Path
            {
                Data = (Geometry)Microsoft.UI.Xaml.Markup.XamlBindingHelper.ConvertValue(typeof(Geometry), "M3.5 2.2c0-0.9 1-1.5 1.8-1l8 5c0.8 0.5 0.8 1.6 0 2.1l-8 5c-0.8 0.5-1.8-0.1-1.8-1z"),
                Fill = fill, Width = 16, Height = 16,
            };
            return new Canvas { Width = 16, Height = 16, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(13, 14, 0, 0), Children = { path } };
        }
        var (glass, fill, ring) = (Disc(), Disc(), Disc());
        glass.Fill = Res("ApexCardBrush");
        fill.Fill = ring.Stroke = Res("ApexAccentBrush");
        ring.StrokeThickness = 2;
        var (light, dark) = (Tri(Res("ApexAccentBrush")), Tri(Res("TextOnAccentFillColorPrimaryBrush")));
        var inside = new Grid { Width = size, Height = size, IsHitTestVisible = false, Opacity = play.IsEnabled ? 1 : 0.45 };
        foreach (var e in new UIElement[] { glass, fill, ring, light, dark }) inside.Children.Add(e);
        (play.Width, play.Content) = (size, inside);
        (play.HorizontalAlignment, play.VerticalAlignment, play.Margin) = (HorizontalAlignment.Right, VerticalAlignment.Bottom, new Thickness(0, 0, 14, 14));

        Microsoft.UI.Composition.Visual V(UIElement e) => ElementCompositionPreview.GetElementVisual(e);
        var (pv, fv, lv, dv) = (V(play), V(fill), V(light), V(dark));
        var c = pv.Compositor;
        pv.CenterPoint = fv.CenterPoint = new(size / 2, size / 2, 0);
        fv.Scale = new(0.01f, 0.01f, 1);
        fv.Opacity = dv.Opacity = 0; // (at that size the middle would still show as a dot)

        var moves = AnimationsOn;
        var ease = c.CreateCubicBezierEasingFunction(new(0.22f, 1f), new(0.36f, 1f));
        var soft = c.CreateCubicBezierEasingFunction(new(0.25f, 0.1f), new(0.25f, 1f));
        TimeSpan Ms(int ms) => TimeSpan.FromMilliseconds(moves ? ms : 1);
        void Scale(Microsoft.UI.Composition.Visual v, float to, int ms)
        {
            var a = c.CreateVector3KeyFrameAnimation();
            a.InsertKeyFrame(1, new(to, to, 1), ease);
            a.Duration = Ms(ms);
            v.StartAnimation("Scale", a);
        }
        void Fade(Microsoft.UI.Composition.Visual v, float to, int ms, int wait = 0)
        {
            var a = c.CreateScalarKeyFrameAnimation();
            a.InsertKeyFrame(1, to, soft);
            (a.Duration, a.DelayTime) = (Ms(ms), TimeSpan.FromMilliseconds(moves ? wait : 0));
            v.StartAnimation("Opacity", a);
        }
        var (over, focus, shown) = (false, false, false);
        void Press() => Scale(pv, play.IsPressed ? 0.9f : shown ? 1.08f : 1f, play.IsPressed ? 100 : 320);
        void Open()
        {
            var on = play.IsEnabled && (over || focus);
            if (on == shown) return;
            shown = on;
            Scale(fv, on ? 1 : 0.01f, 340);
            Fade(fv, on ? 1 : 0, on ? 1 : 60, on ? 0 : 260);
            Fade(dv, on ? 1 : 0, 180);
            Fade(lv, on ? 0 : 1, 180);
            Press();
        }
        play.RegisterPropertyChangedCallback(Microsoft.UI.Xaml.Controls.Primitives.ButtonBase.IsPressedProperty, (_, _) => Press());
        play.GotFocus += (_, _) => { focus = play.FocusState == FocusState.Keyboard; Open(); };
        play.LostFocus += (_, _) => { focus = false; Open(); };
        return (play, on => { over = on; Open(); });
    }

    // a card that rises a little under the pointer, as the gallery's do
    static void Lift(FrameworkElement el, Action? enter = null, Action? exit = null)
    {
        el.SizeChanged += (_, e) => el.CenterPoint = new System.Numerics.Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0);
        el.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(200) };
        el.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(200) };
        el.PointerEntered += (_, _) => { el.Translation = new System.Numerics.Vector3(0, -4, 0); el.Scale = new System.Numerics.Vector3(1.02f, 1.02f, 1); enter?.Invoke(); };
        el.PointerExited += (_, _) => { el.Translation = System.Numerics.Vector3.Zero; el.Scale = System.Numerics.Vector3.One; exit?.Invoke(); };
    }

    // one quiet line for the saves with no backup, and one link that backs them all up
    void BackupLine(StackPanel line, List<Save> bare, string docs)
    {
        line.Children.Clear();
        line.Visibility = bare.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        if (bare.Count == 0) return;
        var icon = Lucide.Icon("hard-drive", 14, Res("ApexMutedBrush"));
        icon.VerticalAlignment = VerticalAlignment.Center;
        line.Children.Add(icon);
        var text = Muted(bare.Count == 1 ? T("1 of these saves has no backup") : T("{0} of these saves have no backup", bare.Count));
        text.VerticalAlignment = VerticalAlignment.Center;
        line.Children.Add(text);
        var now = Link(bare.Count == 1 ? T("Back up now") : T("Back up all"), () => { });
        now.Padding = new Thickness(0);
        now.VerticalAlignment = VerticalAlignment.Center;
        line.Children.Add(now);
        now.Click += async (_, _) =>
        {
            now.IsEnabled = false;
            now.Content = new ProgressRing { IsActive = true, Width = 12, Height = 12 };
            var left = new List<Save>();
            foreach (var save in bare)
                try { await Task.Run(() => Saves.Backup(save, docs)); }
                catch (Exception e) { App.Log(e); left.Add(save); ToolTipService.SetToolTip(line, ErrorText(e)); }
            BackupLine(line, left, docs);
        };
    }

    // ---------- updates and notes (also the updates-first dialog's rows) ----------

    Release? ReleaseOf(string c) => c == Components.Modernization ? ModRelease() : Repo.TryGetValue(c, out var repo) ? _latest.GetValueOrDefault(repo) : null;
    string? InstalledOf(string c) => c == Components.Apex ? _state?.ApexVersion : Known(c) is { Length: > 0 } v ? v : _state?.Installed(c);
    static List<string> Paragraphs(string? md) => (md ?? "").Replace("\r", "").Split("\n\n").Select(p => p.Trim()).Where(p => p.Length > 0 && !p.StartsWith('#') && !p.StartsWith('-')).Select(Plain).ToList();

    // ---------- the pictures: one row of the newest ----------

    UIElement HomePictures()
    {
        var s = new StackPanel { Spacing = 12 };
        var all = Gallery.List(Docs).Where(p => !p.Video).ToList();
        s.Children.Add(Head(T("Your pictures"), all.Count == 0 ? null : T("Gallery"), () => Show(GalleryScreen)));
        if (all.Count == 0) { s.Children.Add(Muted(T("Pictures you take in the game show here."))); return s; }
        var shown = all.Take(6).ToList();
        var row = new Grid { ColumnSpacing = 12 };
        var cells = new List<Button>();
        var culture = LangCulture();
        var white = new SolidColorBrush(Microsoft.UI.Colors.White);
        for (int i = 0; i < 6; i++)
        {
            row.ColumnDefinitions.Add(new ColumnDefinition());
            if (i >= shown.Count) continue;
            var p = shown[i];
            var at = i;
            var g = new Grid();
            g.Children.Add(new Image { Source = Thumb(p.Path, 520), Stretch = Stretch.UniformToFill });
            // under the pointer: when it was taken, and a way to put it on top of Home
            var over = new Grid { Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) } };
            over.Children.Add(new Border { Background = Fade(Windows.UI.Color.FromArgb(255, 12, 12, 12), [(0, 0xD0), (0.6, 0)]), IsHitTestVisible = false });
            over.Children.Add(new TextBlock { Text = p.Taken.ToString("d MMM · HH:mm", culture), FontSize = 12, Foreground = white, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(10, 0, 10, 8) });
            g.Children.Add(over);
            var isTop = _gal?.HomePicture == p.Path || (_gal?.HomePicture is null && i == 0);
            var pin = new Button
            {
                Width = 32, Height = 32, Padding = new Thickness(0), CornerRadius = new CornerRadius(16), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(0, 8, 8, 0), Opacity = isTop ? 1 : 0, BorderThickness = new Thickness(0),
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB8, 20, 20, 20)), OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) },
                Content = Lucide.Icon("house", 14, isTop ? Res("ApexAccentBrush") : white),
            };
            ToolTipService.SetToolTip(pin, isTop ? T("Home picture") : T("Use on Home"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(pin, isTop ? T("Home picture") : T("Use on Home"));
            pin.Click += (_, _) =>
            {
                if (isTop) return;
                (_gal ??= Gallery.Load()).HomePicture = p.Path;
                Gallery.Save(_gal);
                if (_settings.HomePicture != "gallery") { _settings.HomePicture = "gallery"; _settings.Save(); }
                BuildHome();
                Scroller.ChangeView(null, 0, null);
            };
            var b = new Button
            {
                Content = g, Padding = new Thickness(0), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(0), HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch, Background = Res("ApexPillBrush"),
            };
            b.Click += (_, _) => OpenViewer(all, at); // (no flight from the small copy: the viewer waits for the picture, then fades it in)
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, p.Taken.ToString("d MMM yyyy HH:mm", culture));
            var cell = new Grid();
            cell.Children.Add(b);
            cell.Children.Add(pin);
            Lift(cell, () => { over.Opacity = 1; pin.Opacity = 1; }, () => { over.Opacity = 0; pin.Opacity = isTop ? 1 : 0; });
            Grid.SetColumn(cell, i);
            row.Children.Add(cell);
            cells.Add(b);
        }
        // 16:9 at any width
        row.SizeChanged += (_, e) => { var cw = (e.NewSize.Width - 12 * 5) / 6; foreach (var c in cells) c.Height = Math.Max(90, cw * 9 / 16); };
        s.Children.Add(row);
        return s;
    }

    // the window's background, as chosen in Settings: Windows' Mica (tinted by the desktop wallpaper), its stronger kind,
    // Acrylic, or the launcher's own solid colour
    void ApplyBackdrop()
    {
        SystemBackdrop = _settings.Backdrop switch
        {
            "micaalt" => new MicaBackdrop { Kind = Microsoft.UI.Composition.SystemBackdrops.MicaKind.BaseAlt },
            "acrylic" => new DesktopAcrylicBackdrop(),
            "solid" => null,
            _ => new MicaBackdrop(),
        };
        if (_settings.Backdrop == "solid") Root.SetValue(Panel.BackgroundProperty, Res("ApexPaneBrush"));
        else Root.ClearValue(Panel.BackgroundProperty);
    }
}
