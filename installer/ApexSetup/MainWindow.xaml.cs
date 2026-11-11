using System.Diagnostics;
using System.Reflection;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Graphics;
using static ApexSetup.Strings;

namespace ApexSetup;

public sealed class GameItem
{
    public Game Game { get; init; } = null!;
    public string Name { get; set; } = "";
    public string Path { get; set; } = "";
    public string Badge { get; set; } = "";
}

public sealed partial class MainWindow : Window
{
    const int Welcome = 0, GameStep = 1, ComponentsStep = 2, InstallStep = 3, DoneStep = 4, Manage = 5, About = 6;
    readonly UIElement[] _screens;
    readonly List<GameItem> _games = new();
    readonly Dictionary<string, Release?> _latest = new();
    readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(2) };
    int _step;
    Game? _game, _defaultsFor;
    State? _state;
    bool _offline, _busy, _syncNav, _running;
    List<string> _lastInstall = new();

    static readonly Dictionary<string, string> Repo = new()
    {
        [Components.Apex] = Releases.Apex, [Components.Bridge] = Releases.Apex, [Components.Loader] = Releases.AsiLoader, [Components.Dxvk] = Releases.Dxvk, [Components.MonoPatcher] = Releases.MonoPatcher,
        [Components.S3ss] = Releases.S3ss, [Components.Modernization] = Releases.Modernization,
    };

    public MainWindow()
    {
        InitializeComponent();
        _screens = new UIElement[] { ScreenWelcome, ScreenGame, ScreenComponents, ScreenInstall, ScreenDone, ScreenManage, ScreenAbout };
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(DragArea);
        void Size(double scale) => AppWindow.Resize(new SizeInt32((int)(980 * scale), (int)(680 * scale)));
        Size(GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0);
        ((FrameworkElement)Content).Loaded += (_, _) => Size(Content.XamlRoot.RasterizationScale); // the monitor's real scale
        if (AppWindow.Presenter is OverlappedPresenter p) { p.IsResizable = false; p.IsMaximizable = false; }
        var ico = System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "apex.ico");
        if (File.Exists(ico)) AppWindow.SetIcon(ico);
        SetupVersion.Text = "Setup " + Assembly.GetExecutingAssembly().GetName().Version?.ToString(3);

        Pt = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "pt";
        _timer.Tick += (_, _) => { var r = Games.IsRunning(); if (r != _running) { _running = r; RefreshGame(); UpdateFooter(); } };
        ((FrameworkElement)Content).Loaded += async (_, _) => await StartAsync();
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern uint GetDpiForWindow(nint hwnd);

    async Task StartAsync()
    {
        _games.AddRange(Games.Detect().Select(Item));
        // the folders picked before (Browse), and the game used last first: an update opens on the game it updates
        var remembered = Games.Remembered();
        foreach (var r in remembered)
            if (!_games.Any(x => x.Game.Bin.Equals(r.Bin, StringComparison.OrdinalIgnoreCase))) _games.Add(Item(r));
        if (remembered.FirstOrDefault() is { } last && _games.FindIndex(x => x.Game.Bin.Equals(last.Bin, StringComparison.OrdinalIgnoreCase)) is > 0 and var at)
        {
            var item = _games[at];
            _games.RemoveAt(at);
            _games.Insert(0, item);
        }
        // a game folder on the command line (or a shortcut) comes first
        if (Environment.GetCommandLineArgs().Skip(1).FirstOrDefault() is { } arg && Games.FromFolder(arg) is { } given
            && !_games.Any(x => x.Game.Bin.Equals(given.Bin, StringComparison.OrdinalIgnoreCase)))
            _games.Insert(0, Item(given));
        GameList.ItemsSource = _games;
        if (_games.Count > 0) GameList.SelectedIndex = 0;
        NoGames.Visibility = _games.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        _running = Games.IsRunning();
        _timer.Start();
        Localize();
        Show(Welcome);
        await LoadReleasesAsync();
    }

    async Task LoadReleasesAsync()
    {
        _offline = false;
        try
        {
            var repos = Repo.Values.Distinct().ToList();
            var got = await Task.WhenAll(repos.Select(r => Releases.LatestAsync(r)));
            for (int i = 0; i < repos.Count; i++) _latest[repos[i]] = got[i];
        }
        catch (Exception) { _offline = true; }
        RefreshAll();
    }

    GameItem Item(Game g) => new()
    {
        Game = g, Path = g.Root,
        Name = "The Sims 3 · " + g.Label,
        Badge = g.Version + " · " + g.Exe,
    };

    string? Latest(string c) => Repo.TryGetValue(c, out var repo) ? _latest.GetValueOrDefault(repo)?.Version : null;

    // The installed version: Setup's own record, else the latest release's when the file is identical to it (GitHub's SHA-256
    // digest), else "" (installed, version unknown), or null when absent.
    string? Known(string c)
    {
        if (_state is not { } s || _game is not { } g) return null;
        var (present, file, asset) = c switch
        {
            Components.S3ss => (s.S3ss, System.IO.Path.Combine(g.Bin, "Sims3SettingsSetter.asi"), "Sims3SettingsSetter.asi"),
            Components.Modernization => (s.Modernization, System.IO.Path.Combine(Scan.Packages(Scan.DocumentsFor(g)), "ts3fixes.package"), "ts3fixes.package"),
            Components.Bridge => (s.Layer == Layer.Bridge, "", ""),
            Components.Dxvk => (s.Layer == Layer.Dxvk, "", ""),
            Components.MonoPatcher => (s.MonoPatcher, "", ""),
            _ => (s.ApexVersion is not null, "", ""),
        };
        if (!present) return null;
        if (c == Components.Apex) return s.ApexVersion == "?" ? "" : s.ApexVersion;
        if (s.Installed(c) is { } v) return v;
        var rel = _latest.GetValueOrDefault(Repo[c]);
        var digest = rel?.Assets.FirstOrDefault(a => a.Name == asset)?.Sha256;
        return digest is not null && File.Exists(file) && Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(file))).Equals(digest, StringComparison.OrdinalIgnoreCase)
            ? rel!.Version : "";
    }

    // installed and known to be older than the latest release; a copy of unknown version stays as it is (Repair copies it again)
    bool NeedsUpdate(string c) => Known(c) is { Length: > 0 } v && Newer(Latest(c), v);

    static bool Newer(string? latest, string? installed)
    {
        static Version? V(string? s) => s is null ? null : Version.TryParse(s.Split('-', '+')[0], out var v) ? v : null;
        if (installed is null || V(latest) is not { } l) return false;
        return V(installed) is not { } i || l > i; // a copy without a version is older than any release
    }

    // ---------- navigation ----------

    void Show(int step)
    {
        _step = step;
        for (int i = 0; i < _screens.Length; i++) _screens[i].Visibility = i == step ? Visibility.Visible : Visibility.Collapsed;
        Animate(_screens[step]);
        Scroller.ChangeView(null, 0, null, true);
        _syncNav = true;
        Nav.SelectedItem = Nav.MenuItems.Concat(Nav.FooterMenuItems).OfType<NavigationViewItem>().First(n => (string)n.Tag == step.ToString());
        _syncNav = false;
        RefreshAll();
    }

    static void Animate(UIElement el)
    {
        var t = new TranslateTransform { Y = 10 };
        el.RenderTransform = t;
        el.Opacity = 0;
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        var sb = new Storyboard();
        var fade = new DoubleAnimation { From = 0, To = 1, Duration = TimeSpan.FromMilliseconds(260), EasingFunction = ease };
        var move = new DoubleAnimation { From = 10, To = 0, Duration = TimeSpan.FromMilliseconds(320), EasingFunction = ease };
        Storyboard.SetTarget(fade, el); Storyboard.SetTargetProperty(fade, "Opacity");
        Storyboard.SetTarget(move, t); Storyboard.SetTargetProperty(move, "Y");
        sb.Children.Add(fade); sb.Children.Add(move);
        sb.Begin();
    }

    void OnNavSelection(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (_syncNav || args.SelectedItem is not NavigationViewItem item) return;
        var to = int.Parse((string)item.Tag);
        // steps open in order; Install and Done only through the buttons; nothing moves while Setup works
        bool ok = !_busy && (to is Welcome or About || to == GameStep || (to == ComponentsStep && CanLeaveGame()) || to == Manage);
        if (!ok) { DispatcherQueue.TryEnqueue(() => Show(_step)); return; }
        Show(to == Manage && _game is null ? GameStep : to);
    }

    bool CanLeaveGame() => _game is not null && !_running;

    void OnBack(object sender, RoutedEventArgs e) => Show(Math.Max(Welcome, _step - 1));

    async void OnNext(object sender, RoutedEventArgs e)
    {
        switch (_step)
        {
            case Welcome:
                if (WelcomeUpdates() is { Count: > 0 } ups) await InstallAsync(ups); // (an update: one click, no steps)
                else Show(GameStep);
                break;
            case GameStep: if (CanLeaveGame()) Show(ComponentsStep); break;
            case ComponentsStep:
            {
                var (add, del) = Plan();
                var fixes = _game is not null && _state is not null && Compat.Check(_game, _state).Any(x => x.CanFix);
                if (add.Count + del.Count > 0 || fixes) await InstallAsync(add, del); // (nothing to do: the button is off and says so)
                break;
            }
            case DoneStep: Close(); break;
        }
    }

    void UpdateFooter()
    {
        BackButton.Visibility = _step is GameStep or ComponentsStep ? Visibility.Visible : Visibility.Collapsed;
        NextButton.Visibility = _step is Welcome or GameStep or ComponentsStep or DoneStep ? Visibility.Visible : Visibility.Collapsed;
        var (wa, wd) = _step == ComponentsStep ? Plan() : (new List<string>(), new List<string>());
        var work = wa.Concat(wd).ToList();
        var fixes = _step == ComponentsStep && _game is not null && _state is not null && Compat.Check(_game, _state).Any(x => x.CanFix);
        NextText.Text = _step switch
        {
            Welcome => WelcomeUpdates().Count > 0 ? T("Update now") : _state?.ApexVersion is not null ? T("Next") : T("Get started"),
            ComponentsStep => work.Count == 0 ? (fixes ? T("Fix settings") : T("Nothing to apply")) : _state?.ApexVersion is null ? T("Install")
                : work.Count == 1 ? T("Apply 1 change") : T("Apply {0} changes", work.Count),
            DoneStep => T("Finish"),
            _ => T("Next"),
        };
        NextButton.IsEnabled = _step switch
        {
            GameStep => CanLeaveGame(),
            ComponentsStep => !_running && _game is not null && (work.Count > 0 ? !_offline : fixes),
            _ => true,
        };
        FooterNote.Text = _step switch
        {
            GameStep when _running => T("Close The Sims 3 to continue."),
            ComponentsStep when work.Count == 0 && !fixes => T("Everything is installed and up to date."),
            ComponentsStep when _offline && work.Count > 0 => T("Setup needs the internet to download the files."),
            ComponentsStep => T("Setup changes only what differs from your game now: {0}.", string.Join(", ", wa.Select(Name).Concat(wd.Select(c => T("remove {0}", Name(c)))))),
            _ => "",
        };
    }

    // ---------- refresh ----------

    void RefreshAll()
    {
        LangText.Text = Pt ? "Português" : "English";
        RefreshGame();
        RefreshWelcome();
        RefreshComponents();
        RefreshManage();
        RefreshAbout();
        UpdateFooter();
    }

    void RefreshWelcome()
    {
        var latest = Latest(Components.Apex);
        WelcomeTitle.Text = "Apex Radiance" + (latest is null ? "" : " " + latest);
        var inst = _game is null ? null : _state?.ApexVersion;
        if (_offline)
            SetInfo(WelcomeInfo, InfoBarSeverity.Error, T("Setup can't reach GitHub"), T("Check your internet connection, then try again."), T("Try again"), async () => await LoadReleasesAsync());
        else if (Updates() is { Count: > 0 } ups && _game is { } g)
        {
            // checked at every start: the player learns about a new release here, without visiting GitHub
            var lines = ups.Select(c => T("{0} {1} → {2}", Name(c), Known(c) is { Length: > 0 } v ? v : "?", Latest(c)!));
            SetInfo(WelcomeInfo, InfoBarSeverity.Informational, T("Updates available"),
                T("{0} in The Sims 3 · {1}.", string.Join(", ", lines), g.Label) + " " + T("Update now installs them; Game and Components hold every other change."));
        }
        else WelcomeInfo.IsOpen = false;
        RefreshStatus();
    }

    // A game that has Apex Radiance opens on its status: what is installed and whether it is up to date, and the checks of
    // "Check installation" (Verify: the install's own files; Compat: the settings the game's Compatibility page checks), with the
    // ways on: Repair, Change components, Repair or remove. A game without Apex keeps the first-install introduction.
    string? _checkedAt;
    void RefreshStatus()
    {
        var returning = _game is not null && _state?.ApexVersion is not null;
        WelcomeStatus.Visibility = returning ? Visibility.Visible : Visibility.Collapsed;
        WelcomeIntro.Visibility = returning ? Visibility.Collapsed : Visibility.Visible;
        StatusList.Children.Clear();
        if (!returning || _state is not { } s || _game is not { } g) return;
        var muted = (Brush)Application.Current.Resources["ApexMutedBrush"];
        var ok = (Brush)Application.Current.Resources["ApexSuccessBrush"];
        var accent = (Brush)Application.Current.Resources["ApexAccentBrush"];
        StatusList.Children.Add(new TextBlock { Text = T("The Sims 3 \u00b7 {0}", g.Label), Style = (Style)Application.Current.Resources["ApexStrong"] });
        StatusList.Children.Add(new TextBlock { Text = g.Root, FontSize = 12, Foreground = muted, TextWrapping = TextWrapping.Wrap });
        void Row(string name, string value, bool good)
        {
            var row = new Grid();
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(new TextBlock { Text = name, FontSize = 13 });
            var right = new TextBlock { Text = value, FontSize = 12, Foreground = good ? ok : accent };
            Grid.SetColumn(right, 1);
            row.Children.Add(right);
            StatusList.Children.Add(row);
        }
        string State(string c) => Known(c) is not { Length: > 0 } v ? T("Installed") : Newer(Latest(c), v) ? T("{0} \u2192 {1}", v, Latest(c)!) : T("{0} \u00b7 up to date", v);
        Row("Apex Radiance", State(Components.Apex), !Updates().Contains(Components.Apex));
        Row(T("Renderer"), LayerName(s.Layer), true);
        Row(T("ASI loader"), s.Loader ?? T("none"), s.Loader is not null);
        foreach (var (c, name, present) in new[] { (Components.S3ss, "Sims3SettingsSetter", s.S3ss), (Components.Modernization, "Sims 3 Modernization", s.Modernization), (Components.MonoPatcher, "MonoPatcher", s.MonoPatcher) })
            if (present) Row(name, State(c), !Updates().Contains(c));

        var found = Verify.Files(g, s, _latest.GetValueOrDefault(Repo[Components.Apex])).Concat(Compat.Check(g, s)).ToList();
        if (found.Count == 0)
            StatusList.Children.Add(new TextBlock { Text = _checkedAt is null ? T("Everything is in place.") : T("Everything is in place (checked at {0}).", _checkedAt), FontSize = 12, Foreground = ok });
        foreach (var x in found)
            StatusList.Children.Add(new InfoBar
            {
                IsOpen = true, IsClosable = false, Message = T(x.Text, x.Args), Title = x.CanFix ? T("Setup fixes this") : "",
                Severity = x.Severity switch { Severity.Error => InfoBarSeverity.Error, Severity.Warning => InfoBarSeverity.Warning, _ => InfoBarSeverity.Informational },
            });

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        Button Make(string text, Func<Task> act, bool primary = false)
        {
            var b = new Button { Content = text, IsEnabled = !_running && !_busy };
            if (primary) b.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
            b.Click += async (_, _) => await act();
            buttons.Children.Add(b);
            return b;
        }
        // the install's own files set right by installing them again; the settings by their fixes
        var repair = new List<string>();
        if (found.Any(x => x.Text.Contains("ApexRadiance.asi") || x.Text.StartsWith("Apex Radiance is not"))) repair.Add(Components.Apex);
        if (found.Any(x => x.Text.Contains("RTX Remix bridge does not start") && !x.CanFix)) repair.Add(Components.Bridge);
        if (s.Loader is null) repair.Add(Components.Loader);
        if (repair.Count > 0 || found.Any(x => x.CanFix))
            Make(T("Repair"), async () =>
            {
                if (found.Any(x => x.CanFix)) await Guarded(() => { Engine.CheckWritable(); foreach (var x in Compat.Check(g, Scan.Run(g)).Where(x => x.CanFix)) x.Fix!(); }, T("Fixed."));
                if (repair.Count > 0 && !_offline) await InstallAsync(repair);
            }, primary: true);
        Make(T("Check installation"), () =>
        {
            _checkedAt = DateTime.Now.ToString("HH:mm");
            RefreshAll(); // (scans the game again, then Verify and Compat on what is there now)
            return Task.CompletedTask;
        });
        Make(T("Change components"), () => { if (CanLeaveGame()) Show(ComponentsStep); return Task.CompletedTask; });
        Make(T("Repair or remove"), () => { Show(Manage); return Task.CompletedTask; });
        StatusList.Children.Add(buttons);
    }

    // the welcome page's one-click update: the installed components with a newer release, when Setup can download them now
    List<string> WelcomeUpdates() => _offline || _running || _game is null ? new List<string>() : Updates();

    // installed components with a newer release (only those: nothing new is added from the welcome page)
    List<string> Updates()
    {
        var l = new List<string>();
        if (_state is not { } s || _game is null) return l;
        if (s.ApexVersion is { } a && (a == "?" || Newer(Latest(Components.Apex), a))) l.Add(Components.Apex);
        if (s.Layer == Layer.Bridge && s.Installed(Components.Bridge) is { } b && Newer(Latest(Components.Bridge), b)) l.Add(Components.Bridge);
        if (s.Layer == Layer.Dxvk && s.Installed(Components.Dxvk) is { } d && Newer(Latest(Components.Dxvk), d)) l.Add(Components.Dxvk);
        foreach (var c in new[] { Components.S3ss, Components.Modernization, Components.MonoPatcher })
            if (Known(c) is { Length: > 0 } v && Newer(Latest(c), v)) l.Add(c);
        return l;
    }

    static void SetInfo(InfoBar bar, InfoBarSeverity sev, string title, string msg, string? action = null, Action? onAction = null)
    {
        bar.Severity = sev; bar.Title = title; bar.Message = msg; bar.IsOpen = true;
        if (action is null) { bar.ActionButton = null; return; }
        var b = new Button { Content = action };
        b.Click += (_, _) => onAction?.Invoke();
        bar.ActionButton = b;
    }

    void OnGamePicked(object sender, SelectionChangedEventArgs e)
    {
        _game = (GameList.SelectedItem as GameItem)?.Game;
        if (_game is not null) Games.Remember(_game);
        RefreshAll();
    }

    async void OnBrowse(object sender, RoutedEventArgs e)
    {
        var picker = new Microsoft.Windows.Storage.Pickers.FolderPicker(AppWindow.Id);
        var res = await picker.PickSingleFolderAsync();
        if (res is null) return;
        if (Games.FromFolder(res.Path) is not { } g)
        {
            await Message(T("No game in this folder"), T("Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."));
            return;
        }
        var existing = _games.FirstOrDefault(x => x.Game.Bin.Equals(g.Bin, StringComparison.OrdinalIgnoreCase));
        if (existing is null) { existing = Item(g); _games.Add(existing); GameList.ItemsSource = null; GameList.ItemsSource = _games; }
        NoGames.Visibility = Visibility.Collapsed;
        GameList.SelectedItem = existing;
    }

    void RefreshGame()
    {
        _state = _game is null ? null : Scan.Run(_game);
        GameDetails.Visibility = _state is null ? Visibility.Collapsed : Visibility.Visible;
        if (_state is not { } s) return;

        Chip(ClosedIcon, ClosedText, !_running, _running ? T("The game is running: close it") : T("The game is closed"));
        Chip(LoaderIcon, LoaderChip, s.Loader is not null, s.Loader is null ? T("No ASI loader yet") : T("ASI loader: {0}", s.Loader));
        LayerChip.Text = T("Graphics: {0}", LayerName(s.Layer));

        InstalledList.Children.Clear();
        AddInstalled("Apex Radiance", Known(Components.Apex), Components.Apex);
        AddInstalled("RTX Remix bridge", Known(Components.Bridge), Components.Bridge);
        AddInstalled("Sims3SettingsSetter", Known(Components.S3ss), Components.S3ss);
        AddInstalled("Sims 3 Modernization", Known(Components.Modernization), Components.Modernization);
        AddInstalled("MonoPatcher", Known(Components.MonoPatcher), Components.MonoPatcher);
        if (S3ppPresent) AddInstalled("Sims 3 Performance Patch", "", Components.S3pp);
        // the other community mods Setup recognises (installed by hand; links to their official pages)
        foreach (var k in KnownMods.Found(_game!.Bin, Scan.DocumentsFor(_game)))
        {
            var row = new Grid();
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(new TextBlock { Text = k.Name, FontSize = 13 });
            UIElement right = k.Url is null
                ? new TextBlock { Text = T("Installed"), FontSize = 12, Foreground = (Brush)Application.Current.Resources["ApexSuccessBrush"] }
                : new HyperlinkButton { Content = T("Installed · official page"), NavigateUri = new Uri(k.Url), Padding = new Thickness(0), FontSize = 12 };
            Grid.SetColumn((FrameworkElement)right, 1);
            row.Children.Add(right);
            InstalledList.Children.Add(row);
        }

        ProblemList.Children.Clear();
        var found = Compat.Check(_game!, s);
        foreach (var x in found)
        {
            var bar = new InfoBar
            {
                IsOpen = true, IsClosable = false, Message = T(x.Text, x.Args),
                Severity = x.Severity switch { Severity.Error => InfoBarSeverity.Error, Severity.Warning => InfoBarSeverity.Warning, _ => InfoBarSeverity.Informational },
                Title = x.CanFix ? T("Setup fixes this") : "",
            };
            if (x.Link is { } link) bar.ActionButton = new HyperlinkButton { Content = T("Read more"), NavigateUri = new Uri(link) };
            ProblemList.Children.Add(bar);
        }
        if (found.Any(x => x.CanFix))
        {
            var fix = new Button { Content = T("Fix all now"), Style = (Style)Application.Current.Resources["AccentButtonStyle"], IsEnabled = !_running };
            fix.Click += async (_, _) =>
            {
                await Guarded(() => { Engine.CheckWritable(); foreach (var x in Compat.Check(_game!, Scan.Run(_game!)).Where(x => x.CanFix)) x.Fix!(); }, T("Fixed."));
            };
            ProblemList.Children.Add(fix);
        }
        else if (found.Count == 0)
            ProblemList.Children.Add(new TextBlock { Text = T("No incompatible settings found."), FontSize = 12, Foreground = (Brush)Application.Current.Resources["ApexSuccessBrush"] });
    }

    void Chip(FontIcon icon, TextBlock text, bool ok, string label)
    {
        icon.Glyph = ok ? "" : "";
        icon.Foreground = (Brush)Application.Current.Resources[ok ? "ApexSuccessBrush" : "ApexWarningBrush"];
        text.Text = label;
    }

    void AddInstalled(string name, string? version, string c)
    {
        var latest = Latest(c);
        string status = version is null ? T("Not installed")
            : version.Length == 0 ? T("Installed")
            : Newer(latest, version) ? T("{0} · update to {1}", version, latest!)
            : version;
        var g = new Grid();
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new TextBlock { Text = name, FontSize = 13 });
        var st = new TextBlock { Text = status, FontSize = 12, Foreground = (Brush)Application.Current.Resources[version is null ? "ApexMutedBrush" : "ApexSuccessBrush"] };
        Grid.SetColumn(st, 1);
        g.Children.Add(st);
        InstalledList.Children.Add(g);
    }

    string LayerName(Layer l) => l switch
    {
        Layer.Native => T("the game's own Direct3D 9"),
        Layer.Dxvk => "DXVK",
        Layer.Bridge => "RTX Remix bridge",
        _ => T("another d3d9.dll"),
    };

    // ---------- components ----------

    // the renderer choice: DXVK (Apex recommends it), the RTX Remix bridge, the game's own Direct3D 9, or another d3d9.dll kept as it is
    const string RDxvk = "dxvk", RBridge = "bridge", RNative = "native", RKeep = "keep";
    string _renderer = RDxvk;

    static string RendererOf(State s) => s.Layer switch { Layer.Dxvk => RDxvk, Layer.Bridge => RBridge, Layer.Native => RNative, _ => RKeep };

    void SetDefaults()
    {
        // after the release check: the defaults compare with the latest versions
        if (_state is not { } s || _defaultsFor == _game || (_latest.Count == 0 && !_offline)) return;
        _defaultsFor = _game;
        // what is installed stays on; what Apex recommends (DXVK, Sims3SettingsSetter) is on when missing
        _renderer = s.Layer == Layer.Native ? RDxvk : RendererOf(s);
        S3ssToggle.IsOn = true;
        ModToggle.IsOn = s.Modernization;
        MonoToggle.IsOn = s.MonoPatcher;
        S3ppToggle.IsOn = true;
    }

    void OnToggle(object sender, RoutedEventArgs e) => RefreshComponents();

    // installed: its Remove button (Keep while marked), the switch hidden and carrying the choice for Plan(); missing: the switch
    // (made here beside the switch, in its grid cell: declared in the XAML they failed its parse at start)
    readonly Dictionary<ToggleSwitch, Button> _removeButtons = new();
    void ShowCard(ToggleSwitch toggle, bool present)
    {
        if (!_removeButtons.TryGetValue(toggle, out var remove))
        {
            remove = new Button { MinWidth = 96, VerticalAlignment = VerticalAlignment.Center };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(remove, Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(toggle));
            Grid.SetColumn(remove, Grid.GetColumn(toggle));
            remove.Click += (_, _) => toggle.IsOn = !toggle.IsOn; // marks it for removal by Apply, or takes the mark back (OnToggle refreshes)
            if (toggle.Parent is Panel panel) panel.Children.Add(remove);
            _removeButtons[toggle] = remove;
        }
        toggle.Visibility = present ? Visibility.Collapsed : Visibility.Visible;
        remove.Visibility = present ? Visibility.Visible : Visibility.Collapsed;
        remove.Content = toggle.IsOn ? T("Remove") : T("Keep");
    }

    void OnRenderer(object sender, SelectionChangedEventArgs e)
    {
        if (_fillingRenderer || RendererChoice.SelectedIndex < 0) return;
        _renderer = _rendererTags[RendererChoice.SelectedIndex];
        RefreshComponents();
    }
    bool _fillingRenderer;

    readonly List<string> _rendererTags = new();
    void FillRenderer(State s)
    {
        var tags = new List<string> { RDxvk, RBridge, RNative };
        if (s.Layer == Layer.Other) tags.Add(RKeep);
        var titles = tags.Select(t => t switch
        {
            RDxvk => T("DXVK (recommended)"),
            RBridge => T("RTX Remix bridge (64-bit, about 600 MB more free memory)"),
            RNative => T("The game's own Direct3D 9"),
            _ => T("Keep my d3d9.dll"),
        }).ToList();
        _fillingRenderer = true;
        if (!RendererChoice.Items.Cast<object>().Select(o => o as string).SequenceEqual(titles))
        {
            RendererChoice.Items.Clear();
            foreach (var t in titles) RendererChoice.Items.Add(t);
            _rendererTags.Clear();
            _rendererTags.AddRange(tags);
        }
        var want = _rendererTags.IndexOf(_renderer);
        if (RendererChoice.SelectedIndex != want) RendererChoice.SelectedIndex = want;
        _fillingRenderer = false;
    }

    void RefreshComponents()
    {
        if (_state is not { } s || _game is not { } g) return;
        SetDefaults();
        var latest = Latest(Components.Apex) ?? "";
        ApexDesc.Text = s.ApexVersion is null ? T("Installs {0} into Game\\Bin.", latest)
            : s.ApexVersion == "?" ? T("Replaces the installed copy with {0}. Your settings stay.", latest)
            : Newer(latest, s.ApexVersion) ? T("Updates {0} to {1}. Your settings stay.", s.ApexVersion, latest)
            : T("{0} is installed and up to date: nothing to change.", s.ApexVersion);

        FillRenderer(s);
        var cur = RendererOf(s);
        var changed = _renderer != cur;
        BridgeName.Text = T("Renderer · now {0}", LayerName(s.Layer));
        BridgeDetail.Text = (_renderer switch
        {
            RDxvk => T("Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it. From DXVK's GitHub release."),
            RBridge => T("DXVK in its own 64-bit process: the game keeps about 600 MB more free memory. Set for {0}; needs a Vulkan graphics card.", g.Exe),
            RNative => T("Windows' Direct3D 9. Apex Radiance works with it, but it is made and tested with DXVK."),
            _ => T("Your d3d9.dll stays as it is."),
        }) + (changed && s.Layer != Layer.Native ? " " + T("Setup keeps your current d3d9.dll and puts it back if you remove the new one.") : "");

        // the game resets its graphics options when the card it sees changes: DXVK's TS3W profile and the bridge report a
        // GeForce GTX 580; the game's own D3D9 the real card (DXVK on TS3.exe: unverified, so treated as a change)
        string Card(string r) => r switch { RBridge => "580", RDxvk when g.Exe == "TS3W.exe" => "580", RNative => "real", _ => "?" };
        if (changed && (Card(_renderer) != Card(cur) || Card(cur) == "?"))
            SetInfo(GraphicsInfo, InfoBarSeverity.Warning, T("Graphics options may reset once"),
                T("The game resets its graphics options when the graphics card it sees changes, and {0} reports a different one than {1}. Setup keeps a copy of Options.ini and puts it back if you go back.", RendererName(_renderer), LayerName(s.Layer)));
        else if (changed)
            SetInfo(GraphicsInfo, InfoBarSeverity.Informational, T("Your graphics options stay"),
                T("DXVK and the bridge both report a GeForce GTX 580, so the game keeps its settings."));
        else GraphicsInfo.IsOpen = false;

        // An installed component shows a Remove button (Keep once marked: Apply removes it, and its pill says so); a missing one
        // its switch, which means Install. A switch that meant "keep" on an installed component, off meaning uninstall, read
        // as a control that did nothing.
        S3ssPill.Text = s.S3ss && !S3ssToggle.IsOn ? T("Will be removed") : Pill(Known(Components.S3ss), Latest(Components.S3ss), false);
        ModPill.Text = s.Modernization && !ModToggle.IsOn ? T("Will be removed") : Pill(Known(Components.Modernization), Latest(Components.Modernization), s.Ts3NativeWrongBuild);
        MonoPill.Text = s.MonoPatcher && !MonoToggle.IsOn ? T("Will be removed") : Pill(Known(Components.MonoPatcher), Latest(Components.MonoPatcher), false);
        S3ppPill.Text = S3ppPresent && !S3ppToggle.IsOn ? T("Will be removed") : S3ppPresent ? T("Installed") : _s3ppArchive is not null ? T("Ready to install") : T("Not installed");
        ShowCard(S3ssToggle, s.S3ss);
        ShowCard(ModToggle, s.Modernization && g.ModernizationSupported);
        ShowCard(MonoToggle, s.MonoPatcher);
        ShowCard(S3ppToggle, S3ppPresent);
        if (!S3ppPresent) S3ppToggle.Visibility = Visibility.Collapsed; // (missing: its download buttons below)
        S3ppGet.Visibility = !S3ppPresent && _s3ppArchive is null && _s3ppWatch is null ? Visibility.Visible : Visibility.Collapsed;
        S3ppWait.Visibility = _s3ppWatch is not null ? Visibility.Visible : Visibility.Collapsed;
        S3ppWaitText.Text = T("Waiting for your download in {0}…", DownloadsFolder());
        ModToggle.IsEnabled = g.ModernizationSupported;
        if (!g.ModernizationSupported) ModToggle.IsOn = false;
        ModDesc.Text = g.ModernizationSupported
            ? T("Stutter, route and crash fixes by rodneypantonial: ts3native in Game\\Bin and ts3fixes.package in Mods\\Packages, from its GitHub release.")
            : T("Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47.");

        bool need = Engine.NeedsLoader(s, changed && _renderer is RDxvk or RBridge);
        LoaderPlanIcon.Glyph = need ? "" : "";
        LoaderPlanIcon.Foreground = (Brush)Application.Current.Resources[need ? "ApexAccentBrush" : "ApexSuccessBrush"];
        LoaderPlan.Text = !need ? T("ASI loader found: {0}.", s.Loader!)
            : s.LoaderIsD3d9 ? T("Your ASI loader is d3d9.dll, which the new renderer replaces: Setup adds Ultimate ASI Loader as wininet.dll.")
            : T("No ASI loader found: Setup adds Ultimate ASI Loader as wininet.dll, from its GitHub release.");
        UpdateFooter();
    }

    string RendererName(string r) => r switch
    {
        RDxvk => "DXVK", RBridge => "RTX Remix bridge", RNative => T("the game's own Direct3D 9"), _ => T("another d3d9.dll"),
    };

    string Pill(string? version, string? latest, bool wrong) =>
        wrong ? T("Wrong build") : version is null ? T("Not installed") : version.Length == 0 ? T("Installed")
        : Newer(latest, version) ? T("Update {0}", latest!) : T("Up to date");

    // What Apply does: the components to install or update, and the ones to remove. Only what differs from the game as it is.
    (List<string> Install, List<string> Remove) Plan()
    {
        var add = new List<string>();
        var del = new List<string>();
        if (_state is not { } s || _game is not { } g) return (add, del);
        if (s.ApexVersion is null || s.ApexVersion == "?" || Newer(Latest(Components.Apex), s.ApexVersion)) add.Add(Components.Apex);

        var cur = RendererOf(s);
        if (_renderer == cur)
        {
            if (cur == RBridge && NeedsUpdate(Components.Bridge)) add.Add(Components.Bridge);
            if (cur == RDxvk && s.Installed(Components.Dxvk) is { } v && Newer(Latest(Components.Dxvk), v)) add.Add(Components.Dxvk);
        }
        else
        {
            if (cur == RBridge) del.Add(Components.Bridge);
            if (cur == RDxvk) del.Add(Components.Dxvk);
            if (_renderer == RBridge) add.Add(Components.Bridge);
            if (_renderer == RDxvk) add.Add(Components.Dxvk);
        }

        void Optional(string c, bool on, bool present, bool update)
        {
            if (on && (!present || update)) add.Add(c);
            if (!on && present) del.Add(c);
        }
        Optional(Components.S3ss, S3ssToggle.IsOn, s.S3ss, NeedsUpdate(Components.S3ss));
        Optional(Components.MonoPatcher, MonoToggle.IsOn, s.MonoPatcher, NeedsUpdate(Components.MonoPatcher));
        // Sims 3 Performance Patch: installed from the player's own download only
        if (S3ppPresent && !S3ppToggle.IsOn) del.Add(Components.S3pp);
        if (!S3ppPresent && _s3ppArchive is not null) add.Add(Components.S3pp);
        Optional(Components.Modernization, ModToggle.IsOn && g.ModernizationSupported, s.Modernization, s.Ts3NativeWrongBuild || NeedsUpdate(Components.Modernization));
        if (Engine.NeedsLoader(s, _renderer != cur && _renderer is RDxvk or RBridge)) add.Add(Components.Loader);
        return (add, del);
    }

    // ---------- Sims 3 Performance Patch: the player downloads it on Nexus Mods, Setup picks the file up ----------

    const string S3ppPage = "https://www.nexusmods.com/thesims3/mods/235";
    string? _s3ppArchive;
    FileSystemWatcher? _s3ppWatch;
    bool S3ppPresent => _game is { } g && File.Exists(System.IO.Path.Combine(g.Bin, "Sims3Performance.asi"));

    static string DownloadsFolder() =>
        Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders")
            ?.GetValue("{374DE290-123F-4565-9164-39C4925E467B}") as string
        ?? System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");

    void OnS3ppDownload(object sender, RoutedEventArgs e)
    {
        // through Explorer, so the browser opens as the player, not with Setup's administrator rights
        Process.Start(new ProcessStartInfo("explorer.exe", S3ppPage) { UseShellExecute = true });
        _s3ppWatch?.Dispose();
        _s3ppWatch = new FileSystemWatcher(DownloadsFolder()) { IncludeSubdirectories = false, EnableRaisingEvents = true };
        FileSystemEventHandler seen = (_, a) => _ = TakeS3ppAsync(a.FullPath);
        _s3ppWatch.Created += seen;
        _s3ppWatch.Renamed += (_, a) => _ = TakeS3ppAsync(a.FullPath);
        RefreshComponents();
    }

    async void OnS3ppPick(object sender, RoutedEventArgs e)
    {
        var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
        picker.FileTypeFilter.Add(".zip");
        picker.FileTypeFilter.Add(".asi");
        var res = await picker.PickSingleFileAsync();
        if (res is not null) await TakeS3ppAsync(res.Path, fromPicker: true);
    }

    // a finished download (browsers write a temporary name, then rename it) that holds Sims3Performance.asi
    async Task TakeS3ppAsync(string path, bool fromPicker = false)
    {
        var ext = System.IO.Path.GetExtension(path).ToLowerInvariant();
        if (ext is not (".zip" or ".asi")) return;
        for (int i = 0; i < 20; i++) // wait until the browser has closed the file
        {
            try { using (File.Open(path, FileMode.Open, FileAccess.Read, FileShare.None)) break; }
            catch (IOException) { await Task.Delay(500); }
        }
        var found = await Task.Run(() => GitHubSource.S3ppFiles(path));
        DispatcherQueue.TryEnqueue(async () =>
        {
            if (found is null)
            {
                if (fromPicker || ext == ".zip" && System.IO.Path.GetFileName(path).Contains("Performance", StringComparison.OrdinalIgnoreCase))
                    await Message(T("Not the Performance Patch"), T("{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi.", System.IO.Path.GetFileName(path)));
                return;
            }
            _s3ppWatch?.Dispose();
            _s3ppWatch = null;
            _s3ppArchive = path;
            Activate(); // back in front of the browser
            if (_game is not null && !_running && !_busy) await InstallAsync(new List<string> { Components.S3pp });
            else RefreshComponents();
        });
    }

    static string Name(string c) => c switch
    {
        Components.Apex => "Apex Radiance", Components.Bridge => "RTX Remix bridge", Components.Loader => "Ultimate ASI Loader",
        Components.Dxvk => "DXVK", Components.S3ss => "Sims3SettingsSetter", Components.MonoPatcher => "MonoPatcher",
        Components.S3pp => "Sims 3 Performance Patch", _ => "Sims 3 Modernization",
    };

    // ---------- install ----------

    async Task InstallAsync(List<string> comps, List<string>? remove = null)
    {
        if (_game is not { } g || _busy) return;
        _busy = true;
        remove ??= new List<string>();
        var dels = Components.All.Where(remove.Contains).ToList();
        var order = Components.All.Reverse().Where(comps.Contains).ToList(); // the engine's order
        InstallTitle.Text = T("Applying changes");
        Show(InstallStep);
        TaskList.Children.Clear();
        InstallBar.Value = 0;
        var rows = dels.Select(c => AddTask(T("Remove {0}", Name(c)))).Concat(order.Select(c => AddTask(Name(c)))).ToList();
        rows.Add(AddTask(T("Fix incompatible settings")));
        void Mark(int active)
        {
            for (int i = 0; i < rows.Count; i++)
                rows[i](i < active ? 2 : i == active ? 1 : 0);
        }
        Mark(0);
        var progress = new Progress<Progress>(p =>
        {
            InstallBar.Value = p.Fraction;
            Mark(p.Arg is null ? (p.Fraction >= 1 ? rows.Count : p.Fraction > 0.9 ? rows.Count - 1 : dels.Count) : dels.Count + order.IndexOf(p.Arg));
        });
        try
        {
            await Task.Run(async () =>
            {
                foreach (var c in dels) Engine.Remove(g, c);
                await Engine.InstallAsync(g, order, new GitHubSource { S3ppArchive = _s3ppArchive }, progress);
            });
            _lastInstall = order;
            _busy = false;
            Show(DoneStep);
        }
        catch (Exception ex)
        {
            _busy = false;
            await Message(T("Setup could not finish"), T("{0}\n\nWhat was installed before the error can be removed in Repair or remove.", T(ex.Message)));
            Show(ComponentsStep);
        }
    }

    // a row of the task list; the returned action sets it to 0 pending, 1 working, 2 done
    Action<int> AddTask(string text)
    {
        var row = new Grid { Padding = new Thickness(18, 10, 18, 10), ColumnSpacing = 12 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(20) });
        row.ColumnDefinitions.Add(new ColumnDefinition());
        var ring = new ProgressRing { Width = 16, Height = 16, IsActive = false };
        var icon = new FontIcon { FontSize = 15 };
        var label = new TextBlock { Text = text, FontSize = 13, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(label, 1);
        row.Children.Add(ring); row.Children.Add(icon); row.Children.Add(label);
        TaskList.Children.Add(row);
        return state =>
        {
            ring.IsActive = state == 1;
            ring.Visibility = state == 1 ? Visibility.Visible : Visibility.Collapsed;
            icon.Visibility = state == 1 ? Visibility.Collapsed : Visibility.Visible;
            icon.Glyph = state == 2 ? "" : "";
            icon.Foreground = (Brush)Application.Current.Resources[state == 2 ? "ApexSuccessBrush" : "ApexMutedBrush"];
            label.Foreground = (Brush)Application.Current.Resources[state == 0 ? "ApexMutedBrush" : "ApexSoftTextBrush"];
        };
    }

    void RefreshDone()
    {
        DoneTitle.Text = T("You're all set");
        DoneText.Text = T("Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu.");
        DoneSummary.Children.Clear();
        foreach (var c in _lastInstall)
            DoneSummary.Children.Add(new TextBlock { Text = T("Installed: {0} {1}", Name(c), _state?.Installed(c) ?? ""), FontSize = 12, Foreground = (Brush)Application.Current.Resources["ApexSoftTextBrush"] });
        if (_game is { } g)
            DoneSummary.Children.Add(new TextBlock { Text = T("Copies of replaced files: {0}", Manifest.DirOf(g.Bin)), FontSize = 12, TextWrapping = TextWrapping.Wrap, Foreground = (Brush)Application.Current.Resources["ApexMutedBrush"] });
    }

    void OnOpenBin(object sender, RoutedEventArgs e)
    {
        if (_game is { } g) Process.Start(new ProcessStartInfo("explorer.exe", $"\"{g.Bin}\"") { UseShellExecute = true });
    }

    // ---------- repair or remove ----------

    void RefreshManage()
    {
        if (_step == DoneStep) RefreshDone();
        ManageList.Children.Clear();
        if (_game is not { } g || _state is not { } s) { ManageIntro.Text = T("Choose your game first."); return; }
        ManageIntro.Text = T("The Sims 3 · {0} · {1}", g.Label, g.Bin);
        var m = s.Manifest;
        if (s.ApexVersion is not null)
            ManageRow("Apex Radiance " + s.ApexVersion, T("Removes ApexRadiance.asi. Your settings in Documents stay."), T("Remove"), () => RemoveAsync(Components.Apex));
        if (s.Layer == Layer.Bridge)
        {
            bool backup = m.Files.Any(f => f.Component == Components.Bridge && f.Backup is not null && System.IO.Path.GetFileName(f.Path) == "d3d9.dll");
            var desc = backup ? T("Puts your previous d3d9.dll back. Apex Radiance stays.")
                : m.OptionsBackup is not null ? T("The game goes back to its own Direct3D 9; Setup puts your Options.ini copy back.")
                : T("Setup has no copy of your previous d3d9.dll: the game goes back to its own Direct3D 9 and resets its graphics options once.");
            ManageRow(T("RTX Remix bridge"), desc, T("Remove"), () => RemoveAsync(Components.Bridge));
        }
        if (s.Layer == Layer.Dxvk)
            ManageRow("DXVK", m.Files.Any(f => f.Component == Components.Dxvk && f.Backup is not null)
                ? T("Puts your previous d3d9.dll back.")
                : T("The game goes back to its own Direct3D 9 and may reset its graphics options once."), T("Remove"), () => RemoveAsync(Components.Dxvk));
        if (S3ppPresent) ManageRow("Sims 3 Performance Patch", T("Removes Sims3Performance.asi and its .ini."), T("Remove"), () => RemoveAsync(Components.S3pp));
        if (s.MonoPatcher) ManageRow("MonoPatcher", T("Removes MonoPatcher.asi and its package."), T("Remove"), () => RemoveAsync(Components.MonoPatcher));
        if (s.S3ss) ManageRow("Sims3SettingsSetter", T("Removes Sims3SettingsSetter.asi."), T("Remove"), () => RemoveAsync(Components.S3ss));
        if (s.Modernization) ManageRow("Sims 3 Modernization", T("Removes ts3native and ts3fixes.package."), T("Remove"), () => RemoveAsync(Components.Modernization));
        if (m.Versions.ContainsKey(Components.Loader)) ManageRow("Ultimate ASI Loader", T("Setup added it as wininet.dll. Without a loader no .asi mod loads."), T("Remove"), () => RemoveAsync(Components.Loader));
        ManageRow(T("Repair or update"), T("Downloads and copies again everything installed here. Your settings stay."), T("Repair"), Repair);
        ManageRow(T("Uninstall everything"), T("Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."), T("Uninstall"), UninstallAll, danger: true);
    }

    void ManageRow(string title, string desc, string action, Func<Task> run, bool danger = false)
    {
        var row = new Grid { Padding = new Thickness(18, 14, 18, 14), ColumnSpacing = 14, BorderBrush = (Brush)Application.Current.Resources["ApexCardBorderBrush"], BorderThickness = new Thickness(0, ManageList.Children.Count == 0 ? 0 : 1, 0, 0) };
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold });
        text.Children.Add(new TextBlock { Text = desc, FontSize = 12, TextWrapping = TextWrapping.Wrap, Foreground = (Brush)Application.Current.Resources["ApexMutedBrush"] });
        var b = new Button { Content = action, MinWidth = 96, VerticalAlignment = VerticalAlignment.Center, IsEnabled = !_running && !_busy };
        if (danger) { b.Background = new SolidColorBrush(ColorHelper.FromArgb(255, 0x2A, 0x16, 0x15)); b.Foreground = new SolidColorBrush(ColorHelper.FromArgb(255, 0xF2, 0xB6, 0xB2)); }
        b.Click += async (_, _) => await run();
        Grid.SetColumn(b, 1);
        row.Children.Add(text); row.Children.Add(b);
        ManageList.Children.Add(row);
    }

    async Task RemoveAsync(string c)
    {
        if (_game is not { } g) return;
        if (!await Confirm(T("Remove {0}?", Name(c)), T("Close the game first. Setup removes the files and puts back the ones it replaced."), T("Remove"))) return;
        await Guarded(() => Engine.Remove(g, c), T("{0} removed.", Name(c)));
    }

    async Task UninstallAll()
    {
        if (_game is not { } g) return;
        if (!await Confirm(T("Uninstall everything?"), T("Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."), T("Uninstall"))) return;
        await Guarded(() => Engine.RemoveAll(g), T("Everything Setup installed was removed."));
    }

    Task Repair()
    {
        if (_state is not { } s) return Task.CompletedTask;
        var l = new List<string> { Components.Apex };
        if (s.Layer == Layer.Bridge) l.Add(Components.Bridge);
        if (s.Layer == Layer.Dxvk && s.Installed(Components.Dxvk) is not null) l.Add(Components.Dxvk);
        if (s.S3ss) l.Add(Components.S3ss);
        if (s.MonoPatcher) l.Add(Components.MonoPatcher);
        if (s.Modernization && _game!.ModernizationSupported) l.Add(Components.Modernization);
        if (Engine.NeedsLoader(s, false)) l.Add(Components.Loader);
        return InstallAsync(l);
    }

    async Task Guarded(Action work, string done)
    {
        _busy = true;
        try
        {
            await Task.Run(work);
            SetInfo(ManageInfo, InfoBarSeverity.Success, done, "");
        }
        catch (Exception ex) { SetInfo(ManageInfo, InfoBarSeverity.Error, T("Setup could not finish"), T(ex.Message)); }
        _busy = false;
        RefreshAll();
    }

    // ---------- about ----------

    static readonly (string Name, string Licence, string? File, string Url)[] Credits =
    {
        ("RTX Remix bridge", "MIT", "RTX-Remix-bridge-LICENSE.txt", "https://github.com/NVIDIAGameWorks/dxvk-remix"),
        ("RTX Remix bridge: third-party notices", "", "RTX-Remix-bridge-ThirdPartyLicenses.txt", "https://github.com/NVIDIAGameWorks/dxvk-remix"),
        ("Microsoft Detours", "MIT", "Detours-LICENSE.txt", "https://github.com/microsoft/Detours"),
        ("DXVK", "zlib", "DXVK-LICENSE.txt", "https://github.com/doitsujin/dxvk"),
        ("Ultimate ASI Loader", "MIT", "Ultimate-ASI-Loader-LICENSE.txt", "https://github.com/ThirteenAG/Ultimate-ASI-Loader"),
        ("Sims3SettingsSetter", "", null, "https://github.com/sims3fiend/Sims3SettingsSetter"),
        ("Sims 3 Modernization", "MIT", null, "https://github.com/rodneypantonial/sims3-modernization"),
        ("MonoPatcher", "", null, "https://github.com/LazyDuchess/MonoPatcher"),
    };

    void RefreshAbout()
    {
        if (LicenceList.Children.Count > 0) return;
        foreach (var (name, lic, file, url) in Credits)
        {
            var row = new Grid { Padding = new Thickness(18, 8, 12, 8), ColumnSpacing = 8, BorderBrush = (Brush)Application.Current.Resources["ApexCardBorderBrush"], BorderThickness = new Thickness(0, LicenceList.Children.Count == 0 ? 0 : 1, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(new TextBlock { Text = T(name), FontSize = 13, VerticalAlignment = VerticalAlignment.Center });
            var l = new TextBlock { Text = lic, FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Foreground = (Brush)Application.Current.Resources["ApexMutedBrush"] };
            Grid.SetColumn(l, 1); row.Children.Add(l);
            if (file is not null)
            {
                var view = new HyperlinkButton { Content = T("Read") };
                view.Click += (_, _) => LicenceText.Text = Licence(file);
                Grid.SetColumn(view, 2); row.Children.Add(view);
            }
            var src = new HyperlinkButton { Content = "GitHub", NavigateUri = new Uri(url) };
            Grid.SetColumn(src, 3); row.Children.Add(src);
            LicenceList.Children.Add(row);
        }
        LicenceText.Text = Licence("RTX-Remix-bridge-LICENSE.txt");
    }

    static string Licence(string file)
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("Licenses." + file);
        return s is null ? "" : new StreamReader(s).ReadToEnd();
    }

    // ---------- dialogs and language ----------

    async Task Message(string title, string text)
    {
        var d = new ContentDialog { Title = title, Content = text, CloseButtonText = T("OK"), XamlRoot = Content.XamlRoot };
        await d.ShowAsync();
    }

    async Task<bool> Confirm(string title, string text, string yes)
    {
        var d = new ContentDialog { Title = title, Content = text, PrimaryButtonText = yes, CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Close, XamlRoot = Content.XamlRoot };
        return await d.ShowAsync() == ContentDialogResult.Primary;
    }

    void OnLanguage(object sender, RoutedEventArgs e)
    {
        Pt = !Pt;
        Localize();
        LicenceList.Children.Clear();
        RefreshAll();
    }

    void Localize() => Strings.Localize(Root);
}
