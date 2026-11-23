using System.Numerics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// Home: the save to go back to on top (its name, when it was saved, Continue straight into it, another save from a menu),
// one line with health and the last session under it, the other households, and the pictures. The picture on top is
// the player's choice in Settings: a gallery picture, the household's own photo, or none.
public sealed partial class MainWindow
{
    string? _homeSave;          // the save Continue opens: the one chosen on Home for this run, else the newest
    DispatcherTimer? _homeTick; // the playing time, ticking while the game runs

    void BuildHome()
    {
        WarmThumbs();
        var host = ScreenHome;
        host.Children.Clear();
        host.Spacing = 12;
        _homeTick?.Stop();
        if (_game is null)
        {
            host.Children.Add(Heading(T("Home")));
            host.Children.Add(Card(Body(T("The launcher found no The Sims 3. Choose the game folder in Settings.")), Button(T("Choose the game"), () => Show(GameStep))));
            return;
        }
        var saves = Saves.List(Docs);
        var save = saves.FirstOrDefault(s => s.Name == _homeSave) ?? saves.FirstOrDefault();
        if (_healthState == HealthState.Blocked)
            host.Children.Add(Banner(InfoBarSeverity.Error, T("Apex Radiance can't start"), T("The game still starts, but without Apex Radiance until this is fixed."), T("Open Health"), () => Show(HealthScreen)));
        if (_offline) host.Children.Add(Banner(InfoBarSeverity.Warning, T("No internet"), T("You can still play. Updates wait until you're back online."), T("Try again"), async () => await LoadReleasesAsync()));
        host.Children.Add(HomeTop(saves, save));
        host.Children.Add(HomeStatus());
        var others = saves.Where(s => s != save).Take(4).ToList();
        if (others.Count > 0) host.Children.Add(HomeOthers(others, saves.Count - 1));
        host.Children.Add(HomePictures());
    }

    static Border HomeCard(UIElement child) => new()
    {
        Child = child, Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(12),
    };

    // a section's head on Home: its title, a quiet note and a link on the right
    static UIElement Head(string title, string? note, string? link, Action? go)
    {
        var g = new Grid { ColumnSpacing = 8, Margin = new Thickness(0, 16, 0, 0) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new TextBlock { Text = title, Style = (Style)Application.Current.Resources["ApexSection"], VerticalAlignment = VerticalAlignment.Center });
        if (note is not null) { var n = Muted(note); n.VerticalAlignment = VerticalAlignment.Center; n.TextWrapping = TextWrapping.NoWrap; Grid.SetColumn(n, 1); g.Children.Add(n); }
        if (link is not null && go is not null) { var l = Link(link, go); l.Padding = new Thickness(0); l.VerticalAlignment = VerticalAlignment.Center; Grid.SetColumn(l, 2); g.Children.Add(l); }
        return g;
    }

    // when a save was made, in words
    string SavedWhen(DateTime t)
    {
        var c = LangCulture();
        var days = (DateTime.Today - t.Date).Days;
        return days <= 0 ? T("Saved today at {0}", t.ToString("HH:mm", c))
            : days == 1 ? T("Saved yesterday at {0}", t.ToString("HH:mm", c))
            : T("Saved {0} days ago, on {1}", days, t.ToString("d MMM", c));
    }

    // how long a session was, in words
    string Played(TimeSpan t) => t.TotalHours >= 1 ? T("{0} h {1} min played", (int)t.TotalHours, t.Minutes) : T("{0} min played", Math.Max(1, (int)t.TotalMinutes));

    // ---------- the top: the save to go back to ----------

