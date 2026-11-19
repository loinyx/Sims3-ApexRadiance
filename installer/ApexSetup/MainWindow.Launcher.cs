using System.Diagnostics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// Apex Launcher's own screens (Home, Mods, Worlds, Saves, Health, What's new, Settings), Play and the play session, and
// the administrator window for changes the game folder needs it for. The install wizard and Repair or remove are the
// screens Setup had (MainWindow.xaml.cs).
public sealed partial class MainWindow
{
    const int Home = 8, ModsScreen = 9, WorldsScreen = 10, SavesScreen = 11, HealthScreen = 12, NewsScreen = 13, SettingsScreen = 14;
    readonly LauncherSettings _settings = LauncherSettings.Load();
    List<Health.Item> _health = [];
    HealthState _healthState;
    DateTime _checked;
    List<ModFile> _mods = [];
    HashSet<string> _modLooks = new(StringComparer.OrdinalIgnoreCase);
    bool _modsLoaded, _modsLoading;
    string _modKind = "";
    int _healthTab, _newsTab;
    Session? _session;
    SessionInfo? _lastSession;
    Process? _maintenance;
    Tray? _tray;
    Crystal? _homeCrystal, _healthCrystal;
    readonly HashSet<int> _openArea = [];

    string Docs => _game is { } g ? Scan.DocumentsFor(g) : "";
    public static bool Maintenance => Elevation.IsElevated && Elevation.Parse(Environment.GetCommandLineArgs()).Bin is not null;

    // ---------- refresh ----------

    void RefreshLauncher()
    {
        WindowTitle.Text = Maintenance ? T("Apex Launcher (administrator)") : "Apex Launcher";
        HealthBadge.Visibility = _game is not null && _healthState != HealthState.Ready ? Visibility.Visible : Visibility.Collapsed;
        switch (_step)
        {
            case Home: BuildHome(); break;
            case ModsScreen: FillMods(); break;
            case WorldsScreen: BuildWorlds(); break;
            case SavesScreen: FillSaves(); break;
            case HealthScreen: BuildHealth(); break;
            case NewsScreen: BuildNews(); break;
            case SettingsScreen: BuildSettings(); break;
        }
    }

    // Compat, Verify and the mod checks, run again (Check now, after a change, at start)
    void CheckHealth()
    {
        if (_game is not { } g || _state is not { } s) { _health = []; _healthState = HealthState.Ready; return; }
        try { _health = Health.Gather(g, s, Docs, _latest.GetValueOrDefault(Releases.Apex), _modsLoaded ? _mods : null); }
        catch (Exception e) { App.Log(e); _health = []; }
        _healthState = Health.StateOf(_health);
        _checked = DateTime.Now;
    }

    // ---------- Home ----------

    void BuildHome()
    {
        var host = ScreenHome;
        host.Children.Clear();
        if (_game is not { } g)
        {
            host.Children.Add(Heading(T("Home")));
            host.Children.Add(Card(Body(T("The launcher found no The Sims 3. Choose the game folder in Settings.")), Button(T("Choose the game"), () => Show(GameStep))));
            return;
        }
        // what waits for the player first
        if (_running) host.Children.Add(Banner(InfoBarSeverity.Success, T("The Sims 3 is running"), _session is { } ss ? T("Playing for {0}.", Duration(ss.Played)) : ""));
        else if (WelcomeUpdates() is { Count: > 0 } ups)
            host.Children.Add(Banner(InfoBarSeverity.Informational, ups.Count == 1 ? T("1 update is ready") : T("{0} updates are ready", ups.Count),
                _settings.InstallUpdatesOnPlay ? T("It goes in when you press Play; your settings stay.") : T("Install it before you play; your settings stay."),
                T("See updates"), () => Show(Welcome)));
        if (_offline) host.Children.Add(Banner(InfoBarSeverity.Warning, T("No internet"), T("You can still play. Updates wait until you're back online."), T("Try again"), async () => await LoadReleasesAsync()));
        if (_healthState == HealthState.Blocked)
            host.Children.Add(Banner(InfoBarSeverity.Error, T("Apex Radiance can't start"), T("The game still starts, but without Apex Radiance until this is fixed."), T("Open Health"), () => Show(HealthScreen)));

        host.Children.Add(Hero(g));
        host.Children.Add(HealthWidget());

        var cards = new Grid { ColumnSpacing = 16 };
        for (int i = 0; i < 3; i++) cards.ColumnDefinitions.Add(new ColumnDefinition());
        var newest = Saves.Newest(Docs);
        var cont = Card(Col(Eyebrow(T("CONTINUE")), Strong(newest?.Name ?? T("No save yet")),
            Muted(newest is null ? T("Start a new game from the game's menu.") : T("Starts the game straight into your last save.")),
            newest is null ? new Border() : Link(T("Pick another save"), () => Show(SavesScreen))));
        var apex = _latest.GetValueOrDefault(Releases.Apex);
        var news = Card(Col(Eyebrow(T("WHAT'S NEW")), Strong(apex is null ? T("Release notes") : "Apex Radiance " + apex.Version),
            Muted(FirstLine(apex?.Body) ?? T("The notes of each release, read from GitHub.")), Link(T("All release notes"), () => Show(NewsScreen))));
        var pics = Card(Col(Eyebrow(T("YOUR PICTURES")), Pictures(), Link(T("Screenshots folder"), () => OpenFolder(Path.Combine(Docs, "Screenshots")))));
        Grid.SetColumn(news, 1); Grid.SetColumn(pics, 2);
        cards.Children.Add(cont); cards.Children.Add(news); cards.Children.Add(pics);
        host.Children.Add(cards);
    }

