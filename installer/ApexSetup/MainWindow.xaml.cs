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

        // the player's choice (the title bar's language button), else Windows' display language, as the mod's menu
        Lang = SavedChoice() ?? SystemLanguage(System.Globalization.CultureInfo.CurrentUICulture);
        Root.Language = Bcp47(Lang);
        // every screen's buttons follow the game starting or closing (the status and Manage rows are built with that state)
        _timer.Tick += (_, _) => { var r = Games.IsRunning(); if (r != _running) { _running = r; RefreshAll(); } };
        // closing in the middle of an install or a removal would leave the game half changed
        AppWindow.Closing += (w, a) =>
        {
            if (!_busy) return;
            a.Cancel = true;
            _ = Message(T("Setup is applying changes"), T("Wait until it finishes before closing."));
        };
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
        _ = DetectVulkanAsync();
        await LoadReleasesAsync();
    }

    // The card's Vulkan version, asked once off the UI thread (loading the drivers takes a moment); null until known or
    // when the driver could not be asked (the bridge then stays available)
    (Version? Version, string? Card)? _vulkan;
    bool BridgeBlocked => _vulkan is { } vk && (vk.Version is null || vk.Version < Gpu.Needed);

    async Task DetectVulkanAsync()
    {
        try { _vulkan = await Task.Run(Gpu.Vulkan); }
        catch (Exception e) { App.Log(e); return; }
        if (BridgeBlocked && _step == ComponentsStep) RefreshComponents();
    }

    static readonly string? Preview = Environment.GetEnvironmentVariable("APEX_SETUP_PREVIEW_LATEST") is { Length: > 0 } pv ? pv : null;
    bool _loading = true; // the release check has not answered yet: nothing is installed meanwhile
    async Task LoadReleasesAsync()
    {
        _offline = false;
        _loading = true;
        RefreshAll();
        // each repo on its own: an optional mod's repo that fails only hides that mod's update (its install fetches it again)
        await Task.WhenAll(Repo.Values.Distinct().Select(async r =>
        {
            try { _latest[r] = await Releases.LatestAsync(r); }
            catch (Exception) { _latest.Remove(r); }
        }));
        // (screenshots and design checks) APEX_SETUP_PREVIEW_LATEST=3.2.0 makes the latest Apex release that version, to show
        // the update screens; nothing is installed while it is set (InstallAsync)
        if (Preview is { } v && _latest.GetValueOrDefault(Releases.Apex) is { } apex) _latest[Releases.Apex] = apex with { Tag = "v" + v };
        _offline = !_latest.ContainsKey(Releases.Apex);
        _loading = false;
        RefreshAll();
    }

    GameItem Item(Game g) => new()
    {
        Game = g, Path = g.Root,
        Name = "The Sims 3",
        Badge = g.Label,
    };

    // the bridge's version is the Apex release's only when that release ships the bridge zip
    string? Latest(string c) => Repo.TryGetValue(c, out var repo) && _latest.GetValueOrDefault(repo) is { } r
        && (c != Components.Bridge || r.Assets.Any(a => a.Name == "Apex-Radiance-RTX-Remix-bridge.zip")) ? r.Version : null;

    // The installed version: Setup's own record, else the latest release's when the file is identical to it (GitHub's SHA-256
    // digest), else "" (installed, version unknown), or null when absent.
    string? Known(string c)
    {
        if (_state is not { } s || _game is not { } g) return null;
        var (present, file, asset) = c switch
        {
            Components.S3ss => (s.S3ss, Scan.FindAsi(g.Bin, "Sims3SettingsSetter.asi") ?? "", "Sims3SettingsSetter.asi"),
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

    static bool Newer(string? latest, string? installed) => Releases.Newer(latest, installed);

    // ---------- navigation ----------

    void Show(int step)
    {
        _step = step;
        for (int i = 0; i < _screens.Length; i++) _screens[i].Visibility = i == step ? Visibility.Visible : Visibility.Collapsed;
        Animate(_screens[step]);
        Scroller.ChangeView(null, 0, null, true);
        SyncNav();
        RefreshAll();
    }

    // the sidebar shows the screen that is open (also after a refused jump); Install and Done have no item
    void SyncNav()
    {
        _syncNav = true;
        Nav.SelectedItem = Nav.MenuItems.Concat(Nav.FooterMenuItems).OfType<NavigationViewItem>().FirstOrDefault(n => (string)n.Tag == _step.ToString());
        _syncNav = false;
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
        if (!ok) { DispatcherQueue.TryEnqueue(SyncNav); return; }
        _backTo = -1;
        Show(to == Manage && _game is null ? GameStep : to);
    }

    bool CanLeaveGame() => _game is not null && !_running;

    // Back goes where the player came from: the status card's Change components / Repair or remove return to it; the steps
    // go one back
    int _backTo = -1;
    void GoFromStatus(int to) { _backTo = Welcome; Show(to); }
    void OnBack(object sender, RoutedEventArgs e)
    {
        var to = _backTo >= 0 ? _backTo : Math.Max(Welcome, _step - 1);
        _backTo = -1;
        Show(to);
    }

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
                if (_offline && add.Count > 0) { await LoadReleasesAsync(); break; } // (the button reads Try again)
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
        // a game with Apex and nothing to update: the status card's own buttons are the actions, no wizard to go into
        var settled = _step == Welcome && _state?.ApexVersion is not null && _game is not null && WelcomeUpdates().Count == 0;
        NextButton.Visibility = !settled && _step is Welcome or GameStep or ComponentsStep or DoneStep ? Visibility.Visible : Visibility.Collapsed;
        var (wa, wd) = _step == ComponentsStep ? Plan() : (new List<string>(), new List<string>());
        var work = wa.Concat(wd).ToList();
        var fixes = _step == ComponentsStep && _found.Any(x => x.CanFix);
        NextText.Text = _step switch
        {
            Welcome => WelcomeUpdates().Count > 0 ? T("Update now") : _state?.ApexVersion is not null ? T("Next") : T("Get started"),
            ComponentsStep when _offline && wa.Count > 0 => T("Try again"),
            ComponentsStep => work.Count == 0 ? (fixes ? T("Fix settings") : T("Nothing to apply")) : _state?.ApexVersion is null ? T("Install")
                : work.Count == 1 ? T("Apply 1 change") : T("Apply {0} changes", work.Count),
            DoneStep => T("Finish"),
            _ => T("Next"),
        };
        NextButton.IsEnabled = !_busy && _step switch
        {
            GameStep => CanLeaveGame(),
            // removals and fixes need no download; installs wait for the release check (offline: the button retries it)
            ComponentsStep => !_loading && !_running && _game is not null && (work.Count > 0 || fixes),
            _ => true,
        };
        FooterNote.Text = _step switch
        {
            GameStep or ComponentsStep when _running => T("Close The Sims 3 to continue."),
            ComponentsStep when _loading => T("Checking the latest versions\u2026"),
            ComponentsStep when _offline && wa.Count > 0 => T("Setup needs the internet to download the files."),
            ComponentsStep when work.Count == 0 && !fixes => T("Everything is installed and up to date."),
            ComponentsStep when _state?.ApexVersion is null => T("Nothing changes until you click Install."),
            _ => "",
        };
        NavComponents.IsEnabled = CanLeaveGame() && !_busy;
        // an empty bar (no buttons, no note) reads as broken: the page takes its room instead
        Footer.Visibility = BackButton.Visibility == Visibility.Visible || NextButton.Visibility == Visibility.Visible || FooterNote.Text.Length > 0
            ? Visibility.Visible : Visibility.Collapsed;
    }

    // ---------- refresh ----------

    void RefreshAll()
    {
        LangText.Text = Languages.First(l => l.Code == Lang).Name;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(LangButton, T("Language"));
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
            SetInfo(WelcomeInfo, InfoBarSeverity.Error, T("No internet connection"), T("Check your internet connection, then try again."), T("Try again"), async () => await LoadReleasesAsync());
        else if (_loading && _game is not null)
            SetInfo(WelcomeInfo, InfoBarSeverity.Informational, T("Checking for updates\u2026"), "");
        else if (Updates() is { Count: > 0 } ups && _game is { } g)
        {
            // checked at every start: the player learns about a new release here, without visiting GitHub
            var lines = ups.Select(c => T("{0} {1} → {2}", Name(c), Known(c) is { Length: > 0 } v ? v : "?", Latest(c)!));
            SetInfo(WelcomeInfo, InfoBarSeverity.Informational, T("Updates available"),
                string.Join(", ", lines) + ". " + (_running ? T("Close The Sims 3 to update.") : T("Update now installs them.")));
        }
        // a game with Apex, checked and current: say so, so the player knows the check ran
        else if (_game is not null && _state?.ApexVersion is not null && !_loading)
            SetInfo(WelcomeInfo, InfoBarSeverity.Success, T("No updates available"), T("Everything in this game is on its latest version."));
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
        var apexRelease = _latest.GetValueOrDefault(Repo[Components.Apex]);
        var test = Verify.TestBuild(g, s, apexRelease);
        Row("Apex Radiance", test ? T("{0} \u00b7 test build", s.ApexVersion!) : State(Components.Apex), !test && !Updates().Contains(Components.Apex));
        Row(T("Graphics"), LayerName(s.Layer), true);
        foreach (var (c, name, present) in new[] { (Components.S3ss, "Sims3SettingsSetter", s.S3ss), (Components.Modernization, "Sims 3 Modernization", s.Modernization), (Components.MonoPatcher, "MonoPatcher", s.MonoPatcher) })
            if (present) Row(name, State(c), !Updates().Contains(c));

        var found = Verify.Files(g, s, apexRelease).Concat(_found.Where(x => x.Suggest is null)).ToList();
        // (a Warning suggestion is something to look at too)
        if (found.Count == 0 && !_found.Any(x => x.Suggest is not null && x.Severity != Severity.Info))
            StatusList.Children.Add(new TextBlock { Text = _checkedAt is null ? T("Everything is in place.") : T("Everything is in place (checked at {0}).", _checkedAt), FontSize = 12, Foreground = ok });
        else if (_checkedAt is not null)
            StatusList.Children.Add(new TextBlock { Text = T("Checked at {0}.", _checkedAt), FontSize = 12, Foreground = muted });
        foreach (var x in found)
            StatusList.Children.Add(new InfoBar
            {
                IsOpen = true, IsClosable = false, Message = T(x.Text, x.Args), Title = x.CanFix ? T("Setup fixes this") : "",
                Severity = x.Severity switch { Severity.Error => InfoBarSeverity.Error, Severity.Warning => InfoBarSeverity.Warning, _ => InfoBarSeverity.Informational },
            });

        FillSuggestions(StatusList, WelcomeInfo, onStatus: true);

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        // (none accent: the footer's button is the screen's one accent button)
        Button Make(string text, Func<Task> act, bool needsNet = false)
        {
            var b = new Button { Content = text, IsEnabled = !_running && !_busy && !(needsNet && (_offline || _loading)) };
            b.Click += async (_, _) => await act();
            buttons.Children.Add(b);
            return b;
        }
        // the install's own files set right by installing them again; the settings by their fixes
        var repair = new List<string>();
        if (found.Any(x => x.Text.StartsWith("Apex Radiance is not"))) repair.Add(Components.Apex);
        if (found.Any(x => x.Text.Contains("RTX Remix bridge does not start") && !x.CanFix)) repair.Add(Components.Bridge);
        if (s.Loader is null) repair.Add(Components.Loader);
        var fixable = found.Any(x => x.CanFix);
        if (repair.Count > 0 || fixable)
            Make(T("Repair"), async () =>
            {
                if (fixable && !await Guarded(() => { Engine.CheckWritable(); foreach (var x in Compat.Check(g, Scan.Run(g)).Where(x => x.CanFix)) x.Fix!(); }, T("Fixed."), WelcomeInfo, "Fix incompatible settings")) return;
                if (repair.Count > 0 && !_offline) await InstallAsync(repair);
            }, needsNet: !fixable);
        if (test && apexRelease is not null)
            Make(T("Install the official {0}", apexRelease.Version), () => InstallAsync(new List<string> { Components.Apex }), needsNet: true);
        Make(T("Check installation"), () =>
        {
            _checkedAt = DateTime.Now.ToString("HH:mm");
            RefreshAll(); // (scans the game again, then Verify and Compat on what is there now)
            return Task.CompletedTask;
        });
        Make(T("Change components"), () => { if (CanLeaveGame()) GoFromStatus(ComponentsStep); return Task.CompletedTask; });
        StatusList.Children.Add(buttons);
    }

    // the welcome page's one-click update: the installed components with a newer release, when Setup can download them now
    List<string> WelcomeUpdates() => _offline || _running || _game is null ? new List<string>() : Updates();

    // installed components with a newer release (only those: nothing new is added from the welcome page)
    List<string> Updates()
    {
        var l = new List<string>();
        if (_state is not { } s || _game is null) return l;
        if (s.ApexVersion is { } a && (a == "?" || Newer(Latest(Components.Apex), a) && !TestBuildNow())) l.Add(Components.Apex);
        if (s.Layer == Layer.Bridge && Newer(Latest(Components.Bridge), s.Installed(Components.Bridge) ?? "")) l.Add(Components.Bridge);
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
        _checkedAt = null;
        ManageInfo.IsOpen = false;
        if (_game?.Bin != _s3ppFor) _s3ppArchive = null;
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

    List<Finding> _found = new();
    void RefreshGame()
    {
        _state = _game is null ? null : Scan.Run(_game);
        // once per scan: Compat walks Mods and reads every Bin .asi, and the status and footer read it again
        _found = _state is null ? new() : Compat.Check(_game!, _state);
        GameDetails.Visibility = _state is null ? Visibility.Collapsed : Visibility.Visible;
        if (_state is not { } s) return;

        ProblemList.Children.Clear();
        var found = _found.Where(x => x.Suggest is null).ToList();
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
        // (the fixes run with Install / Apply, or Fix settings when nothing else changes)
        if (found.Count == 0)
            ProblemList.Children.Add(new TextBlock { Text = T("No incompatible settings found."), FontSize = 12, Foreground = (Brush)Application.Current.Resources["ApexSuccessBrush"] });
        SuggestList.Children.Clear();
        FillSuggestions(SuggestList, GraphicsInfo, onStatus: false);
    }

    // "Suggested for Apex Radiance": what Apex recommends in game, each applied only by its own click (or Apply all
    // suggestions), each in a restore point; a component's suggestion points to Components
    void FillSuggestions(Panel list, InfoBar resultBar, bool onStatus)
    {
        var sug = _found.Where(x => x.Suggest is not null).ToList();
        if (sug.Count == 0 || _game is not { } g) return;
        list.Children.Add(new TextBlock { Text = T("Suggested for Apex Radiance"), Style = (Style)Application.Current.Resources["ApexStrong"], FontSize = 13 });
        var idle = !_busy && !_running;
        // the files as they are now: the scan may be minutes old
        void Apply(IEnumerable<Finding> which, string label, params string[] args)
        {
            var keys = which.Select(x => (x.Suggest, string.Join("\n", x.Args))).ToHashSet();
            _ = Guarded(() =>
            {
                Engine.CheckWritable();
                foreach (var x in Compat.Check(g, Scan.Run(g)).Where(x => x.CanApply && keys.Contains((x.Suggest, string.Join("\n", x.Args))))) x.Fix!();
            }, T("Applied."), resultBar, label, keepChoices: true, args: args);
        }
        foreach (var x in sug)
        {
            var bar = new InfoBar
            {
                IsOpen = true, IsClosable = false, Title = T(x.Suggest!), Message = T(x.Text, TArgs(x.Args)),
                Severity = x.Severity == Severity.Warning ? InfoBarSeverity.Warning : InfoBarSeverity.Informational,
            };
            if (x.CanApply)
            {
                var b = new Button { Content = T("Apply"), IsEnabled = idle };
                b.Click += (_, _) => Apply(new[] { x }, "Apply suggestion: {0}", x.Suggest!);
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, T("Apply") + " " + T(x.Suggest!)); // (which suggestion)
                bar.ActionButton = b;
            }
            else if (onStatus && x.Suggest is "DXVK" or "Sims3SettingsSetter" or "RTX Remix bridge") // (a component to choose)
            {
                var b = new Button { Content = T("Change components"), IsEnabled = idle };
                b.Click += (_, _) => { if (CanLeaveGame()) GoFromStatus(ComponentsStep); };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, T("Change components") + " " + T(x.Suggest!));
                bar.ActionButton = b;
            }
            list.Children.Add(bar);
        }
        if (sug.Count(x => x.CanApply) > 1)
        {
            var all = new Button { Content = T("Apply all suggestions"), IsEnabled = idle };
            all.Click += (_, _) => Apply(sug.Where(x => x.CanApply), "Apply all suggestions");
            list.Children.Add(all);
        }
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
        if (_state is not { } s || _defaultsFor == _game) return;
        _defaultsFor = _game;
        // what is installed stays on; a first install takes what Apex recommends (DXVK, Sims3SettingsSetter); a game that already
        // has Apex keeps its renderer and its choices
        var first = s.ApexVersion is null;
        _renderer = first && s.Layer == Layer.Native ? RDxvk : RendererOf(s);
        S3ssToggle.IsOn = first || s.S3ss;
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
            Grid.SetColumn(remove, Grid.GetColumn(toggle));
            remove.Click += (_, _) => toggle.IsOn = !toggle.IsOn; // marks it for removal by Apply, or takes the mark back (OnToggle refreshes)
            if (toggle.Parent is Panel panel) panel.Children.Add(remove);
            _removeButtons[toggle] = remove;
        }
        toggle.Visibility = present ? Visibility.Collapsed : Visibility.Visible;
        remove.Visibility = present ? Visibility.Visible : Visibility.Collapsed;
        remove.Content = toggle.IsOn ? T("Remove") : T("Keep");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(remove, (string)remove.Content + " " + Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(toggle));
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
        // the bridge needs Vulkan 1.3; one already installed stays listed, so it can be kept or removed
        var tags = new List<string> { RDxvk, RBridge, RNative };
        if (BridgeBlocked && s.Layer != Layer.Bridge) tags.Remove(RBridge);
        if (s.Layer == Layer.Other) tags.Add(RKeep);
        var titles = tags.Select(t => t switch
        {
            RDxvk => T("Vulkan, DXVK (recommended)"),
            RBridge => T("Vulkan with more memory (RTX Remix bridge, experimental)"),
            RNative => T("Windows default (Direct3D 9)"),
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
        ApexDesc.Text = TestBuildNow() ? T("Test build {0}: kept as it is.", s.ApexVersion!)
            : s.ApexVersion is null ? T("Installs {0} into Game\\Bin.", latest)
            : s.ApexVersion == "?" ? T("Replaces the installed copy with {0}. Your settings stay.", latest)
            : Newer(latest, s.ApexVersion) ? T("Updates {0} to {1}. Your settings stay.", s.ApexVersion, latest)
            : T("{0} is installed and up to date: nothing to change.", s.ApexVersion);

        if (BridgeBlocked && _renderer == RBridge && s.Layer != Layer.Bridge) _renderer = RDxvk;
        FillRenderer(s);
        var cur = RendererOf(s);
        var changed = _renderer != cur;
        BridgeDetail.Text = _renderer switch
        {
            RDxvk => T("Fewer stutters; Apex Radiance is made and tested with it."),
            RBridge => T("Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."),
            RNative => T("Works, but Apex Radiance is tested with DXVK."),
            _ => T("Your d3d9.dll stays as it is."),
        };
        if (BridgeBlocked && _renderer != RBridge && _vulkan is { } vk)
            BridgeDetail.Text += "\n" + T("The RTX Remix bridge needs a graphics card with Vulkan 1.3 (found: {0}).",
                vk.Card is null ? "Vulkan —" : $"{vk.Card}, Vulkan {vk.Version}");

        // the game resets its graphics options when the card it sees changes: DXVK's TS3W profile and the bridge report a
        // GeForce GTX 580; the game's own D3D9 the real card (DXVK on TS3.exe: unverified, so treated as a change)
        string Card(string r) => r switch { RBridge => "580", RDxvk when g.Exe == "TS3W.exe" => "580", RNative => "real", _ => "?" };
        if (changed && (Card(_renderer) != Card(cur) || Card(cur) == "?"))
            SetInfo(GraphicsInfo, InfoBarSeverity.Warning, "",
                T("Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."));
        else GraphicsInfo.IsOpen = false;

        // An installed component shows a Remove button (Keep once marked: Apply removes it, and its pill says so); a missing one
        // its switch, which means Install. A switch that meant "keep" on an installed component, off meaning uninstall, read
        // as a control that did nothing.
        SetPill(S3ssPill, s.S3ss && !S3ssToggle.IsOn ? T("Will be removed") : Pill(Known(Components.S3ss), Latest(Components.S3ss), false));
        SetPill(ModPill, s.Modernization && !ModToggle.IsOn ? T("Will be removed") : Pill(Known(Components.Modernization), Latest(Components.Modernization), s.Ts3NativeWrongBuild));
        SetPill(MonoPill, s.MonoPatcher && !MonoToggle.IsOn ? T("Will be removed") : Pill(Known(Components.MonoPatcher), Latest(Components.MonoPatcher), false));
        SetPill(S3ppPill, S3ppPresent && !S3ppToggle.IsOn ? T("Will be removed") : S3ppPresent ? T("Installed") : _s3ppArchive is not null ? T("Ready to install") : T("Not installed"));
        ShowCard(S3ssToggle, s.S3ss);
        ShowCard(ModToggle, s.Modernization);
        ShowCard(MonoToggle, s.MonoPatcher);
        ShowCard(S3ppToggle, S3ppPresent);
        if (!S3ppPresent) S3ppToggle.Visibility = Visibility.Collapsed; // (missing: its download buttons below)
        S3ppGet.Visibility = !S3ppPresent && _s3ppArchive is null ? Visibility.Visible : Visibility.Collapsed;
        S3ppWait.Visibility = _s3ppWatch is not null ? Visibility.Visible : Visibility.Collapsed;
        S3ppWaitText.Text = T("Waiting for your download in {0}…", DownloadsFolder());
        ModToggle.IsEnabled = g.ModernizationSupported || s.Modernization;
        if (!g.ModernizationSupported && !s.Modernization) ModToggle.IsOn = false;
        ModDesc.Text = g.ModernizationSupported
            ? T("Fixes stutters, Sims getting stuck and crashes.")
            : T("Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47.");

        UpdateFooter();
    }

    bool TestBuildNow() => _game is { } g && _state is { } s && Verify.TestBuild(g, s, _latest.GetValueOrDefault(Repo[Components.Apex]));

    // the pill's colour follows its text; "Not installed" shows no pill (the switch says it)
    static void SetPill(TextBlock pill, string text)
    {
        pill.Text = text;
        var brush = text == T("Will be removed") || text == T("Wrong build") ? "ApexWarningBrush"
            : text.StartsWith(T("Update {0}", "")) ? "ApexAccentBrush"
            : text == T("Up to date") || text == T("Installed") ? "ApexSuccessBrush" : "ApexMutedBrush";
        pill.Foreground = (Brush)Application.Current.Resources[brush];
        ((FrameworkElement)pill.Parent).Visibility = text == T("Not installed") ? Visibility.Collapsed : Visibility.Visible;
    }

    string Pill(string? version, string? latest, bool wrong) =>
        wrong ? T("Wrong build") : version is null ? T("Not installed") : version.Length == 0 ? T("Installed")
        : Newer(latest, version) ? T("Update {0}", latest!) : T("Up to date");

    // What Apply does: the components to install or update, and the ones to remove. Only what differs from the game as it is.
    (List<string> Install, List<string> Remove) Plan()
    {
        var add = new List<string>();
        var del = new List<string>();
        if (_state is not { } s || _game is not { } g) return (add, del);
        if (s.ApexVersion is null || s.ApexVersion == "?" || Newer(Latest(Components.Apex), s.ApexVersion) && !TestBuildNow()) add.Add(Components.Apex);

        var cur = RendererOf(s);
        if (_renderer == cur)
        {
            if (cur == RBridge && Newer(Latest(Components.Bridge), s.Installed(Components.Bridge) ?? "")) add.Add(Components.Bridge);
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
        if (!S3ppPresent && _s3ppArchive is not null && _s3ppFor == g.Bin) add.Add(Components.S3pp);
        Optional(Components.Modernization, ModToggle.IsOn && (g.ModernizationSupported || s.Modernization), s.Modernization,
                 g.ModernizationSupported && (s.Ts3NativeWrongBuild || NeedsUpdate(Components.Modernization)));
        if (Engine.NeedsLoader(s, _renderer != cur && _renderer is RDxvk or RBridge)) add.Add(Components.Loader);
        return (add, del);
    }

    // ---------- Sims 3 Performance Patch: the player downloads it on Nexus Mods, Setup picks the file up ----------

    const string S3ppPage = "https://www.nexusmods.com/thesims3/mods/235";
    string? _s3ppArchive, _s3ppFor; // (the download and the Game\\Bin it is for)
    FileSystemWatcher? _s3ppWatch;
    bool S3ppPresent => _game is { } g && Scan.FindAsi(g.Bin, "Sims3Performance.asi") is not null;

    static string DownloadsFolder() =>
        Scan.PlayerShellFolder("{374DE290-123F-4565-9164-39C4925E467B}") ?? Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders")
            ?.GetValue("{374DE290-123F-4565-9164-39C4925E467B}") as string
        ?? System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");

    void OnS3ppDownload(object sender, RoutedEventArgs e)
    {
        _s3ppFor = _game?.Bin;
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
        _s3ppFor = _game?.Bin;
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
        if (ext is not (".zip" or ".asi" or ".7z" or ".rar")) return;
        for (int i = 0; i < 20; i++) // wait until the browser has closed the file
        {
            try { using (File.Open(path, FileMode.Open, FileAccess.Read, FileShare.None)) break; }
            catch (IOException) { await Task.Delay(500); }
        }
        // Firefox's empty placeholder: the finished download replaces it later (a rename)
        try { if (!fromPicker && new FileInfo(path).Length == 0) return; } catch (IOException) { return; }
        var found = ext is ".7z" or ".rar" ? null : await Task.Run(() => GitHubSource.S3ppFiles(path));
        DispatcherQueue.TryEnqueue(async () =>
        {
            if (!fromPicker && _s3ppWatch is null) return; // (the watch already took a file)
            if (found is null)
            {
                if (fromPicker || ext != ".asi" && System.IO.Path.GetFileName(path).Contains("Performance", StringComparison.OrdinalIgnoreCase))
                    await Message(T("Not the Performance Patch"), T("{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi.", System.IO.Path.GetFileName(path)));
                return;
            }
            _s3ppWatch?.Dispose();
            _s3ppWatch = null;
            _s3ppArchive = path;
            Activate(); // back in front of the browser
            // with Apex installed, at once; on a first install it goes with Install (Plan)
            if (_game is not null && _game.Bin == _s3ppFor && _state?.ApexVersion is not null && !_running && !_busy) await InstallAsync(new List<string> { Components.S3pp });
            else RefreshAll();
        });
    }

    static string Name(string c) => Components.Name(c);

    // ---------- install ----------

    async Task InstallAsync(List<string> comps, List<string>? remove = null)
    {
        if (_game is not { } g || _busy || _running) return;
        if (Preview is not null) { await Message(T("Preview"), T("This is a preview: nothing was installed.")); return; }
        var from = _step == InstallStep ? ComponentsStep : _step;
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
        var touched = false; // (a file was already changed: the error message points at Repair or remove)
        var progress = new Progress<Progress>(p =>
        {
            touched |= p.Fraction >= 0.5; // (the downloads, before 0.5, change nothing)
            InstallBar.Value = p.Fraction;
            // (downloads, before 0.5, install nothing: the first row stays active until the copies start)
            Mark(p.Arg is null || p.Fraction < 0.5 ?(p.Fraction >= 1 ? rows.Count : p.Fraction > 0.9 ? rows.Count - 1 : dels.Count) : dels.Count + order.IndexOf(p.Arg));
        });
        // one restore point for the whole change (the engine's own calls join it)
        RestorePoints.Scope? point = null;
        try
        {
            await Task.Run(async () =>
            {
                using (point = dels.Count == 0 && order.Count == 0 ? RestorePoints.Begin(g.Bin, "Fix incompatible settings")
                    : dels.Count == 0 ? RestorePoints.Begin(g.Bin, "Install {0}", string.Join(", ", order.Select(Name)))
                    : order.Count == 0 ? RestorePoints.Begin(g.Bin, "Remove {0}", string.Join(", ", dels.Select(Name)))
                    : RestorePoints.Begin(g.Bin, "Change components"))
                {
                    foreach (var c in dels) { touched = true; Engine.Remove(g, c); }
                    try { await Engine.InstallAsync(g, order, new GitHubSource { S3ppArchive = _s3ppArchive, Shown = _latest }, progress); }
                    finally { try { Directory.Delete(GitHubSource.TempRoot, true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
                }
            });
            _lastInstall = order;
            _lastRemove = dels;
            if (order.Contains(Components.S3pp)) _s3ppArchive = null;
            _defaultsFor = null; // (the switches follow the game as it is now)
            _busy = false;
            Show(DoneStep);
        }
        catch (Exception ex)
        {
            _busy = false;
            _defaultsFor = null;
            var net = ex is HttpRequestException or TaskCanceledException;
            var title = net ? T("Setup can't reach GitHub") : T("Setup could not finish");
            var text = (net ? T("Check your internet connection, then try again.") : T(ex.Message)) + (touched ? "\n\n" + T("Repair or remove sets the game right.") : "");
            // the files it changed before the error: offered back at once
            if (point?.Committed is { } p)
            {
                if (await ShowDialog(new ContentDialog { Title = title, Content = text, PrimaryButtonText = T("Undo these changes"), CloseButtonText = T("OK"),
                        DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot }) == ContentDialogResult.Primary)
                {
                    Show(Manage);
                    await RestoreAsync(p, ask: false);
                    return;
                }
            }
            else await Message(title, text);
            Show(from);
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

    List<string> _lastRemove = new();
    void RefreshDone()
    {
        var apex = _state?.ApexVersion is not null;
        DoneTitle.Text = apex ? T("You're all set") : T("Changes applied");
        DoneText.Text = apex ? T("Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu.") : "";
        DoneSummary.Children.Clear();
        foreach (var c in _lastInstall)
            DoneSummary.Children.Add(new TextBlock { Text = T("Installed: {0} {1}", Name(c), _state?.Installed(c) ?? ""), FontSize = 12, Foreground = (Brush)Application.Current.Resources["ApexSoftTextBrush"] });
        foreach (var c in _lastRemove)
            DoneSummary.Children.Add(new TextBlock { Text = T("Removed: {0}", Name(c)), FontSize = 12, Foreground = (Brush)Application.Current.Resources["ApexSoftTextBrush"] });
        DoneSummaryCard.Visibility = DoneSummary.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    // ---------- repair or remove ----------

    void RefreshManage()
    {
        if (_step == DoneStep) RefreshDone();
        ManageList.Children.Clear();
        RestoreList.Children.Clear();
        if (_game is not { } g || _state is not { } s) { ManageIntro.Text = T("Choose your game first."); return; }
        ManageIntro.Text = T("The Sims 3 \u00b7 {0}", g.Label);
        var m = s.Manifest;
        ManageRow(T("Repair or update"), T("Downloads and copies again everything installed here. Your settings stay."), T("Repair"), Repair);
        if (s.ApexVersion is not null)
            ManageRow("Apex Radiance " + s.ApexVersion, T("Removes ApexRadiance.asi. Your settings in Documents stay."), T("Remove"), () => RemoveAsync(Components.Apex));
        if (s.Layer == Layer.Bridge)
        {
            bool backup = m.Files.Any(f => f.Component == Components.Bridge && f.Backup is not null && System.IO.Path.GetFileName(f.Path) == "d3d9.dll");
            ManageRow(T("RTX Remix bridge"), GraphicsRemoval(backup), T("Remove"), () => RemoveAsync(Components.Bridge));
        }
        if (s.Layer == Layer.Dxvk)
            ManageRow("DXVK", GraphicsRemoval(m.Files.Any(f => f.Component == Components.Dxvk && f.Backup is not null)), T("Remove"), () => RemoveAsync(Components.Dxvk));
        if (S3ppPresent) ManageRow("Sims 3 Performance Patch", T("Removes Sims3Performance.asi and its .ini."), T("Remove"), () => RemoveAsync(Components.S3pp));
        if (s.MonoPatcher) ManageRow("MonoPatcher", T("Removes MonoPatcher.asi and its package."), T("Remove"), () => RemoveAsync(Components.MonoPatcher));
        if (s.S3ss) ManageRow("Sims3SettingsSetter", T("Removes Sims3SettingsSetter.asi."), T("Remove"), () => RemoveAsync(Components.S3ss));
        if (s.Modernization) ManageRow("Sims 3 Modernization", T("Removes ts3native and ts3fixes.package."), T("Remove"), () => RemoveAsync(Components.Modernization));
        ManageRow(T("Uninstall everything"), T("Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."), T("Uninstall"), UninstallAll, danger: true);

        foreach (var p in RestorePoints.List(g.Bin))
        {
            var when = p.Created.ToLocalTime().ToString("g", LangCulture());
            ManageRow(T("Before: {0}", T(p.Label, TArgs(p.Args))), p.FileCount == 1 ? T("{0} \u00b7 1 file", when) : T("{0} \u00b7 {1} files", when, p.FileCount),
                T("Restore"), () => RestoreAsync(p), list: RestoreList);
        }
        if (RestoreList.Children.Count == 0)
            RestoreList.Children.Add(new TextBlock { Text = T("No restore points yet. Setup makes one before every change."), Margin = new Thickness(18, 14, 18, 14),
                FontSize = 12, TextWrapping = TextWrapping.Wrap, Foreground = (Brush)Application.Current.Resources["ApexMutedBrush"] });
    }

    // dates in Setup's language, not the format Windows uses
    System.Globalization.CultureInfo LangCulture()
    {
        try { return System.Globalization.CultureInfo.GetCultureInfo(Bcp47(Lang)); }
        catch (System.Globalization.CultureNotFoundException) { return System.Globalization.CultureInfo.CurrentCulture; }
    }

    string GraphicsRemoval(bool backup) => backup ? T("Puts back the graphics files you had before.")
        : T("Goes back to the game's default graphics. Your in-game graphics settings may reset once.");

    void ManageRow(string title, string desc, string action, Func<Task> run, bool danger = false, Panel? list = null)
    {
        var restore = list is not null;
        list ??= ManageList;
        var row = new Grid { Padding = new Thickness(18, 14, 18, 14), ColumnSpacing = 14, BorderBrush = (Brush)Application.Current.Resources["ApexCardBorderBrush"], BorderThickness = new Thickness(0, list.Children.Count == 0 ? 0 : 1, 0, 0) };
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        text.Children.Add(new TextBlock { Text = desc, FontSize = 12, TextWrapping = TextWrapping.Wrap, Foreground = (Brush)Application.Current.Resources["ApexMutedBrush"] });
        var b = new Button { Content = action, MinWidth = 96, VerticalAlignment = VerticalAlignment.Center, IsEnabled = !_running && !_busy };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, action + " " + title + (restore ? ", " + desc : "")); // (two points can share a title)
        if (danger) { b.Background = new SolidColorBrush(ColorHelper.FromArgb(255, 0x2A, 0x16, 0x15)); b.Foreground = new SolidColorBrush(ColorHelper.FromArgb(255, 0xF2, 0xB6, 0xB2)); }
        b.Click += async (_, _) => await run();
        Grid.SetColumn(b, 1);
        row.Children.Add(text); row.Children.Add(b);
        list.Children.Add(row);
    }

    // the point and every newer one, back as they were; the result on the bar of the screen it was started from
    async Task RestoreAsync(RestorePoint p, bool ask = true, InfoBar? bar = null)
    {
        if (_game is not { } g || _busy || _running) return;
        if (ask && !await Confirm(T("Restore this point?"), T("Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."), T("Restore"))) return;
        await Guarded(() =>
        {
            if (RestorePoints.Restore(g.Bin, p) is { Count: > 0 } failed)
                throw new IOException(T("Setup could not put back {0}.", string.Join(", ", failed)));
        }, T("Changes undone."), bar);
    }

    async Task RemoveAsync(string c)
    {
        if (_game is not { } g || _busy || _running) return;
        if (!await Confirm(T("Remove {0}?", Name(c)), T("Close the game first. Setup removes the files and puts back the ones it replaced."), T("Remove"))) return;
        await Guarded(() => Engine.Remove(g, c), T("{0} removed.", Name(c)), label: "Remove {0}", args: Name(c));
    }

    async Task UninstallAll()
    {
        if (_game is not { } g || _busy || _running) return;
        if (!await Confirm(T("Uninstall everything?"), T("Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."), T("Uninstall"))) return;
        await Guarded(() => Engine.RemoveAll(g), T("Everything Setup installed was removed."), label: "Uninstall everything");
    }

    Task Repair()
    {
        if (_state is not { } s) return Task.CompletedTask;
        var l = new List<string>();
        if (!TestBuildNow()) l.Add(Components.Apex);
        if (s.Layer == Layer.Bridge) l.Add(Components.Bridge);
        if (s.Layer == Layer.Dxvk && s.Installed(Components.Dxvk) is not null) l.Add(Components.Dxvk);
        if (s.S3ss) l.Add(Components.S3ss);
        if (s.MonoPatcher) l.Add(Components.MonoPatcher);
        if (s.Modernization && _game!.ModernizationSupported) l.Add(Components.Modernization);
        if (Engine.NeedsLoader(s, false)) l.Add(Components.Loader);
        return InstallAsync(l);
    }

    // a change made in place (a removal, the settings' fixes): the buttons off while it runs, its result on the bar of the
    // screen it was started from (Repair or remove's by default); false when it did not run or failed. label: the restore point
    // taken of what work changes (an English key with args, as the restore list shows it); keepChoices: the Components choices not
    // yet installed stay (a suggestion changes the game's files, not what to install)
    async Task<bool> Guarded(Action work, string done, InfoBar? bar = null, string? label = null, bool keepChoices = false, params string[] args)
    {
        if (_busy || _running || _game is not { } g) return false;
        bar ??= ManageInfo;
        _busy = true;
        RefreshAll();
        var ok = false;
        string? error = null;
        RestorePoints.Scope? point = null;
        try
        {
            await Task.Run(() => { using (point = label is null ? null : RestorePoints.Begin(g.Bin, label, args)) work(); });
            ok = true;
        }
        catch (Exception ex) { error = T(ex.Message); }
        finally
        {
            _busy = false;
            if (!keepChoices) _defaultsFor = null;
            RefreshAll();
        }
        // after the refresh, which rewrites the screens' bars; Components' bar open again holds the graphics-reset warning for
        // the choices kept, so it stays (the suggestion's card going shows the result)
        if (ok) { if (!(bar == GraphicsInfo && GraphicsInfo.IsOpen)) SetInfo(bar, InfoBarSeverity.Success, done, ""); }
        else if (point?.Committed is { } p) SetInfo(bar, InfoBarSeverity.Error, T("Setup could not finish"), error!, T("Undo these changes"),
            // a change made since then would be undone too: asked first
            () => _ = RestoreAsync(p, ask: RestorePoints.List(g.Bin).FirstOrDefault()?.Dir != p.Dir, bar));
        else SetInfo(bar, InfoBarSeverity.Error, T("Setup could not finish"), error!);
        return ok;
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

    async Task Message(string title, string text) =>
        await ShowDialog(new ContentDialog { Title = title, Content = text, CloseButtonText = T("OK"), XamlRoot = Content.XamlRoot });

    async Task<bool> Confirm(string title, string text, string yes) =>
        await ShowDialog(new ContentDialog { Title = title, Content = text, PrimaryButtonText = yes, CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Close, XamlRoot = Content.XamlRoot })
            == ContentDialogResult.Primary;

    // WinUI allows one open ContentDialog: a newer one closes the open one (a Confirm closed so reads as Cancel)
    ContentDialog? _dialog;
    Task<ContentDialogResult>? _dialogShown;
    async Task<ContentDialogResult> ShowDialog(ContentDialog d)
    {
        while (_dialog is { } open) { open.Hide(); await _dialogShown!; }
        _dialog = d;
        d.Language = Root.Language; // (a dialog is a popup: it does not inherit the window's language)
        _dialogShown = d.ShowAsync().AsTask();
        try { return await _dialogShown; }
        finally { if (_dialog == d) _dialog = null; }
    }

    // the language button's list: Automatic, then the 21 languages in their own names; a check on the choice and on the
    // language shown
    void OnLanguage(object sender, RoutedEventArgs e)
    {
        var saved = SavedChoice();
        var menu = new MenuFlyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedRight };
        var auto = new ToggleMenuFlyoutItem { Text = T("Automatic"), IsChecked = saved is null, Language = Root.Language };
        auto.Click += (_, _) => SetLanguage(null);
        menu.Items.Add(auto);
        menu.Items.Add(new MenuFlyoutSeparator());
        foreach (var (code, name) in Languages)
        {
            var item = new ToggleMenuFlyoutItem { Text = name, IsChecked = code == Lang, Language = Bcp47(code) };
            item.Click += (_, _) => SetLanguage(code);
            menu.Items.Add(item);
        }
        menu.ShowAt(LangButton);
    }

    // every screen at once: the static texts by Localize, the ones set in code by the refresh
    void SetLanguage(string? choice)
    {
        SaveChoice(choice);
        Lang = choice ?? SystemLanguage(System.Globalization.CultureInfo.CurrentUICulture);
        Root.Language = Bcp47(Lang);
        Localize();
        LicenceList.Children.Clear();
        RefreshAll();
    }

    void Localize() => Strings.Localize(Root);
}