    UIElement HomeTop(List<Save> saves, Save? save)
    {
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(24, 22, 24, 22) };
        words.Children.Add(Muted(save is null ? T("Welcome") : T("Continue where you left off")));
        words.Children.Add(new TextBlock { Text = save?.Name ?? "The Sims 3", FontSize = 28, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        words.Children.Add(Muted(save is null ? T("No saves yet: they show here once you save a game.") : SavedWhen(save.LastPlayed)));
        var acts = HomeActions(saves, save);
        acts.Margin = new Thickness(0, 14, 0, 0);
        words.Children.Add(acts);

        var mode = _settings.HomePicture;
        if (mode == "gallery" && HomePicture() is { } shot)
        {
            // the picture fades into the card at its foot, so the words and buttons sit on the card's own colour
            var g = new Grid { Height = Math.Clamp(_settings.HomePictureHeight, 200, 480) };
            g.Children.Add(new Image { Source = Thumb(shot, 1600), Stretch = Stretch.UniformToFill, VerticalAlignment = VerticalAlignment.Center, Opacity = Math.Clamp(_settings.HomePictureStrength, 30, 100) / 100.0 });
            var card = ((SolidColorBrush)Res("ApexCardBrush")).Color;
            var fade = new LinearGradientBrush { StartPoint = new(0, 1), EndPoint = new(0, 0) };
            fade.GradientStops.Add(new GradientStop { Color = card, Offset = 0 });
            fade.GradientStops.Add(new GradientStop { Color = Windows.UI.Color.FromArgb(0xEE, card.R, card.G, card.B), Offset = 0.3 });
            fade.GradientStops.Add(new GradientStop { Color = Windows.UI.Color.FromArgb(0x99, card.R, card.G, card.B), Offset = 0.5 });
            fade.GradientStops.Add(new GradientStop { Color = Windows.UI.Color.FromArgb(0, card.R, card.G, card.B), Offset = 0.78 });
            g.Children.Add(new Border { Background = fade, IsHitTestVisible = false });
            g.Children.Add(words);
            return HomeCard(g);
        }
        if (mode == "household" && save is not null)
        {
            var g = new Grid { MinHeight = 200 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(160) });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            var photo = new Grid { Background = Res("ApexPillBrush") };
            g.Children.Add(photo);
            Grid.SetColumn(words, 1);
            g.Children.Add(words);
            _ = Task.Run(() => Saves.Photo(save)).ContinueWith(async t =>
            {
                if (!t.IsFaulted && await SaveCard.ImageOf(t.Result) is { } img) photo.Children.Add(new Image { Source = img, Stretch = Stretch.UniformToFill, VerticalAlignment = VerticalAlignment.Top });
            }, TaskScheduler.FromCurrentSynchronizationContext());
            return HomeCard(g);
        }
        words.Margin = new Thickness(26, 24, 26, 24);
        return HomeCard(words);
    }