    UIElement Hero(Game g)
    {
        var grid = new Grid { Height = 300, CornerRadius = new CornerRadius(12), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) };
        if (Screenshots(1).FirstOrDefault() is { } shot)
            grid.Children.Add(new Image { Source = new BitmapImage(new Uri(shot)), Stretch = Stretch.UniformToFill });
        var shade = new Border
        {
            Background = new LinearGradientBrush
            {
                StartPoint = new Windows.Foundation.Point(0, 1), EndPoint = new Windows.Foundation.Point(0, 0),
                GradientStops = { new GradientStop { Color = Windows.UI.Color.FromArgb(240, 11, 12, 14), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(40, 11, 12, 14), Offset = 1 } },
            },
        };
        grid.Children.Add(shade);
        var row = new Grid { Padding = new Thickness(28), VerticalAlignment = VerticalAlignment.Bottom, ColumnSpacing = 20 };
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var left = Col(Eyebrow(g.Label.ToUpperInvariant()), new TextBlock { Text = "The Sims 3", FontSize = 36, FontWeight = FontWeights.SemiBold });
        ((StackPanel)left).Spacing = 8;
        var versions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        versions.Children.Add(new Image { Source = new BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = 16, Height = 16 });
        versions.Children.Add(Muted("Apex Radiance " + (_state?.ApexVersion ?? "?")
            + (Known(Components.Modernization) is { Length: > 0 } mv ? "  ·  Sims 3 Modernization " + mv : "")));
        ((StackPanel)left).Children.Add(versions);
        var badges = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
        foreach (var (p, dir) in Expansions.Installed(g).Where(x => x.Pack.Expansion)) badges.Children.Add(PackBadge(p, dir, 30));
        ((StackPanel)left).Children.Add(badges);
        row.Children.Add(left);
        UIElement action;
        if (_running)
        {
            action = new Border { Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 22, 36, 28)), CornerRadius = new CornerRadius(8), Padding = new Thickness(20, 14, 20, 14),
                Child = new TextBlock { Text = _session is { } s ? T("Playing · {0}", Duration(s.Played)) : T("Playing"), Foreground = Res("ApexSuccessBrush"), FontWeight = FontWeights.SemiBold } };
        }
        else
        {
            var play = new Button { Style = (Style)Application.Current.Resources["AccentButtonStyle"], MinWidth = 180, Height = 52, IsEnabled = !_busy && !Maintenance };
            play.Content = new TextBlock { Text = _healthState == HealthState.Blocked ? T("Play anyway") : T("Play"), FontSize = 17, FontWeight = FontWeights.SemiBold };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(play, "PlayButton");
            play.Click += async (_, _) => await PlayAsync(null);
            action = play;
        }
        Grid.SetColumn((FrameworkElement)action, 1);
        ((FrameworkElement)action).VerticalAlignment = VerticalAlignment.Bottom;
        row.Children.Add(action);
        grid.Children.Add(row);
        return grid;
    }

    UIElement HealthWidget()
    {
        var b = new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(14, 10, 22, 10), CornerRadius = new CornerRadius(14) };
        // the state's colour coming in from the crystal's side, as in the design
        b.Background = new LinearGradientBrush
        {
            StartPoint = new Windows.Foundation.Point(0, 0.5), EndPoint = new Windows.Foundation.Point(1, 0.5),
            GradientStops = { new GradientStop { Color = CardTint(0.07), Offset = 0 }, new GradientStop { Color = CardTint(0), Offset = 0.6 } },
        };
        var g = new Grid { ColumnSpacing = 20 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _homeCrystal = new Crystal(84); // (a new one each time: a XAML element cannot move between rebuilt screens)
        _homeCrystal.SetState(_healthState, animate: false);
        g.Children.Add(_homeCrystal);
        var text = Col(Eyebrow(T("HEALTH")), new TextBlock { Text = HealthTitle(), FontSize = 20, FontWeight = FontWeights.SemiBold },
            Muted(_checked == default ? T("Checked when you press Play") : T("Checked {0} · checked again when you press Play", _checked.ToString("t"))));
        var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Margin = new Thickness(0, 4, 0, 0) };
        chips.Children.Add(Chip(WelcomeUpdates().Count > 0 ? T("Update ready") : T("Up to date"), WelcomeUpdates().Count > 0 ? "ApexAccentBrush" : "ApexSuccessBrush", () => Show(WelcomeUpdates().Count > 0 ? Welcome : NewsScreen)));
        var issues = _health.Count(i => i.Finding.Severity != Severity.Info);
        chips.Children.Add(Chip(issues == 0 ? T("All checks passed") : issues == 1 ? T("1 thing to look at") : T("{0} things to look at", issues), issues == 0 ? "ApexSuccessBrush" : "ApexWarningBrush", () => Show(HealthScreen)));
        if (_modLooks.Count > 0) chips.Children.Add(Chip(_modLooks.Count == 1 ? T("1 mod needs a look") : T("{0} mods need a look", _modLooks.Count), "ApexWarningBrush", () => Show(ModsScreen)));
        ((StackPanel)text).Children.Add(chips);
        Grid.SetColumn((FrameworkElement)text, 1);
        ((FrameworkElement)text).VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(text);
        var open = Button(T("Open Health"), () => Show(HealthScreen));
        Grid.SetColumn(open, 2);
        open.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(open);
        b.Child = g;
        return b;
    }

    // the crystal's colour for the state, at an alpha (0 to 255)
    Windows.UI.Color StateTint(byte alpha) => _healthState switch
    {
        HealthState.Blocked => Windows.UI.Color.FromArgb(alpha, 233, 68, 54),
        HealthState.NeedsYou => Windows.UI.Color.FromArgb(alpha, 245, 166, 40),
        _ => Windows.UI.Color.FromArgb(alpha, 67, 215, 62),
    };

    // the card's own grey with a little of the state's colour, opaque: a fade from a transparent colour turns into a band of
    // the pure colour (the brush blends the colour before its transparency)
    Windows.UI.Color CardTint(double amount)
    {
        var t = StateTint(255);
        byte Mix(byte card, byte c) => (byte)Math.Round(card + (c - card) * amount);
        return Windows.UI.Color.FromArgb(255, Mix(0x13, t.R), Mix(0x14, t.G), Mix(0x17, t.B));
    }

    string HealthTitle() => _healthState switch
    {
        HealthState.Blocked => T("Apex Radiance can't start"),
        HealthState.NeedsYou => _health.Count(i => i.Finding.Severity != Severity.Info) is var n && n == 1 ? T("1 thing to look at") : T("{0} things to look at", _health.Count(i => i.Finding.Severity != Severity.Info)),
        _ => T("Ready to play"),
    };

    UIElement Pictures()
    {
        var row = new Grid { ColumnSpacing = 6, Height = 64 };
        var shots = Screenshots(3);
        for (int i = 0; i < 3; i++)
        {
            row.ColumnDefinitions.Add(new ColumnDefinition());
            var b = new Border { CornerRadius = new CornerRadius(4), Background = Res("ApexPillBrush") };
            if (i < shots.Count) b.Child = new Image { Source = new BitmapImage(new Uri(shots[i])) { DecodePixelWidth = 200 }, Stretch = Stretch.UniformToFill };
            Grid.SetColumn(b, i);
            row.Children.Add(b);
        }
        return row;
    }

    List<string> Screenshots(int n)
    {
        var dir = Path.Combine(Docs, "Screenshots");
        try
        {
            return !Directory.Exists(dir) ? [] : new DirectoryInfo(dir).EnumerateFiles().Where(f => f.Extension.ToLowerInvariant() is ".jpg" or ".jpeg" or ".png")
                .OrderByDescending(f => f.LastWriteTime).Take(n).Select(f => f.FullName).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    // a pack's own icon from the player's install (its game definition file), else its short code on its colour
    static FrameworkElement PackBadge(Pack p, string? dir, double size)
    {
        var b = new Border { Width = size, Height = size, CornerRadius = new CornerRadius(size * 0.22), Background = new SolidColorBrush(PackColor(p.Code)) };
        b.Child = new TextBlock { Text = p.Short, FontSize = size * 0.34, FontWeight = FontWeights.Bold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        ToolTipService.SetToolTip(b, p.Name);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, p.Name);
        if (dir is not null)
            _ = Task.Run(() => Expansions.Icon(dir)).ContinueWith(async t =>
            {
                if (await SaveCard.ImageOf(t.Result) is { } img) { b.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent); b.Child = new Image { Source = img, Width = size, Height = size }; }
            }, TaskScheduler.FromCurrentSynchronizationContext());
        return b;
    }

    static Windows.UI.Color PackColor(string code) => code switch
    {
        "EP1" => Rgb(0x3E, 0x6E, 0x8E), "EP2" => Rgb(0x7A, 0x5A, 0x2E), "EP3" => Rgb(0x5B, 0x3F, 0x7A), "EP4" => Rgb(0x3F, 0x7A, 0x5B),
        "EP5" => Rgb(0x8A, 0x4E, 0x3A), "EP6" => Rgb(0x7A, 0x3F, 0x62), "EP7" => Rgb(0x3F, 0x4A, 0x7A), "EP8" => Rgb(0x4E, 0x7A, 0x3F),
        "EP9" => Rgb(0x7A, 0x6A, 0x2E), "EP10" => Rgb(0x2E, 0x6E, 0x7A), _ => Rgb(0x4A, 0x4F, 0x6E),
    };
    static Windows.UI.Color Rgb(byte r, byte g, byte b) => Windows.UI.Color.FromArgb(255, r, g, b);

    // ---------- Play and the session ----------

    async Task PlayAsync(string? save)
    {
        if (_game is not { } g || _busy) return;
        if (_running || Games.IsRunning()) { await Message(T("The Sims 3 is already running"), T("Switch to the game, or close it and press Play again.")); return; }
        // updates already offered: in before the game starts, as the player chose in Settings
        if (WelcomeUpdates() is { Count: > 0 } ups)
        {
            var install = _settings.InstallUpdatesOnPlay || await Confirm(T("Updates are ready"), T("Install them first? It takes about a minute; your settings stay."), T("Install and play"));
            if (install)
            {
                await InstallAsync(ups);
                if (WelcomeUpdates().Count > 0) return; // (it failed or was cancelled: its message said why)
            }
        }
        CheckHealth();
        // the fixes that are safe to make without asking (each keeps a copy of the file it changes)
        if (_settings.FixSafeAutomatically && _health.Any(i => i.Finding.CanFix))
        {
            if (!await Guarded(() => { Engine.CheckWritable(); foreach (var x in Compat.Check(g, Scan.Run(g)).Where(x => x.CanFix)) x.Fix!(); },
                T("Fixed."), null, "Fix incompatible settings")) return;
            RefreshGame();
            CheckHealth();
        }
        if (_healthState == HealthState.Blocked)
        {
            var first = _health.First(i => i.Finding.Severity == Severity.Error && !i.Finding.CanFix).Finding;
            var r = await ShowDialog(new ContentDialog
            {
                Title = T("One thing before you play"), Content = T(first.Text, first.Args) + "\n\n" + T("The game starts without Apex Radiance until this is fixed."),
                PrimaryButtonText = T("Open Health"), SecondaryButtonText = T("Play anyway"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
            });
            if (r == ContentDialogResult.Primary) { Show(HealthScreen); return; }
            if (r != ContentDialogResult.Secondary) return;
        }
        await StartGameAsync(g, save);
    }

    async Task StartGameAsync(Game g, string? save)
    {
        Process p;
        try { p = Launch.Start(g, save); }
        catch (Exception e) { App.Log(e); await Message(T("The game did not start"), ErrorText(e)); return; }
        var session = _session = new Session(p);
        _running = true;
        if (_settings.HideWhilePlaying) { TrayShow(T("Apex Launcher · The Sims 3 is running")); AppWindow.Hide(); }
        RefreshAll();
        var info = await session.WatchAsync(new Progress<Session>(s =>
        {
            _tray?.Show(T("The Sims 3 · {0} · {1} MB", Duration(s.Played), s.MemoryBytes >> 20));
            if (_step == Home && Environment.TickCount64 % 30000 < 5000) BuildHome(); // (the playing time on Home, now and then)
        }));
        _lastSession = info;
        _session = null;
        _running = Games.IsRunning();
        AppWindow.Show();
        Activate();
        _tray?.Hide();
        RefreshGame();
        CheckHealth();
        RefreshAll();
        if (Session.ShouldOffer(info, _state?.RestartHost ?? false))
        {
            var low = info.End == SessionEnd.LowMemory;
            var again = await Confirm(low ? T("You quit after saving low on memory") : T("The game closed unexpectedly"),
                low ? T("A fresh start clears the memory. Pick up right where you saved.") : T("Your last save is safe. Start again where you left off."),
                T("Start again into this save"));
            if (again && Saves.Newest(Docs) is { } last) await StartGameAsync(g, last.Name);
        }
    }

    void TrayShow(string tip)
    {
        _tray ??= new Tray(Path.Combine(AppContext.BaseDirectory, "Assets", "apex.ico"), () => DispatcherQueue.TryEnqueue(() => { AppWindow.Show(); Activate(); }),
            () => [(T("Open Apex Launcher"), () => DispatcherQueue.TryEnqueue(() => { AppWindow.Show(); Activate(); })),
                   (T("Quit"), () => DispatcherQueue.TryEnqueue(() => { _tray?.Dispose(); Application.Current.Exit(); }))]);
        _tray.Show(tip);
    }

    static string Duration(TimeSpan t) => t.TotalHours >= 1 ? $"{(int)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}" : $"{t.Minutes}:{t.Seconds:00}";

    // ---------- the administrator window ----------

    // A change to the game's files: made here when this copy may write there, else in the administrator window, opened on
    // the same screen; this window waits, hidden, and reads the game again when it closes. False when the change was not
    // made here (the caller stops).
    // drivers: NVIDIA's and AMD's driver settings (Smooth Motion, AFMF) are machine-wide and always need it
    async Task<bool> CanChangeHere(string screen, bool drivers = false)
    {
        if (_game is not { } g || Elevation.IsElevated || (!drivers && Elevation.CanWrite(g.Bin))) return true;
        if (Environment.ProcessPath is not { } exe) return true;
        if (!await Confirm(T("Administrator permission needed"), T("The game is in a protected folder. Windows will ask for permission, then the change opens in its own window."), T("Continue"))) return false;
        _maintenance = Elevation.StartMaintenance(exe, g.Bin, screen);
        if (_maintenance is null) { await Message(T("Nothing was changed"), T("Windows did not allow the change.")); return false; }
        AppWindow.Hide();
        try { await _maintenance.WaitForExitAsync(); } catch (Exception e) { App.Log(e); }
        _maintenance = null;
        AppWindow.Show();
        Activate();
        _defaultsFor = null;
        _modsLoaded = false;
        RefreshGame();
        CheckHealth();
        if (_step is Welcome or GameStep && !FirstInstall) Show(Home); // (a first install finished in the administrator window)
        else RefreshAll();
        return false;
    }

    bool NeedsAdminWindow => _game is { } g && !Elevation.IsElevated && !Elevation.CanWrite(g.Bin) && Preview is null;

    // the screen the administrator window opens on
    int MaintenanceScreen(string? screen) => screen switch { "manage" => Manage, "welcome" => Welcome, "health" => HealthScreen, _ => ComponentsStep };

    // ---------- Mods ----------

    async Task LoadModsAsync()
    {
        if (_game is not { } g || _modsLoading) return;
        _modsLoading = true;
        ModsSummary.Text = T("Reading your mods…");
        var docs = Docs;
        try
        {
            var (mods, looks) = await Task.Run(() =>
            {
                var m = Mods.List(g, docs);
                return (m, ModChecks.Run(m, g, docs).SelectMany(f => f.Files).ToHashSet(StringComparer.OrdinalIgnoreCase));
            });
            _mods = mods;
            _modLooks = looks;
            _modsLoaded = true;
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The launcher could not read your mods"), ErrorText(e)); }
        finally { _modsLoading = false; }
        var folders = new List<string> { T("All folders") };
        folders.AddRange(_mods.Select(m => m.Folder).Distinct(StringComparer.OrdinalIgnoreCase).Order());
        ModFolder.ItemsSource = folders;
        ModFolder.SelectedIndex = 0;
        FillMods();
    }

    // the filters: the file type (tabs), what the files hold (chips, the ones of the chosen type), then search, folder, state
    string _modType = "";
    static string TypeOf(ModFile m) => m.FileType;
    bool _fillingMods;

    void FillMods()
    {
        if (_game is null) { ModsSummary.Text = T("Choose the game first."); return; }
        if (!_modsLoaded) { _ = LoadModsAsync(); return; }
        _fillingMods = true;
        try
        {
            var size = _mods.Sum(m => m.Size);
            ModsSummary.Text = T("{0} files \u00b7 {1} MB", _mods.Count, size >> 20);
            ModRow.Toggle = ToggleMod;
            // file types, each with its count
            (string Key, string Label)[] types = [("", T("All")), ("package", T("Packages")), ("asi", T("ASI plugins")), ("sims3pack", "Sims3Packs"), ("library", T("Library"))];
            ModTypes.Items.Clear();
            foreach (var (key, label) in types)
            {
                var n = key == "" ? _mods.Count : _mods.Count(m => TypeOf(m) == key);
                if (n == 0 && key != "") continue;
                var tab = Tab($"{label}  {n}", ModTypes.Items.Count == 0);
                tab.Tag = key;
                ModTypes.Items.Add(tab);
            }
            ModTypes.SelectedItem = ModTypes.Items.FirstOrDefault(i => (string)i.Tag == _modType) ?? ModTypes.Items.FirstOrDefault();
            // what the chosen type's files hold
            var inType = _mods.Where(m => _modType == "" || TypeOf(m) == _modType).ToList();
            ModKinds.Children.Clear();
            // (the file types have their tabs: the chips are what the packages, or the Library, hold)
            var kinds = Enum.GetValues<ModKind>().Where(k => k is not (ModKind.Asi or ModKind.Sims3Pack) && (_modType is "library" or "sims3pack" || k is not (ModKind.World or ModKind.Lot or ModKind.Household))).OrderBy(k => k == ModKind.Package ? 1 : 0).Select(k => (k, n: inType.Count(m => m.Kind == k))).Where(x => x.n > 0).ToList(); // ("Other" last)
            ModKinds.Visibility = kinds.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
            if (kinds.Count > 1)
            {
                ModKinds.Children.Add(KindChip("", T("Everything"), kinds.Sum(x => x.n)));
                foreach (var (k, n) in kinds) ModKinds.Children.Add(KindChip(k.ToString(), ModRow.KindName(k), n));
            }
            if (ModState.Items.Count == 0)
            {
                foreach (var t in new[] { T("Any state"), T("On"), T("Off"), T("Needs a look") }) ModState.Items.Add(t);
                ModState.SelectedIndex = 0;
            }
            var shown = Filtered().ToList();
            var filtered = _modType != "" || _modKind != "" || ModState.SelectedIndex > 0 || ModFolder.SelectedIndex > 0 || (ModSearch.Text ?? "").Length > 0;
            ModClear.Visibility = filtered ? Visibility.Visible : Visibility.Collapsed;
            var gallery = ModsViewGallery.IsSelected;
            ModListCard.Visibility = gallery ? Visibility.Collapsed : Visibility.Visible;
            ModGallery.Visibility = gallery ? Visibility.Visible : Visibility.Collapsed;
            if (gallery) FillGallery(shown);
            else ModList.ItemsSource = shown.Select(m => new ModRow(m, _modLooks.Contains(m.Path))).ToList();
            ModsNote.Text = (shown.Count == 0 ? T("No file matches these filters.") + " " : "")
                + T("Switching a mod off moves it to Mods\\{0}, out of the game's reach; switching it on puts it back. Components the launcher installs are changed in Components.", Mods.DisabledName);
        }
        finally { _fillingMods = false; }
    }

    ToggleButton KindChip(string key, string label, int count)
    {
        var b = new ToggleButton { IsChecked = _modKind == key, Padding = new Thickness(12, 4, 12, 5), CornerRadius = new CornerRadius(14) };
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        s.Children.Add(new TextBlock { Text = label, FontSize = 13 });
        s.Children.Add(new TextBlock { Text = count.ToString(), FontSize = 12, Opacity = 0.7, VerticalAlignment = VerticalAlignment.Center });
        b.Content = s;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, label);
        b.Click += (_, _) => { _modKind = key; FillMods(); };
        return b;
    }

    IEnumerable<ModFile> Filtered()
    {
        var q = ModSearch.Text?.Trim() ?? "";
        var folder = ModFolder.SelectedIndex > 0 ? ModFolder.SelectedItem as string : null;
        var state = ModState.SelectedIndex;
        return _mods.Where(m =>
            (_modType == "" || TypeOf(m) == _modType)
            && (_modKind == "" || m.Kind.ToString() == _modKind)
            && (q.Length == 0 || m.Rel.Contains(q, StringComparison.OrdinalIgnoreCase))
            && (folder is null || m.Folder.Equals(folder, StringComparison.OrdinalIgnoreCase) || m.Folder.StartsWith(folder + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            && state switch { 1 => m.Enabled, 2 => !m.Enabled, 3 => _modLooks.Contains(m.Path), _ => true });
    }

    void OnModType(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        if (_fillingMods || sender.SelectedItem?.Tag is not string key || key == _modType) return;
        _modType = key;
        _modKind = ""; // (the content chips are the new type's)
        DispatcherQueue.TryEnqueue(FillMods);
    }

    void OnModClear(object sender, RoutedEventArgs e)
    {
        _modType = _modKind = "";
        _fillingMods = true;
        ModSearch.Text = "";
        ModFolder.SelectedIndex = 0;
        ModState.SelectedIndex = 0;
        _fillingMods = false;
        FillMods();
    }

    ModFile? ToggleMod(ModRow row, bool on)
    {
        try
        {
            var path = Mods.SetEnabled(row.File, on, Docs);
            var moved = row.File with { Path = path, Enabled = on };
            _mods[_mods.IndexOf(row.File)] = moved;
            return moved;
        }
        catch (Exception e)
        {
            App.Log(e);
            SetInfo(ModsInfo, InfoBarSeverity.Error, T("The mod was not moved"), ErrorText(e));
            DispatcherQueue.TryEnqueue(FillMods); // (the switch back as it was)
            return null;
        }
    }

    const int GalleryCap = 120;

    void FillGallery(List<ModFile> shown)
    {
        ModGallery.Children.Clear();
        foreach (var group in shown.Where(m => m.Kind != ModKind.Asi).GroupBy(m => m.Kind).OrderBy(x => x.Key == ModKind.Script ? 9 : (int)x.Key))
        {
            var items = group.Take(GalleryCap).Select(m => new ThumbItem(m)).ToList();
            var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            head.Children.Add(new TextBlock { Text = ModRow.KindName(group.Key), FontSize = 16, FontWeight = FontWeights.SemiBold });
            head.Children.Add(Muted(group.Count() > GalleryCap ? T("{0} of {1}", GalleryCap, group.Count()) : group.Count().ToString()));
            ModGallery.Children.Add(head);
            var grid = new GridView { ItemTemplate = (DataTemplate)Root.Resources["ThumbTemplate"], SelectionMode = ListViewSelectionMode.None, ItemsSource = items };
            ModGallery.Children.Add(grid);
            _ = LoadThumbsAsync(items);
        }
        if (ModGallery.Children.Count == 0) ModGallery.Children.Add(Muted(T("No packages match.")));
    }

    static async Task LoadThumbsAsync(List<ThumbItem> items)
    {
        foreach (var it in items)
        {
            var bytes = await Task.Run(() => Mods.Thumbnail(it.File));
            it.Image = await SaveCard.ImageOf(bytes);
        }
    }

    void OnModSearch(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args) { if (_modsLoaded && !_fillingMods) FillMods(); }
    void OnModFilter(object sender, SelectionChangedEventArgs e) { if (_modsLoaded && !_fillingMods) FillMods(); }

    // a click on the sidebar item already highlighted (Settings while Repair or remove is open): back to that screen
    void OnNavInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
    {
        if (args.InvokedItemContainer?.Tag is not string tag || !int.TryParse(tag, out var to)) return;
        DispatcherQueue.TryEnqueue(() => { if (_step != to && !_busy && to >= Home) Show(to); });
    }
    void OnModsView(SelectorBar sender, SelectorBarSelectionChangedEventArgs args) { if (_modsLoaded) FillMods(); }
    void OnModsOpenFolder(object sender, RoutedEventArgs e) => OpenFolder(Path.Combine(Docs, "Mods"));

    // ---------- Worlds ----------

    void BuildWorlds()
    {
        var host = ScreenWorlds;
        host.Children.Clear();
        var head = (StackPanel)Col(Heading(T("Worlds")));
        if (_game is not { } g) { host.Children.Add(Muted(T("Choose the game first."))); return; }
        var worlds = Worlds.List(g, Docs);
        head.Children.Add(Muted(T("{0} worlds you can start a new game in", worlds.Count)));
        host.Children.Add(head);
        foreach (var (kind, label) in new[] { (WorldKind.BaseGame, T("The Sims 3")), (WorldKind.Expansion, T("From your expansions")), (WorldKind.Store, T("From the store")), (WorldKind.Custom, T("Custom worlds")) })
        {
            var list = worlds.Where(w => w.Kind == kind).ToList();
            if (list.Count == 0) continue;
            host.Children.Add(new TextBlock { Text = label, FontSize = 15, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 8, 0, 0) });
            var wrap = new VariableSizedWrapGrid { Orientation = Orientation.Horizontal, ItemWidth = 248, ItemHeight = 236 };
            var installed = Expansions.Installed(g);
            foreach (var w in list)
            {
                var card = new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(0), Margin = new Thickness(0, 0, 16, 16), CornerRadius = new CornerRadius(10) };
                var col = new StackPanel();
                var pic = new Grid { Height = 150, Background = Res("ApexPillBrush"), CornerRadius = new CornerRadius(10, 10, 0, 0) };
                var world = w;
                var packOf = w.Pack is { } wp ? Expansions.All.First(p => p.Short == wp) : null;
                _ = Task.Run(() => Worlds.Picture(world)).ContinueWith(async t =>
                {
                    if (await SaveCard.ImageOf(t.Result) is { } img) pic.Children.Add(new Image { Source = img, Stretch = Stretch.UniformToFill });
                    // a world without its own picture: its pack's icon, large, never a lot's picture
                    else if (packOf is not null) { var big = PackBadge(packOf, installed.FirstOrDefault(x => x.Pack == packOf).Dir, 72); big.HorizontalAlignment = HorizontalAlignment.Center; big.VerticalAlignment = VerticalAlignment.Center; pic.Children.Add(big); }
                    else pic.Children.Add(new FontIcon { Glyph = "\uE909", FontSize = 30, Foreground = new SolidColorBrush(Rgb(0x4A, 0x4B, 0x55)) });
                }, TaskScheduler.FromCurrentSynchronizationContext());
                col.Children.Add(pic);
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Padding = new Thickness(14, 10, 14, 12) };
                var pack = w.Pack is { } sh ? Expansions.All.First(p => p.Short == sh) : null;
                if (pack is not null) row.Children.Add(PackBadge(pack, installed.FirstOrDefault(x => x.Pack == pack).Dir, 28));
                row.Children.Add(Col(Strong(w.Name), Muted(pack?.Name ?? label)));
                col.Children.Add(row);
                card.Child = col;
                wrap.Children.Add(card);
            }
            host.Children.Add(wrap);
        }
        host.Children.Add(Muted(T("Custom worlds go in Documents\\Electronic Arts\\The Sims 3\\InstalledWorlds; removing one never touches saves made in it.")));
    }

    // ---------- Saves ----------

    void FillSaves()
    {
        if (_game is null) { SavesEmpty.Text = T("Choose the game first."); SavesEmpty.Visibility = Visibility.Visible; return; }
        var docs = Docs;
        var cards = Saves.List(docs).Select(s => new SaveCard(s)).ToList();
        SaveGrid.ItemsSource = cards;
        SavesEmpty.Text = T("No saves yet: they show here once you save a game.");
        SavesEmpty.Visibility = cards.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        SavesBackupAll.IsEnabled = cards.Count > 0;
        SaveCard.Play = c => _ = PlayAsync(c.Save.Name);
        SaveCard.Backup = c => _ = BackupAsync([c.Save]);
        _ = Task.Run(async () =>
        {
            foreach (var c in cards)
            {
                var bytes = Saves.Photo(c.Save);
                DispatcherQueue.TryEnqueue(async () => c.Photo = await SaveCard.ImageOf(bytes));
            }
            await Task.CompletedTask;
        });
    }

    async Task BackupAsync(List<Save> saves)
    {
        var docs = Docs;
        try
        {
            var n = await Task.Run(() => saves.Select(s => Saves.Backup(s, docs)).Count());
            SetInfo(SavesInfo, InfoBarSeverity.Success, n == 1 ? T("Save backed up") : T("{0} saves backed up", n), Saves.BackupDir(docs), T("Open folder"), () => OpenFolder(Saves.BackupDir(docs)));
        }
        catch (Exception e) { App.Log(e); SetInfo(SavesInfo, InfoBarSeverity.Error, T("The backup did not finish"), ErrorText(e)); }
    }

    void OnBackupAll(object sender, RoutedEventArgs e) => _ = BackupAsync(Saves.List(Docs));

    // ---------- Health ----------

    void BuildHealth()
    {
        var host = ScreenHealth;
        host.Children.Clear();
        if (_game is null) { host.Children.Add(Heading(T("Health"))); host.Children.Add(Muted(T("Choose the game first."))); return; }
        if (_checked == default) CheckHealth();
        var top = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center, Spacing = 12 };
        _healthCrystal = new Crystal(170);
        _healthCrystal.SetState(_healthState, animate: false);
        _healthCrystal.HorizontalAlignment = HorizontalAlignment.Center;
        top.Children.Add(_healthCrystal);
        top.Children.Add(new TextBlock { Text = HealthTitle(), FontSize = 28, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center });
        top.Children.Add(new TextBlock
        {
            Text = _healthState switch
            {
                HealthState.Blocked => T("The game still starts, but without Apex Radiance until this is fixed."),
                HealthState.NeedsYou => T("The game starts fine. Have a look when you have a minute."),
                _ => T("Every check passed. The launcher checks again each time you press Play."),
            },
            Foreground = Res("ApexSoftTextBrush"), FontSize = 14, HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap, MaxWidth = 520,
        });
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
        var check = new Button { Content = Icon("", T("Check now")), CornerRadius = new CornerRadius(18), Padding = new Thickness(16, 7, 18, 7) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(check, "HealthCheckNow");
        check.Click += (_, _) => { _modsLoaded = false; RefreshGame(); CheckHealth(); BuildHealth(); };
        var copy = new Button { Content = new FontIcon { Glyph = "", FontSize = 14 }, CornerRadius = new CornerRadius(18), Padding = new Thickness(11, 9, 11, 9) };
        ToolTipService.SetToolTip(copy, T("Copy report"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(copy, T("Copy report"));
        copy.Click += (_, _) => CopyReport();
        actions.Children.Add(check); actions.Children.Add(copy);
        top.Children.Add(actions);
        top.Children.Add(new TextBlock { Text = T("Last checked {0}", _checked.ToString("t")), Style = (Style)Application.Current.Resources["ApexMuted"], HorizontalAlignment = HorizontalAlignment.Center });
        // a soft glow of the state's colour behind the crystal, fading down the page
        host.Children.Add(new Border
        {
            Child = top, CornerRadius = new CornerRadius(18), Padding = new Thickness(0, 18, 0, 18),
            Background = new LinearGradientBrush
            {
                StartPoint = new Windows.Foundation.Point(0.5, 0), EndPoint = new Windows.Foundation.Point(0.5, 1),
                GradientStops = { new GradientStop { Color = StateTint(12), Offset = 0 }, new GradientStop { Color = StateTint(0), Offset = 0.85 } },
            },
        });

        var tabs = new SelectorBar { HorizontalAlignment = HorizontalAlignment.Center };
        string[] names = [T("Overview"), T("Automatic fixes"), T("Restore points"), T("Last session")];
        foreach (var n in names) tabs.Items.Add(new SelectorBarItem { Text = n });
        tabs.SelectedItem = tabs.Items[_healthTab];
        tabs.SelectionChanged += (s, _) => { var i = s.Items.IndexOf(s.SelectedItem); if (i < 0 || i == _healthTab) return; _healthTab = i; DispatcherQueue.TryEnqueue(BuildHealth); };
        host.Children.Add(tabs);
        var body = new StackPanel { Spacing = 14, MaxWidth = 760, HorizontalAlignment = HorizontalAlignment.Stretch };
        host.Children.Add(body);
        switch (_healthTab)
        {
            case 0: HealthOverview(body); HealthRepair(body); break;
            case 1: HealthFixes(body); break;
            case 2: HealthRestore(body); break;
            default: HealthSession(body); break;
        }

    }

    void HealthOverview(StackPanel body)
    {
        var needs = _health.Where(i => i.Finding.Severity != Severity.Info).ToList();
        if (needs.Count > 0)
        {
            body.Children.Add(Eyebrow(T("NEEDS YOU")));
            var list = new StackPanel();
            foreach (var it in needs) list.Children.Add(FindingRow(it, list.Children.Count > 0));
            body.Children.Add(Card(list, pad: false));
        }
        body.Children.Add(Eyebrow(T("CHECKS")));
        var areas = new StackPanel();
        (HealthArea Area, string Name, string Detail, string Glyph)[] all =
        [
            (HealthArea.ApexRadiance, "Apex Radiance", T("files, settings, one copy"), ""),
            (HealthArea.Loaders, T("Loaders"), T("ASI loader, Sims 3 Modernization"), ""),
            (HealthArea.Graphics, T("Graphics"), _state?.Layer.ToString() ?? "", ""),
            (HealthArea.GameSettings, T("Game settings"), "Options.ini, Resource.cfg", ""),
            (HealthArea.Mods, T("Mods"), T("conflicts and what they need"), ""),
        ];
        foreach (var (area, name, detail, glyph) in all)
        {
            var items = _health.Where(i => i.Area == area).ToList();
            var bad = items.Count(i => i.Finding.Severity != Severity.Info);
            var exp = new Expander { HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, IsExpanded = _openArea.Contains((int)area) };
            exp.Expanding += (_, _) => _openArea.Add((int)area);
            exp.Collapsed += (_, _) => _openArea.Remove((int)area);
            var head = new Grid { ColumnSpacing = 14 };
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            head.ColumnDefinitions.Add(new ColumnDefinition());
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            head.Children.Add(new Border { Width = 32, Height = 32, CornerRadius = new CornerRadius(9), Background = Res("ApexPillBrush"), Child = new FontIcon { Glyph = glyph, FontSize = 15, Foreground = Res("ApexSoftTextBrush") } });
            var t = Col(new TextBlock { Text = name, FontSize = 14 }, Muted(detail));
            Grid.SetColumn((FrameworkElement)t, 1);
            head.Children.Add(t);
            var st = new TextBlock { Text = bad == 0 ? T("All good") : bad == 1 ? T("1 to look at") : T("{0} to look at", bad), FontSize = 12, VerticalAlignment = VerticalAlignment.Center,
                Foreground = Res(bad == 0 ? "ApexSuccessBrush" : items.Any(i => i.Finding.Severity == Severity.Error) ? "ApexErrorBrush" : "ApexWarningBrush") };
            Grid.SetColumn(st, 2);
            head.Children.Add(st);
            exp.Header = head;
            var content = new StackPanel { Spacing = 6 };
            if (items.Count == 0) content.Children.Add(Muted(T("Nothing to report.")));
            foreach (var it in items) content.Children.Add(Body(T(it.Finding.Text, it.Finding.Args)));
            exp.Content = content;
            areas.Children.Add(exp);
        }
        body.Children.Add(areas);
    }

    // Repair: every file the launcher installed copied again, the settings fixed; the other choices in Repair or remove
    void HealthRepair(StackPanel body)
    {
        body.Children.Add(Eyebrow(T("REPAIR THE GAME")));
        var g = new Grid { ColumnSpacing = 16 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new Border { Width = 40, Height = 40, CornerRadius = new CornerRadius(10), Background = Res("ApexPillBrush"), VerticalAlignment = VerticalAlignment.Center,
            Child = new FontIcon { Glyph = "\uE90F", FontSize = 18, Foreground = Res("ApexAccentBrush") } });
        var text = Col(new TextBlock { Text = T("Something not right in the game?"), FontSize = 14, FontWeight = FontWeights.SemiBold },
            Muted(T("Repair copies every file the launcher installed again and fixes the game's settings. Your saves, mods and Apex Radiance settings stay.")));
        Grid.SetColumn((FrameworkElement)text, 1);
        g.Children.Add(text);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        var repair = Button(T("Repair"), async () => await Repair(), accent: true);
        repair.IsEnabled = !_running && !_busy && _state?.ApexVersion is not null;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(repair, "HealthRepair");
        buttons.Children.Add(repair);
        buttons.Children.Add(Button(T("More options"), () => Show(Manage)));
        Grid.SetColumn(buttons, 2);
        g.Children.Add(buttons);
        body.Children.Add(Card(g));
    }

    UIElement FindingRow(Health.Item it, bool line)
    {
        var f = it.Finding;
        var g = new Grid { ColumnSpacing = 14, Padding = new Thickness(18, 14, 18, 14), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, line ? 1 : 0, 0, 0) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var error = f.Severity == Severity.Error;
        g.Children.Add(new Border { Width = 10, Height = 10, CornerRadius = new CornerRadius(5), VerticalAlignment = VerticalAlignment.Center, Background = Res(error ? "ApexErrorBrush" : "ApexWarningBrush") });
        var text = Body(T(f.Text, f.Args));
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        Button? act = null;
        if (f.CanFix) act = Button(T("Fix"), async () => { if (await Guarded(() => { Engine.CheckWritable(); f.Fix!(); }, T("Fixed."), null, "Fix incompatible settings")) { RefreshGame(); CheckHealth(); BuildHealth(); } }, accent: true);
        else if (it.Files.Count > 0) act = Button(T("Show in Mods"), () => { _modKind = ""; _modType = ""; Show(ModsScreen); ModState.SelectedIndex = 3; });
        else if (f.Link is { } link) act = Button(T("Read more"), () => Process.Start(new ProcessStartInfo(link) { UseShellExecute = true }));
        if (act is not null) { Grid.SetColumn(act, 2); act.VerticalAlignment = VerticalAlignment.Center; g.Children.Add(act); }
        return g;
    }

    void HealthFixes(StackPanel body)
    {
        body.Children.Add(Muted(T("Safe fixes run on their own before the game starts. A copy of each file is kept, so every fix can be undone.")));
        var fixes = _game is { } g ? RestorePoints.List(g.Bin).Where(p => p.Label.StartsWith("Fix", StringComparison.Ordinal)).ToList() : [];
        var waiting = _health.Where(i => i.Finding.CanFix).ToList();
        var list = new StackPanel();
        foreach (var it in waiting) list.Children.Add(FindingRow(it, list.Children.Count > 0));
        foreach (var p in fixes) list.Children.Add(PointRow(p, list.Children.Count > 0, T("Undo")));
        if (list.Children.Count == 0) list.Children.Add(new TextBlock { Text = T("Nothing was fixed yet."), Margin = new Thickness(18, 14, 18, 14), Foreground = Res("ApexMutedBrush") });
        body.Children.Add(Card(list, pad: false));
    }

    void HealthRestore(StackPanel body)
    {
        body.Children.Add(Muted(T("Going back undoes that point and every later one.")));
        var list = new StackPanel();
        if (_game is { } g)
            foreach (var p in RestorePoints.List(g.Bin)) list.Children.Add(PointRow(p, list.Children.Count > 0, T("Go back")));
        if (list.Children.Count == 0) list.Children.Add(new TextBlock { Text = T("No restore points yet."), Margin = new Thickness(18, 14, 18, 14), Foreground = Res("ApexMutedBrush") });
        body.Children.Add(Card(list, pad: false));
    }

    UIElement PointRow(RestorePoint p, bool line, string action)
    {
        var g = new Grid { ColumnSpacing = 14, Padding = new Thickness(18, 12, 18, 12), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, line ? 1 : 0, 0, 0) };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(Col(new TextBlock { Text = T(p.Label, p.Args), FontSize = 14 }, Muted(p.Created.ToLocalTime().ToString("g"))));
        var b = Button(action, async () => { await RestoreAsync(p); CheckHealth(); BuildHealth(); });
        b.IsEnabled = !_running && !_busy;
        Grid.SetColumn(b, 1);
        b.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(b);
        return g;
    }

    void HealthSession(StackPanel body)
    {
        if (_lastSession is not { } s) { body.Children.Add(Card(Muted(T("Play once through the launcher and the session shows here.")))); return; }
        var grid = new Grid { ColumnSpacing = 12 };
        (string, string, string)[] stats =
        [
            (T("Played"), Duration(s.Played), "ApexSoftTextBrush"),
            (T("Ended"), s.End switch { SessionEnd.Crash => T("In a crash"), SessionEnd.LowMemory => T("Low on memory"), _ => T("Normally") }, s.End == SessionEnd.Normal ? "ApexSuccessBrush" : "ApexWarningBrush"),
            (T("Peak memory"), $"{s.PeakBytes >> 20} MB", "ApexSoftTextBrush"),
        ];
        for (int i = 0; i < stats.Length; i++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            var c = Card(Col(Muted(stats[i].Item1), new TextBlock { Text = stats[i].Item2, FontSize = 20, FontWeight = FontWeights.SemiBold, Foreground = Res(stats[i].Item3) }));
            Grid.SetColumn(c, i);
            grid.Children.Add(c);
        }
        body.Children.Add(grid);
    }

    void CopyReport()
    {
        var lines = new List<string> { App.Details(), "", "Health: " + _healthState };
        lines.AddRange(_health.Select(i => $"[{i.Finding.Severity}] {i.Area}: {string.Format(i.Finding.Text, i.Finding.Args)}"));
        var dp = new Windows.ApplicationModel.DataTransfer.DataPackage();
        dp.SetText(string.Join("\r\n", lines));
        Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(dp);
    }

    // ---------- What's new ----------

    void BuildNews()
    {
        var host = ScreenNews;
        host.Children.Clear();
        host.Children.Add(Col(Heading(T("What's new")), Muted(T("Release notes, read from each project's GitHub releases."))));
        var tabs = new SelectorBar();
        foreach (var n in new[] { "Apex Radiance", "Sims 3 Modernization" }) tabs.Items.Add(Tab(n, tabs.Items.Count == 0));
        tabs.SelectedItem = tabs.Items[_newsTab];
        tabs.SelectionChanged += (s, _) => { var i = s.Items.IndexOf(s.SelectedItem); if (i < 0 || i == _newsTab) return; _newsTab = i; DispatcherQueue.TryEnqueue(BuildNews); };
        host.Children.Add(tabs);
        var r = _newsTab == 0 ? _latest.GetValueOrDefault(Releases.Apex) : ModRelease() ?? _latest.GetValueOrDefault(Releases.Modernization);
        if (r is null) { host.Children.Add(Card(Muted(_offline ? T("No internet: the notes show once the launcher can reach GitHub.") : T("Checking the latest versions…")))); return; }
        var installed = _newsTab == 0 ? _state?.ApexVersion : Known(Components.Modernization);
        var notes = new StackPanel { Spacing = 8 };
        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        head.Children.Add(new TextBlock { Text = r.Version, FontSize = 20, FontWeight = FontWeights.SemiBold });
        head.Children.Add(Pill(installed == r.Version ? T("Installed") : installed is null ? T("Latest") : T("Newer than yours"), installed == r.Version ? "ApexSuccessBrush" : "ApexAccentBrush"));
        notes.Children.Add(head);
        foreach (var raw in (r.Body ?? T("This release has no notes.")).Replace("\r", "").Split('\n'))
        {
            var line = raw.TrimEnd();
            if (line.Length == 0) continue;
            if (line.StartsWith('#')) notes.Children.Add(new TextBlock { Text = line.TrimStart('#', ' '), FontSize = 15, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 8, 0, 0), TextWrapping = TextWrapping.Wrap });
            else if (line.TrimStart().StartsWith("- ") || line.TrimStart().StartsWith("* ")) notes.Children.Add(Body("• " + Plain(line.TrimStart()[2..])));
            else notes.Children.Add(Body(Plain(line)));
        }
        host.Children.Add(Card(notes));
        host.Children.Add(Link(T("Open on GitHub"), () => Process.Start(new ProcessStartInfo($"https://github.com/{r.Repo}/releases/tag/{r.Tag}") { UseShellExecute = true })));
    }

    static string Plain(string md) => md.Replace("**", "").Replace("`", "");
    static string? FirstLine(string? md) => md?.Replace("\r", "").Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0 && !l.StartsWith('#')) is { } l ? Plain(l) : null;

    // ---------- Settings ----------

    void BuildSettings()
    {
        var host = ScreenSettings;
        host.Children.Clear();
        host.Children.Add(Heading(T("Settings")));
        host.Children.Add(Section(T("Starting the game")));
        var installed = Environment.ProcessPath is { } exe && LauncherInstall.IsInstalledCopy(exe);
        host.Children.Add(Toggle(T("Start with Windows"), T("Opens quietly in the notification area, so updates are ready before you play."), LauncherInstall.RunsAtStartup(),
            on => LauncherInstall.SetRunAtStartup(Paths.InstalledExe, on), enabled: installed));
        host.Children.Add(Toggle(T("Hide while playing"), T("The launcher goes to the notification area while the game runs and comes back when it closes."), _settings.HideWhilePlaying,
            on => { _settings.HideWhilePlaying = on; _settings.Save(); }));
        host.Children.Add(Toggle(T("Fix safe things without asking"), T("Like the Resource.cfg and Edge Smoothing fixes. A copy of each file is always kept."), _settings.FixSafeAutomatically,
            on => { _settings.FixSafeAutomatically = on; _settings.Save(); }));
        host.Children.Add(Section(T("Updates")));
        host.Children.Add(Toggle(T("Install updates when I press Play"), T("Otherwise the launcher asks first."), _settings.InstallUpdatesOnPlay,
            on => { _settings.InstallUpdatesOnPlay = on; _settings.Save(); }));
        host.Children.Add(Row(T("Apex Launcher {0}", App.Version), T("Updates itself from each Apex Radiance release."), null));
        host.Children.Add(Section(T("General")));
        host.Children.Add(Row(T("Language"), Languages.First(l => l.Code == Lang).Name, Button(T("Change"), () => OnLanguage(LangButton, new RoutedEventArgs()))));
        host.Children.Add(Row(T("Game folder"), _game?.Root ?? T("No game chosen"), Id(Button(T("Change"), () => Show(GameStep)), "SettingsGame")));
        host.Children.Add(Row(T("Logs"), T("What to attach when you ask for help."), Button(T("Open logs folder"), () => OpenFolder(App.LogDir))));
        host.Children.Add(Row(T("Repair or remove"), T("Repair the files, remove a component, or go back to a restore point."), Id(Button(T("Open"), () => Show(Manage)), "SettingsManage")));
        host.Children.Add(Section(T("Uninstall")));
        host.Children.Add(Row(T("Uninstall Apex Launcher"), T("Removes Apex Radiance and everything the launcher installed, puts back every file it replaced, then the launcher itself. Your saves and your own mods stay."),
            Id(Button(T("Uninstall…"), async () => await UninstallAsync(), danger: true), "SettingsUninstall")));
        host.Children.Add(Section(T("About and licences")));
        host.Children.Add(Row(T("Credits and licences"), T("Apex Radiance by loinyx, and the authors of every component the launcher installs."), Id(Button(T("Open"), () => Show(About)), "SettingsAbout")));
    }

    // ---------- uninstall ----------

    // Settings' Uninstall and Windows' Installed apps (--uninstall)
    public async Task UninstallAsync(bool fromWindows = false)
    {
        if (Games.IsRunning()) { await Message(T("Close the game first"), T("Close The Sims 3, then uninstall.")); return; }
        var keep = new CheckBox { Content = T("Keep my Apex Radiance settings"), IsChecked = true };
        var text = new StackPanel { Spacing = 12 };
        text.Children.Add(Body(T("Apex Radiance and every component the launcher installed are removed, and every file they replaced is put back. Mods you switched off are put back on. Your saves and your own mods stay.")));
        text.Children.Add(keep);
        var ok = await ShowDialog(new ContentDialog { Title = T("Uninstall Apex Launcher"), Content = text, PrimaryButtonText = T("Uninstall"), CloseButtonText = T("Cancel"),
            DefaultButton = ContentDialogButton.Close, XamlRoot = Content.XamlRoot }) == ContentDialogResult.Primary;
        if (!ok) { if (fromWindows) Application.Current.Exit(); return; }
        _busy = true;
        var keepSettings = keep.IsChecked == true;
        var left = new List<string>();
        try
        {
            var games = await Task.Run(Uninstall.Games);
            var needAdmin = !Elevation.IsElevated && games.Any(g => !Elevation.CanWrite(g.Bin));
            if (needAdmin && Environment.ProcessPath is { } exe)
            {
                var p = Elevation.StartMaintenance(exe, "*", keepSettings ? "uninstall-keep" : "uninstall");
                if (p is null) { _busy = false; await Message(T("Nothing was changed"), T("Windows did not allow the change.")); return; }
                await p.WaitForExitAsync();
                if (p.ExitCode != 0) left.Add(T("Some files could not be put back: see the log."));
            }
            else left.AddRange(await Task.Run(() => games.SelectMany(g => Uninstall.FromGame(g, Scan.DocumentsFor(g), keepSettings)).ToList()));
        }
        catch (Exception e) { App.Log(e); left.Add(ErrorText(e)); }
        Uninstall.Launcher();
        _busy = false;
        await Message(T("Apex Launcher was uninstalled"), left.Count == 0 ? T("Everything it installed is gone, and the files it replaced are back.") : T("Some things stayed: {0}", string.Join("; ", left)));
        Application.Current.Exit();
    }

    // Start with Windows: only the notification area icon until the player opens the window
    public void StartInTray() => TrayShow(T("Apex Launcher"));

    // Installed apps' Uninstall: the window opens on the uninstall question
    public void UninstallWhenShown() => ((FrameworkElement)Content).Loaded += async (_, _) => await UninstallAsync(fromWindows: true);

    // the administrator copy's uninstall (no window): every game, then the machine-wide markers
    public static int UninstallElevated(bool keepSettings)
    {
        var failed = false;
        foreach (var g in Uninstall.Games())
        {
            try { if (Uninstall.FromGame(g, Scan.DocumentsFor(g), keepSettings).Count > 0) failed = true; }
            catch (Exception e) { App.Log(e); failed = true; }
        }
        Uninstall.MachineData();
        return failed ? 1 : 0;
    }

    // ---------- small builders ----------

    // a tab of a bar under a title: the first one without the bar's inner left space, so its text starts on the title's edge
    static SelectorBarItem Tab(string text, bool first)
    {
        var t = new SelectorBarItem { Text = text };
        if (first) t.Margin = new Thickness(-12, 0, 0, 0);
        return t;
    }

    static TextBlock Heading(string t) => new() { Text = t, Style = (Style)Application.Current.Resources["ApexTitle"] };
    static TextBlock Strong(string t) => new() { Text = t, FontSize = 16, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
    static TextBlock Body(string t) => new() { Text = t, Style = (Style)Application.Current.Resources["ApexBody"], Foreground = Res("ApexSoftTextBrush") };
    static TextBlock Muted(string t) => new() { Text = t, Style = (Style)Application.Current.Resources["ApexMuted"] };
    static TextBlock Eyebrow(string t) => new() { Text = t, FontSize = 11, CharacterSpacing = 80, Foreground = Res("ApexMutedBrush") };
    static TextBlock Section(string t) => new() { Text = t, FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexSoftTextBrush"), Margin = new Thickness(0, 14, 0, 2) };
    static UIElement Col(params UIElement[] items) { var s = new StackPanel { Spacing = 4 }; foreach (var i in items) s.Children.Add(i); return s; }
    static Border Card(UIElement child, bool pad = true) => new() { Style = (Style)Application.Current.Resources["ApexCard"], Child = child, Padding = pad ? new Thickness(18, 14, 18, 14) : new Thickness(0) };
    static Border Card(UIElement a, UIElement b) => Card(Col(a, b));

    static StackPanel Icon(string glyph, string text)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        s.Children.Add(new FontIcon { Glyph = glyph, FontSize = 14 });
        s.Children.Add(new TextBlock { Text = text });
        return s;
    }

    static Button Button(string text, Action click, bool accent = false, bool danger = false)
    {
        var b = new Button { Content = text };
        if (accent) b.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        if (danger) b.Foreground = Res("ApexErrorBrush");
        b.Click += (_, _) => click();
        return b;
    }

    static Button Button(string text, Func<Task> click, bool accent = false, bool danger = false)
    {
        var b = Button(text, () => { }, accent, danger);
        b.Click += async (_, _) => await click();
        return b;
    }

    // an automation id for UI tests (ui-smoke finds Settings' row buttons by it)
    static Button Id(Button b, string id) { Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(b, id); return b; }

    static HyperlinkButton Link(string text, Action click)
    {
        var l = new HyperlinkButton { Content = text, Padding = new Thickness(0) };
        l.Click += (_, _) => click();
        return l;
    }

    static Border Pill(string text, string brush) => new()
    {
        Style = (Style)Application.Current.Resources["ApexPill"],
        Child = new TextBlock { Text = text, FontSize = 11, Foreground = Res(brush) },
    };

    static Button Chip(string text, string dot, Action click)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        s.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 6, Height = 6, Fill = Res(dot), VerticalAlignment = VerticalAlignment.Center });
        s.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexSoftTextBrush") });
        var b = new Button { Content = s, CornerRadius = new CornerRadius(12), Padding = new Thickness(10, 3, 10, 4), Background = Res("ApexPillBrush"), BorderThickness = new Thickness(0) };
        b.Click += (_, _) => click();
        return b;
    }

    InfoBar Banner(InfoBarSeverity sev, string title, string text, string? action = null, Action? run = null)
    {
        var bar = new InfoBar { IsClosable = false };
        SetInfo(bar, sev, title, text, action, run);
        return bar;
    }

    static Border Row(string title, string detail, UIElement? action)
    {
        var g = new Grid { ColumnSpacing = 16 };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(Col(new TextBlock { Text = title, FontSize = 14 }, Muted(detail)));
        if (action is FrameworkElement a) { Grid.SetColumn(a, 1); a.VerticalAlignment = VerticalAlignment.Center; g.Children.Add(a); }
        return Card(g);
    }

    static Border Toggle(string title, string detail, bool on, Action<bool> set, bool enabled = true)
    {
        var t = new ToggleSwitch { IsOn = on, OnContent = "", OffContent = "", MinWidth = 0, IsEnabled = enabled };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(t, title);
        t.Toggled += (_, _) => { try { set(t.IsOn); } catch (Exception e) { App.Log(e); } };
        return Row(title, detail, t);
    }


    static void OpenFolder(string dir)
    {
        try { Directory.CreateDirectory(dir); Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true }); }
        catch (Exception e) { App.Log(e); }
    }
}