    // Continue goes straight into the save on top; Play without updating skips the ready updates; Another save picks the save
    StackPanel HomeActions(List<Save> saves, Save? save)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (_running)
        {
            var playing = new TextBlock { FontWeight = FontWeights.SemiBold, Foreground = Res("ApexAccentTextBrush"), VerticalAlignment = VerticalAlignment.Center };
            var pill = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            pill.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center });
            pill.Children.Add(playing);
            row.Children.Add(new Border { Child = pill, Background = Res("ApexAccentSoftBrush"), CornerRadius = new CornerRadius(8), Padding = new Thickness(18, 0, 20, 0), Height = 48 });
            var mem = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            var bar = new ProgressBar { Width = 160, Maximum = 1, VerticalAlignment = VerticalAlignment.Center };
            var used = Muted("");
            used.VerticalAlignment = VerticalAlignment.Center;
            mem.Children.Add(new TextBlock { Text = T("Memory"), Style = (Style)Application.Current.Resources["ApexMuted"], VerticalAlignment = VerticalAlignment.Center });
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
        var play = new Button { Height = 48, Padding = new Thickness(18, 0, 22, 0), CornerRadius = new CornerRadius(8), IsEnabled = !_busy && !Maintenance };
        if (!blocked) play.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        var pc = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        pc.Children.Add(new FontIcon { Glyph = "", FontSize = 13, VerticalAlignment = VerticalAlignment.Center });
        var label = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        var title = blocked ? T("Play anyway") : save is null ? T("Play") : T("Continue");
        label.Children.Add(new TextBlock { Text = title, FontSize = 15, FontWeight = FontWeights.SemiBold });
        var sub = blocked ? T("The game starts without Apex Radiance") : update ? (ups.Count == 1 ? T("1 update goes in first") : T("{0} updates go in first", ups.Count)) : null;
        if (sub is not null) label.Children.Add(new TextBlock { Text = sub, FontSize = 12, Opacity = 0.75 });
        pc.Children.Add(label);
        play.Content = pc;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(play, "PlayButton");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(play, title);
        if (update) ToolTipService.SetToolTip(play, string.Join("\n", ups.Select(c => Name(c) + " " + (ReleaseOf(c)?.Version ?? ""))));
        var name = save?.Name;
        play.Click += async (_, _) => await PlayAsync(name, update: update);
        row.Children.Add(play);
        if (update && !blocked)
        {
            var asIs = new Button { Content = T("Play without updating"), Height = 48, Padding = new Thickness(16, 0, 16, 0), CornerRadius = new CornerRadius(8), IsEnabled = !_busy && !Maintenance };
            asIs.Click += async (_, _) => await PlayAsync(name);
            row.Children.Add(asIs);
        }
        if (saves.Count > 1)
        {
            var menu = new MenuFlyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedLeft };
            foreach (var s in saves.Take(8))
            {
                var pick = s.Name;
                var item = new ToggleMenuFlyoutItem { Text = pick, IsChecked = s == save };
                item.Click += (_, _) => { _homeSave = pick; BuildHome(); };
                menu.Items.Add(item);
            }
            menu.Items.Add(new MenuFlyoutSeparator());
            var all = new MenuFlyoutItem { Text = T("All saves"), Icon = new FontIcon { Glyph = "" } };
            all.Click += (_, _) => Show(SavesScreen);
            menu.Items.Add(all);
            var other = new DropDownButton { Height = 48, Padding = new Thickness(14, 0, 12, 0), CornerRadius = new CornerRadius(8), Flyout = menu };
            var oc = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            oc.Children.Add(Lucide.Icon("folder", 15));
            oc.Children.Add(new TextBlock { Text = T("Another save"), VerticalAlignment = VerticalAlignment.Center });
            other.Content = oc;
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(other, T("Another save"));
            row.Children.Add(other);
        }
        return row;
    }

    Action? _homeTickAction;
    void OnHomeTick(object? sender, object e)
    {
        if (!_running || _step != Home) { _homeTick?.Stop(); return; }
        _homeTickAction?.Invoke();
    }

    // ---------- one line: health on the left, the last session on the right ----------

    UIElement HomeStatus()
    {
        var g = new Grid();
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1) });
        g.ColumnDefinitions.Add(new ColumnDefinition());

        // health: the crystal, the state and the first thing to look at
        var crystal = new Crystal(40);
        crystal.SetLevel(HealthLevel, animate: false);
        var first = _health.FirstOrDefault(h => _healthState == HealthState.Blocked ? h.Finding.Severity == Severity.Error : h.Finding.Severity != Severity.Info);
        var note = _healthState == HealthState.Ready || first is null ? T("Everything is in place.") : Sentences(T(first.Finding.Text, first.Finding.Args)).Item1;
        var tone = _healthState switch { HealthState.Blocked => "ApexErrorBrush", HealthState.NeedsYou => "ApexWarningBrush", _ => (string?)null };
        var end = _healthState switch
        {
            HealthState.Blocked => (FrameworkElement)Button(T("Open Health"), () => Show(HealthScreen)),
            HealthState.NeedsYou => Button(T("Look"), () => Show(HealthScreen)),
            _ => Lucide.Icon("chevron-right", 14, Res("ApexMutedBrush")),
        };
        var health = Half(crystal, HealthTitle(), note, tone, end);
        g.Children.Add(Clickable(health, T("Open Health"), () => Show(HealthScreen)));

        var rule = new Border { Background = Res("ApexCardBorderBrush"), Margin = new Thickness(0, 12, 0, 12) };
        Grid.SetColumn(rule, 1);
        g.Children.Add(rule);

        // the last session: how long, how it ended; after a crash or low memory, straight back into the newest save
        var last = SessionHistory.List().FirstOrDefault();
        var bad = last?.End is SessionEnd.Crash or SessionEnd.LowMemory;
        string lt, ls;
        if (last is null) { lt = T("No game played yet"); ls = T("Press Play to start"); }
        else
        {
            var when = last.Started.ToString("dddd HH:mm", LangCulture());
            lt = last.End switch { SessionEnd.Crash => T("The game closed on its own"), SessionEnd.LowMemory => T("Ran out of memory"), _ => Played(last.Played) };
            ls = bad ? Played(last.Played) + " · " + T("{0} at its peak", ModRow.SizeText(last.PeakBytes)) : T("Ended well") + " · " + when;
        }
        var icon = Lucide.Icon(bad ? "triangle-alert" : "calendar-clock", 18, Res(bad ? "ApexWarningBrush" : "ApexSoftTextBrush"));
        var icoBox = new Grid { Width = 40, Height = 40 };
        icon.HorizontalAlignment = HorizontalAlignment.Center; icon.VerticalAlignment = VerticalAlignment.Center;
        icoBox.Children.Add(icon);
        FrameworkElement? again = null;
        if (bad && !_running && Saves.Newest(Docs) is { } newest)
        {
            var b = Button(T("Start again into this save"), async () => await PlayAsync(newest.Name));
            b.IsEnabled = !_busy && !Maintenance;
            again = b;
        }
        var lastHalf = Half(icoBox, lt, ls, bad ? "ApexWarningBrush" : null, again);
        Grid.SetColumn(lastHalf, 2);
        g.Children.Add(lastHalf);
        return HomeCard(g);
    }

    // a half of the status line: a picture, a title over a quiet line, and an action at its end
    static Grid Half(FrameworkElement pic, string title, string text, string? tone, FrameworkElement? end)
    {
        var g = new Grid { ColumnSpacing = 14, Padding = new Thickness(14, 12, 16, 12) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        pic.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(pic);
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        var t = new TextBlock { Text = title, Style = (Style)Application.Current.Resources["ApexStrong"], TextTrimming = TextTrimming.CharacterEllipsis };
        if (tone is not null) t.Foreground = Res(tone);
        words.Children.Add(t);
        var line = Muted(text);
        line.TextWrapping = TextWrapping.NoWrap;
        line.TextTrimming = TextTrimming.CharacterEllipsis;
        ToolTipService.SetToolTip(line, text);
        words.Children.Add(line);
        Grid.SetColumn(words, 1);
        g.Children.Add(words);
        if (end is not null) { end.VerticalAlignment = VerticalAlignment.Center; Grid.SetColumn(end, 2); g.Children.Add(end); }
        return g;
    }

    // a part of a card that opens a screen: a lighter fill under the pointer, no lift
    static Grid Clickable(Grid content, string name, Action go)
    {
        var clear = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        content.Background = clear;
        content.CornerRadius = new CornerRadius(11, 0, 0, 11);
        content.PointerEntered += (_, _) => content.Background = Res("ApexHoverBrush");
        content.PointerExited += (_, _) => content.Background = clear;
        content.Tapped += (_, _) => go(); // (a button inside handles its own press: its tap does not reach here)
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(content, name);
        return content;
    }

    // ---------- the other households: each into its own save, with its backup ----------

    UIElement HomeOthers(List<Save> others, int count)
    {
        var s = new StackPanel { Spacing = 12 };
        s.Children.Add(Head(T("Other households"), count == 1 ? T("1 save") : T("{0} saves", count), T("All saves"), () => Show(SavesScreen)));
        var grid = new Grid { ColumnSpacing = 12, RowSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        var docs = Docs;
        var rows = new List<(Save Save, Grid Photo, TextBlock Meta, StackPanel Backup)>();
        for (int i = 0; i < others.Count; i++)
        {
            var save = others[i];
            var g = new Grid { ColumnSpacing = 16, Padding = new Thickness(0, 0, 16, 0) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var photo = new Grid { Height = 96, Background = Res("ApexPillBrush") };
            g.Children.Add(photo);
            var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            words.Children.Add(new TextBlock { Text = save.Name, Style = (Style)Application.Current.Resources["ApexStrong"], TextTrimming = TextTrimming.CharacterEllipsis });
            var meta = Muted(save.LastPlayed.ToString("d MMM", LangCulture()));
            meta.TextWrapping = TextWrapping.NoWrap;
            words.Children.Add(meta);
            var backup = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            words.Children.Add(backup);
            Grid.SetColumn(words, 1);
            g.Children.Add(words);
            var name = save.Name;
            var go = new Button { Width = 36, Height = 36, Padding = new Thickness(0), CornerRadius = new CornerRadius(18), VerticalAlignment = VerticalAlignment.Center,
                IsEnabled = !_busy && !Maintenance && !_running, Content = new FontIcon { Glyph = "", FontSize = 11, Margin = new Thickness(2, 0, 0, 0) } };
            ToolTipService.SetToolTip(go, T("Play this save"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(go, T("Play {0}", name));
            go.Click += async (_, _) => await PlayAsync(name);
            Grid.SetColumn(go, 2);
            g.Children.Add(go);
            var card = HomeCard(g);
            Grid.SetColumn(card, i % 2);
            if (grid.RowDefinitions.Count <= i / 2) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Grid.SetRow(card, i / 2);
            grid.Children.Add(card);
            rows.Add((save, photo, meta, backup));
        }
        // a narrow window: one household a row
        grid.SizeChanged += (_, e) =>
        {
            var one = e.NewSize.Width < 640;
            grid.ColumnDefinitions[1].Width = one ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
            grid.ColumnSpacing = one ? 0 : 12;
            while (grid.RowDefinitions.Count < (one ? others.Count : (others.Count + 1) / 2)) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            for (int i = 0; i < grid.Children.Count; i++) { Grid.SetColumn((FrameworkElement)grid.Children[i], one ? 0 : i % 2); Grid.SetRow((FrameworkElement)grid.Children[i], one ? i : i / 2); }
        };
        s.Children.Add(grid);

        // the photos, sizes and backups read off the UI thread
        _ = Task.Run(() => (SaveHealth.Check(docs, record: false), others.Select(Saves.Photo).ToList())).ContinueWith(async t =>
        {
            if (t.IsFaulted) return;
            var (reports, photos) = t.Result;
            for (int i = 0; i < rows.Count; i++)
            {
                var (save, photo, meta, backup) = rows[i];
                if (reports.FirstOrDefault(r => r.Save.Dir == save.Dir) is { } r)
                {
                    meta.Text = save.LastPlayed.ToString("d MMM", LangCulture()) + " · " + ModRow.SizeText(r.Bytes);
                    BackupLine(backup, save, docs, r.LastBackup is not null);
                }
                if (await SaveCard.ImageOf(photos[i]) is { } img) photo.Children.Add(new Image { Source = img, Stretch = Stretch.UniformToFill, VerticalAlignment = VerticalAlignment.Top });
            }
        }, TaskScheduler.FromCurrentSynchronizationContext());
        return s;
    }

    // "Backed up", or "No backup · Back up now", which turns into a ring and then a tick
    void BackupLine(StackPanel line, Save save, string docs, bool backedUp)
    {
        line.Children.Clear();
        if (backedUp) { line.Children.Add(Muted(T("backed up"))); return; }
        var warn = Muted(T("No backup"));
        warn.Foreground = Res("ApexWarningBrush");
        line.Children.Add(warn);
        var now = Link(T("Back up now"), () => { });
        now.Padding = new Thickness(0);
        now.FontSize = 12;
        line.Children.Add(now);
        now.Click += async (_, _) =>
        {
            now.IsEnabled = false;
            now.Content = new ProgressRing { IsActive = true, Width = 12, Height = 12 };
            try { await Task.Run(() => Saves.Backup(save, docs)); BackupLine(line, save, docs, true); }
            catch (Exception e) { App.Log(e); now.Content = T("Try again"); now.IsEnabled = true; ToolTipService.SetToolTip(now, ErrorText(e)); }
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
        s.Children.Add(Head(T("Your pictures"), all.Count == 0 ? null : all.Count.ToString(), all.Count == 0 ? null : T("Gallery"), () => Show(GalleryScreen)));
        if (all.Count == 0) { s.Children.Add(Muted(T("Pictures you take in the game show here."))); return s; }
        var row = new Grid { ColumnSpacing = 8, Margin = new Thickness(0, 0, 0, 24) };
        for (int i = 0; i < 6; i++)
        {
            row.ColumnDefinitions.Add(new ColumnDefinition());
            if (i >= all.Count) continue;
            var b = new Border { CornerRadius = new CornerRadius(8), Background = Res("ApexPillBrush"), Height = 104, Child = new Image { Source = Thumb(all[i].Path, 420), Stretch = Stretch.UniformToFill } };
            Grid.SetColumn(b, i);
            row.Children.Add(b);
        }
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
