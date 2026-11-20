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
    const int Welcome = 0, GameStep = 1, ComponentsStep = 2, InstallStep = 3, DoneStep = 4, Manage = 5, About = 6, RendererStep = 7;
    readonly UIElement[] _screens;
    readonly List<GameItem> _games = new();
    readonly Dictionary<string, Release?> _latest = new();
    IReadOnlyList<Release>? _modRecent = []; // Sims 3 Modernization's recent releases, its -exp. ones included; null: the listing failed
    bool _modExp; // its experimental channel chosen
    readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(2) };
    int _step;
    Game? _game, _defaultsFor;
    State? _state;
    bool _offline, _busy, _syncNav, _running;
    List<string> _lastInstall = new();

    static readonly Dictionary<string, string> Repo = new()
    {
        [Components.Apex] = Releases.Apex, [Components.Bridge] = Releases.Apex, [Components.Loader] = Releases.AsiLoader, [Components.Dxvk] = Releases.Dxvk, [Components.MonoPatcher] = Releases.MonoPatcher,
        [Components.S3ss] = Releases.S3ss, [Components.Modernization] = Releases.Modernization, [Components.RestartHost] = Releases.Apex,
    };

    public MainWindow()
    {
        InitializeComponent();
        // (a test run can pick the theme; otherwise it is Windows')
        if (Environment.GetEnvironmentVariable("APEX_LAUNCHER_THEME") is { } pick) Root.RequestedTheme = pick == "light" ? ElementTheme.Light : ElementTheme.Dark;
        _light = Root.ActualTheme == ElementTheme.Light;
        ThemeChrome();
        Root.ActualThemeChanged += (_, _) => ThemeChanged();
        _ui.ColorValuesChanged += (_, _) => DispatcherQueue.TryEnqueue(ThemeChanged);
        // status bars in a dark tint of their colour (the window's own resources: read before any bar draws), not the default
        // olive and brown
        // the log's context line: the game, its edition and version, the renderer and the loaders Setup found
        App.Context = () => _game is not { } cg ? "no game chosen"
            : $"game {cg.Root} ({cg.Label} {cg.Version}, {cg.Exe}); renderer {_state?.Layer.ToString() ?? "?"}; loaders {(Scan.Loaders(cg.Bin) is { Count: > 0 } ls ? string.Join(", ", ls) : "none")}";
        Root.SizeChanged += (_, _) => QueueAudit();
        // (the version under the sidebar only fits it expanded)
        Nav.DisplayModeChanged += (_, _) => ShowSetupVersion();
        _screens = new UIElement[] { ScreenWelcome, ScreenGame, ScreenComponents, ScreenInstall, ScreenDone, ScreenManage, ScreenAbout, ScreenRenderer,
            ScreenHome, ScreenMods, ScreenWorlds, ScreenSaves, ScreenHealth, ScreenNews, ScreenSettings, ScreenGallery };
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(DragArea);
        // 1180 x 760 at the monitor's scale, never past its work area (a small laptop screen)
        void Size(double scale)
        {
            var area = Microsoft.UI.Windowing.DisplayArea.GetFromWindowId(AppWindow.Id, Microsoft.UI.Windowing.DisplayAreaFallback.Primary).WorkArea;
            AppWindow.Resize(new SizeInt32(Math.Min((int)(1180 * scale), area.Width), Math.Min((int)(760 * scale), area.Height)));
        }
        Size(GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0);
        ((FrameworkElement)Content).Loaded += (_, _) => Size(Content.XamlRoot.RasterizationScale); // the monitor's real scale
        if (AppWindow.Presenter is OverlappedPresenter p) { p.PreferredMinimumWidth = 760; p.PreferredMinimumHeight = 560; }
        var ico = System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "apex.ico");
        if (File.Exists(ico)) AppWindow.SetIcon(ico);
        SetupVersion.Text = "Apex Launcher " + Assembly.GetExecutingAssembly().GetName().Version?.ToString(3);

        // the player's choice (the title bar's language button), else Windows' display language, as the mod's menu
        Lang = Environment.GetEnvironmentVariable("APEX_LAUNCHER_LANG") ?? SavedChoice() ?? SystemLanguage(System.Globalization.CultureInfo.CurrentUICulture); // (the variable: UI tests in one language)
        Root.Language = Bcp47(Lang);
        // every screen's buttons follow the game starting or closing (the status and Manage rows are built with that state)
        _timer.Tick += (_, _) => { var r = Games.IsRunning(); if (r != _running) { _running = r; RefreshAll(); } };
        // closing in the middle of an install or a removal would leave the game half changed
        AppWindow.Closing += (w, a) =>
        {
            if (!_busy) { _tray?.Dispose(); return; }
            a.Cancel = true;
            _ = Message(T("The launcher is applying changes"), T("Wait until it finishes before closing."));
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
        if (Environment.GetCommandLineArgs().Skip(1).FirstOrDefault() is { } arg && !arg.StartsWith("--") && Games.FromFolder(arg) is { } given
            && !_games.Any(x => x.Game.Bin.Equals(given.Bin, StringComparison.OrdinalIgnoreCase)))
            _games.Insert(0, Item(given));
        // the administrator window (--maintain): the game it was opened for
        if (Elevation.Parse(Environment.GetCommandLineArgs()).Bin is { } mb && Games.FromFolder(mb) is { } mg)
        {
            _games.RemoveAll(x => x.Game.Bin.Equals(mg.Bin, StringComparison.OrdinalIgnoreCase));
            _games.Insert(0, Item(mg));
        }
        GameList.ItemsSource = _games;
        if (_games.Count > 0) GameList.SelectedIndex = 0;
        NoGames.Visibility = _games.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        _running = Games.IsRunning();
        _timer.Start();
        Localize();
        RefreshGame();
        Show(Maintenance ? MaintenanceScreen(Elevation.Parse(Environment.GetCommandLineArgs()).Screen) : FirstInstall ? Welcome : Home);
        _ = DetectVulkanAsync();
        await LoadReleasesAsync();
        if (AuditFile is not null && Environment.GetEnvironmentVariable("APEX_LAUNCHER_AUDIT_TOUR") == "1") _ = AuditTourAsync();
        else { await AskInstallModeAsync(); await OpenPendingAsync(); }
        // (the launcher's choices, applied as if Apply was clicked: the player already did, before Windows asked)
        if (Maintenance && TakeChoices()) OnNext(NextButton, new RoutedEventArgs());
        await CheckSetupUpdateAsync();
    }

    // A newer Apex Radiance Setup in the latest Apex release is downloaded and checked, then offered on the start page;
    // Update Setup puts it in place of this exe and starts it. Any failure leaves this Setup working as it is.
    async Task CheckSetupUpdateAsync()
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1" || Preview is not null || Environment.ProcessPath is not { } self
            || _latest.GetValueOrDefault(Releases.Apex) is not { } apex) return;
        try
        {
            if (await SelfUpdate.DownloadIfNewerAsync(apex, self) is not { } file) return;
            var v = System.Diagnostics.FileVersionInfo.GetVersionInfo(file).FileVersion is { } fv && Version.TryParse(fv, out var pv) ? pv.ToString(3) : "";
            SetInfo(SetupUpdateInfo, InfoBarSeverity.Success, T("A new Apex Launcher is available"),
                T("Apex Launcher {0} was downloaded and checked. Update the launcher, then it opens again.", v), T("Update the launcher"), () =>
                {
                    if (_busy) return; // (an install in progress finishes first)
                    try
                    {
                        SelfUpdate.ReplaceAndStart(file, self, LauncherInstall.HandoverArgs(Environment.GetCommandLineArgs().Skip(1)));
                        Application.Current.Exit();
                    }
                    catch (Exception e)
                    {
                        App.Log(e);
                        SetupUpdateInfo.IsOpen = false;
                        // (the new exe quarantined or blocked by an antivirus: said as such)
                        var shown = e is FileNotFoundException || e is System.ComponentModel.Win32Exception { NativeErrorCode: 2 or 225 or 226 } || Engine.IsVirusBlock(e)
                            ? Engine.Antivirus(SelfUpdate.AssetName, Path.GetDirectoryName(file) ?? "") : e;
                        _ = ShowDialog(new ContentDialog { Title = T("The launcher could not update itself"), Content = ErrorContent(ErrorText(shown)), CloseButtonText = T("OK"), XamlRoot = Content.XamlRoot });
                    }
                });
        }
        catch (Exception e) { App.Log(e); }
    }

    // The card's Vulkan version and the DXVK extensions its driver lacks, asked once off the UI thread (loading the drivers
    // takes a moment); null until known or when the driver could not be asked (DXVK and the bridge then stay available)
    (Version? Version, string? Card, string[] Missing)? _vulkan;
    bool OldCard => _vulkan is { } vk && (vk.Version is null || vk.Version < Gpu.Needed);
    bool OldDriver => !OldCard && _vulkan is { Missing.Length: > 0 };
    bool VulkanBlocked => OldCard || OldDriver; // DXVK, and the bridge drawing through it, close the game at start
    string CardFound => _vulkan is not { Card: { } card } vk ? "Vulkan —" : $"{card}, Vulkan {vk.Version}";
    string MissingList => string.Join(", ", _vulkan?.Missing ?? Array.Empty<string>());

    // DXVK or the bridge installed where it cannot start: the game closes at start (an old driver: the vendor's driver page)
    List<Finding> GpuFindings(State s)
    {
        var name = s.Layer switch { Layer.Dxvk => "DXVK", Layer.Bridge => "RTX Remix bridge", _ => null };
        if (name is null || !VulkanBlocked) return new();
        return new()
        {
            OldDriver
                ? new(Severity.Error, "{0} cannot start with this graphics driver (missing: {1}), so the game closes at start. Update the graphics driver, or choose Windows default (Direct3D 9) in Change components.",
                    new[] { name, MissingList }, Link: Gpu.DriverPage(_vulkan?.Card))
                : new(Severity.Error, "{0} cannot start on this graphics card ({1}), so the game closes at start. Choose Windows default (Direct3D 9) in Change components.",
                    new[] { name, CardFound }),
        };
    }

    async Task DetectVulkanAsync()
    {
        try { _vulkan = await Task.Run(Gpu.Vulkan); }
        catch (Exception e) { App.Log(e); return; }
        RefreshComponents();
        RefreshStatus(); // (their DXVK warning needs the card)
        FillProblems();
    }

    static readonly string? Preview = Environment.GetEnvironmentVariable("APEX_SETUP_PREVIEW_LATEST") is { Length: > 0 } pv ? pv : null;
    SetupException? _releaseError; // (GitHub's limit at the release check: said as such, not as "no internet")
    bool _loading = true; // the release check has not answered yet: nothing is installed meanwhile
    async Task LoadReleasesAsync()
    {
        _offline = false;
        _releaseError = null;
        _loading = true;
        RefreshAll();
        // each repo on its own: an optional mod's repo that fails only hides that mod's update (its install fetches it again)
        await Task.WhenAll(Repo.Values.Distinct().Select(async r =>
        {
            try { _latest[r] = await Releases.LatestAsync(r); }
            catch (Exception e) { App.Log(e); _latest.Remove(r); if (e is SetupException se) _releaseError = se; }
        }).Append(Task.Run(async () =>
        {
            try { _modRecent = await Releases.RecentAsync(Releases.Modernization, experimental: true); }
            catch (Exception) { _modRecent = null; } // (not "no experimental build": nothing changes channel on it)
        })));
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
    string? Latest(string c) => c == Components.Modernization ? ModRelease()?.Version
        : Repo.TryGetValue(c, out var repo) && _latest.GetValueOrDefault(repo) is { } r
        && (c != Components.Bridge || r.Assets.Any(a => a.Name == "Apex-Radiance-RTX-Remix-bridge.zip") || GitHubSource.HasManualZip(r) is not null) ? r.Version : null;

    // Sims 3 Modernization's release for the chosen channel and this edition (no update to a release without its build)
    Release? ModRelease() => _game is null ? _latest.GetValueOrDefault(Releases.Modernization)
        : Engine.PickModernization(_latest.GetValueOrDefault(Releases.Modernization), _modRecent, _game.Edition, _modExp);

    // the installed copy is from the other channel than the chosen one's release: installed again (its ts3native.ini too)
    bool ModChannelChange() => Known(Components.Modernization) is { Length: > 0 } v && ModRelease() is { } r && Releases.IsExperimental(v) != Releases.IsExperimental(r.Tag);

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
            Components.RestartHost => (s.RestartHost, "", ""),
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
        if (step != GalleryScreen) FullBleed(false);
        Scroller.Visibility = step == ModsScreen ? Visibility.Collapsed : Visibility.Visible; // (Mods scrolls in its own panes)
        QueueAudit();
        Animate(_screens[step]);
        Scroller.ChangeView(null, 0, null, true);
        SyncNav();
        RefreshAll();
    }

    // the sidebar shows the screen that is open (also after a refused jump); Install and Done have no item
    void SyncNav()
    {
        _syncNav = true;
        var tag = (_step == RendererStep ? ComponentsStep : _step is Welcome or Manage or About or GameStep ? (_step == Welcome ? Home : SettingsScreen) : _step).ToString();
        Nav.SelectedItem = Nav.MenuItems.Concat(Nav.FooterMenuItems).OfType<NavigationViewItem>().FirstOrDefault(n => (string)n.Tag == tag);
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

    // the side pane: wide windows keep it open unless the player folded it to its icons with the toggle (remembered);
    // narrow ones fold it on their own, which is not a choice to remember
    void OnNavLoaded(object sender, RoutedEventArgs e) { if (_settings.NavCompact) Nav.IsPaneOpen = false; ShowSetupVersion(); }
    void OnNavPaneOpening(NavigationView sender, object args) => NavFolded(false);
    void OnNavPaneClosing(NavigationView sender, NavigationViewPaneClosingEventArgs args) => NavFolded(true);
    void NavFolded(bool folded)
    {
        SetupVersion.Visibility = !folded && Nav.DisplayMode == NavigationViewDisplayMode.Expanded ? Visibility.Visible : Visibility.Collapsed;
        if (Nav.DisplayMode != NavigationViewDisplayMode.Expanded || _settings.NavCompact == folded) return;
        _settings.NavCompact = folded;
        _settings.Save();
    }
    // (the version line fits only the open pane: folded to icons it would wrap a letter at a time)
    void ShowSetupVersion() => SetupVersion.Visibility = Nav.IsPaneOpen && Nav.DisplayMode == NavigationViewDisplayMode.Expanded ? Visibility.Visible : Visibility.Collapsed;

    void OnNavSelection(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (_syncNav || args.SelectedItem is not NavigationViewItem item) return;
        var to = int.Parse((string)item.Tag);
        // steps open in order; Install and Done only through the buttons; nothing moves while Setup works
        bool ok = !_busy && (to is Welcome or About or >= Home || to == GameStep || (to == ComponentsStep && CanLeaveGame()) || to == Manage);
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
        if (_step == RendererStep) { Show(ComponentsStep); return; }
        var to = _backTo >= 0 ? _backTo : _step == GameStep && !FirstInstall ? SettingsScreen : Math.Max(Welcome, _step - 1);
        _backTo = -1;
        Show(to);
    }

    async void OnNext(object sender, RoutedEventArgs e)
    {
        switch (_step)
        {
            case Welcome:
                if (_updated is not null) { _updated = null; RefreshAll(); } // (the update's result read: back to the status)
                else if (WelcomeUpdates() is { Count: > 0 } ups) await InstallAsync(ups, inline: true); // (an update: one click, in its card)
                else Show(GameStep);
                break;
            case RendererStep: Show(ComponentsStep); break;
            case GameStep: if (CanLeaveGame()) Show(ComponentsStep); break;
            case ComponentsStep:
            {
                var (add, del) = Plan();
                if (_offline && add.Count > 0) { await LoadReleasesAsync(); break; } // (the button reads Try again)
                var fixes = _game is not null && _state is not null && Compat.Check(_game, _state).Any(x => x.CanFix);
                // (nothing to do: the button is off and says so; only driver or frame limit changes: no component work, no fixes)
                if (add.Count + del.Count > 0 || fixes) await InstallAsync(add, del);
                else if (SettingChanges().Count > 0) await InstallAsync(add, del, settingsOnly: true);
                break;
            }
            case DoneStep: if (Maintenance) Close(); else Show(FirstInstall ? Welcome : Home); break;
        }
    }

    void UpdateFooter()
    {
        // a first install opens on its own welcome over the window; its steps show their bar beside the sidebar
        FirstRun.Visibility = FirstInstall && _step == Welcome && !_busy ? Visibility.Visible : Visibility.Collapsed;
        // the glow moves only while it shows, and not when Windows' animation effects are off (started once, not on every refresh)
        var glow = FirstRun.Visibility == Visibility.Visible && new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;
        if (glow != _glowOn) { _glowOn = glow; if (glow) FirstRunGlow.Begin(); else FirstRunGlow.Stop(); }
        BackButton.Visibility = _step is GameStep or RendererStep || _step == ComponentsStep && FirstInstall ? Visibility.Visible : Visibility.Collapsed; // (in the launcher Components has no step before it)
        // a game with Apex and nothing to update: the status card's own buttons are the actions, no wizard to go into
        var settled = _step == Welcome && _state?.ApexVersion is not null && _game is not null && WelcomeUpdates().Count == 0 && _updated is null;
        NextButton.Visibility = !settled && _step is Welcome or GameStep or ComponentsStep or DoneStep or RendererStep ? Visibility.Visible : Visibility.Collapsed;
        var (wa, wd) = _step == ComponentsStep ? Plan() : (new List<string>(), new List<string>());
        var settings = _step == ComponentsStep ? SettingChanges().Select(c => c.Label).ToList() : new List<string>();
        var work = wa.Concat(wd).Concat(settings).ToList();
        var fixes = _step == ComponentsStep && _found.Any(x => x.CanFix);
        NextText.Text = _step switch
        {
            Welcome => _updated is not null ? T("Done") : WelcomeUpdates() is { Count: > 0 } u ? (u.Count == 1 ? T("Update 1 item") : T("Update {0} items", u.Count))
                : _state?.ApexVersion is not null ? T("Next") : T("Get started"),
            RendererStep => T("Use {0}", RendererShort(_renderer)),
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
            GameStep or ComponentsStep when _running => T("Close The Sims 3, its launcher, the restart host (ts3wrap.exe) and the RTX Remix bridge (NvRemixBridge.exe) to continue."),
            ComponentsStep when _loading => T("Checking the latest versions\u2026"),
            ComponentsStep when _offline && wa.Count > 0 && _releaseError is { } le => T(le.Key, le.Args),
            ComponentsStep when _offline && wa.Count > 0 => T("The launcher needs the internet to download the files."),
            ComponentsStep when work.Count == 0 && !fixes => T("Everything is installed and up to date."),
            ComponentsStep when _state?.ApexVersion is null => T("Nothing changes until you click Install."),
            // what Apply will change in the drivers and Apex's settings (the components show theirs on their rows)
            ComponentsStep when settings.Count > 0 => T("To apply: {0}", string.Join(", ", settings)),
            _ => "",
        };
        NavComponents.IsEnabled = CanLeaveGame() && !_busy;
        // an empty bar (no buttons, no note) reads as broken: the page takes its room instead
        Footer.Visibility = BackButton.Visibility == Visibility.Visible || NextButton.Visibility == Visibility.Visible || FooterNote.Text.Length > 0
            ? Visibility.Visible : Visibility.Collapsed;
    }

    // ---------- first install ----------

    // no Apex Radiance in the chosen game (or no game yet): the welcome covers the window and the steps show their bar
    bool FirstInstall => _state?.ApexVersion is null;
    bool _glowOn;

    void RefreshFirstRun()
    {
        var latest = Latest(Components.Apex);
        FirstTitle.Text = "Apex Radiance" + (latest is null ? "" : " " + latest);
        FirstGame.Text = _game is { } g ? T("The Sims 3 \u00b7 {0} found", g.Label) : T("The launcher found no install. Browse for the game folder.");
        FirstGameIcon.Visibility = FirstNotThis.Visibility = _game is null ? Visibility.Collapsed : Visibility.Visible;
        FirstGamePath.Text = _game?.Root ?? "";
        FirstGamePath.Visibility = FirstGameIcon.Visibility;
        Steps(StepsGame, 1);
        Steps(StepsComponents, 2);
        Steps(StepsRenderer, 2);
        Steps(StepsInstall, 3);
    }

    void OnFirstStart(object sender, RoutedEventArgs e) => Show(GameStep);
    void OnGlowPanelSize(object sender, SizeChangedEventArgs e) =>
        GlowPanel.Clip = new RectangleGeometry { Rect = new Windows.Foundation.Rect(0, 0, e.NewSize.Width, e.NewSize.Height) };
    void OnNotThisGame(object sender, RoutedEventArgs e) => Show(GameStep);

    // Game, Components, Install: numbered, the done ones checked, the current one in the accent
    void Steps(StackPanel host, int current)
    {
        host.Children.Clear();
        host.Visibility = FirstInstall ? Visibility.Visible : Visibility.Collapsed;
        if (!FirstInstall) return;
        var names = new[] { T("Game"), T("Components"), T("Install") };
        for (int i = 1; i <= names.Length; i++)
        {
            if (i > 1) host.Children.Add(new Border { Width = 40, Height = 1, VerticalAlignment = VerticalAlignment.Center, Background = App.Brush("ApexCardBorderBrush") });
            var done = i < current;
            var on = i == current;
            var dot = new Border
            {
                Width = 22, Height = 22, CornerRadius = new CornerRadius(11),
                Background = on ? App.Brush("ApexAccentBrush") : done ? Res("ApexSuccessSoftBrush") : null,
                BorderBrush = App.Brush("ApexCardBorderBrush"), BorderThickness = new Thickness(on || done ? 0 : 1),
                Child = done
                    ? new FontIcon { Glyph = "\uE73E", FontSize = 11, Foreground = App.Brush("ApexSuccessBrush") }
                    : new TextBlock { Text = i.ToString(), FontSize = 11, FontWeight = FontWeights.Bold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
                                      Foreground = on ? Res("TextOnAccentFillColorPrimaryBrush") : App.Brush("ApexMutedBrush") },
            };
            var label = new TextBlock
            {
                Text = names[i - 1], FontSize = 13, VerticalAlignment = VerticalAlignment.Center, FontWeight = on ? FontWeights.SemiBold : FontWeights.Normal,
                Foreground = App.Brush(done ? "ApexSuccessBrush" : on ? "ApexSoftTextBrush" : "ApexMutedBrush"),
            };
            var step = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            step.Children.Add(dot);
            step.Children.Add(label);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(step, T("Step {0} of {1}: {2}", i, names.Length, names[i - 1]));
            host.Children.Add(step);
        }
    }

    // ---------- the update card (Welcome, a game with Apex) ----------

    readonly HashSet<string> _skipUpdate = new();          // updates the player unticked
    bool _inline;                                           // an update runs in the card
    (List<(string Name, string From, string To)> Rows, RestorePoint? Point)? _updated; // the last update's result, until Done

    void RefreshUpdateCard()
    {
        if (_inline) return; // (the install fills the card itself)
        var ups = _state?.ApexVersion is not null && !_offline && !_loading ? Updates() : new List<string>();
        var show = _game is not null && (_updated is not null || ups.Count > 0);
        UpdateCard.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        WelcomeStatus.Visibility = show ? Visibility.Collapsed : WelcomeStatus.Visibility;
        UpdateList.Children.Clear();
        if (!show || _game is not { } g) return;
        var muted = App.Brush("ApexMutedBrush");
        if (_updated is { } done)
        {
            CardHeader(UpdateList, "\uE73E", "ApexSuccessBrush", T("Everything is updated"), T("Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."));
            foreach (var (name, from, to) in done.Rows) CardRow(UpdateList, new TextBlock { Text = name, FontSize = 14 }, from.Length > 0 ? $"{from} \u2192 {to}" : to, "ApexSuccessBrush");
            if (done.Point is { } p)
            {
                var undo = new Button { Content = T("Undo this update"), IsEnabled = !_busy && !_running };
                undo.Click += async (_, _) => { await RestoreAsync(p, ask: true, WelcomeInfo); _updated = null; RefreshAll(); };
                CardStrip(UpdateList, T("Something looks wrong in the game? This puts back everything as it was before this update."), undo);
            }
            return;
        }
        CardHeader(UpdateList, "\uE896", "ApexAccentBrush", ups.Count == 1 ? T("1 update for The Sims 3 \u00b7 {0}", g.Label) : T("{0} updates for The Sims 3 \u00b7 {1}", ups.Count, g.Label),
            T("The launcher makes a restore point before it changes any file, so you can undo this."));
        foreach (var c in ups)
        {
            var box = new CheckBox { Content = Name(c), IsChecked = !_skipUpdate.Contains(c), MinWidth = 0, IsEnabled = !_busy };
            box.Checked += (_, _) => { _skipUpdate.Remove(c); UpdateFooter(); };
            box.Unchecked += (_, _) => { _skipUpdate.Add(c); UpdateFooter(); };
            var left = new StackPanel { Spacing = 2 };
            left.Children.Add(box);
            if (c == Components.Apex && _latest.GetValueOrDefault(Releases.Apex) is { } rel)
                left.Children.Add(new HyperlinkButton { Content = T("See what's new"), Padding = new Thickness(28, 0, 0, 0),
                    NavigateUri = new Uri($"https://github.com/{Releases.Apex}/releases/tag/{Uri.EscapeDataString(rel.Tag)}") });
            CardRow(UpdateList, left, $"{(Known(c) is { Length: > 0 } v ? v : "?")} \u2192 {Latest(c)}", "ApexSoftTextBrush");
        }
        if (_running) CardStrip(UpdateList, T("Close The Sims 3, its launcher, the restart host (ts3wrap.exe) and the RTX Remix bridge (NvRemixBridge.exe) to update."), null);

    }

    // ---------- card rows (the update card, the status card) ----------

    static Brush Res(string key) => App.Brush(key);

    // Windows' light or dark and its accent colour: the window follows them as they change
    static bool _light { get => App.Light; set => App.Light = value; }
    readonly Windows.UI.ViewManagement.UISettings _ui = new();
    static readonly Windows.UI.ViewManagement.UISettings Ui = new();
    static Windows.UI.Color Accent(Windows.UI.ViewManagement.UIColorType kind = Windows.UI.ViewManagement.UIColorType.Accent) => Ui.GetColorValue(kind);
    // a colour for the theme in use: its dark form, else its light one
    static Windows.UI.Color Pick(uint dark, uint light) { var c = _light ? light : dark; return Windows.UI.Color.FromArgb((byte)(c >> 24), (byte)(c >> 16), (byte)(c >> 8), (byte)c); }
    // the accent as text and icons: light on dark, deep on light
    static Windows.UI.Color AccentInk => Accent(_light ? Windows.UI.ViewManagement.UIColorType.AccentDark2 : Windows.UI.ViewManagement.UIColorType.AccentLight2);
    static Windows.UI.Color Mix(Windows.UI.Color a, Windows.UI.Color b, double k) =>
        Windows.UI.Color.FromArgb(255, (byte)(a.R + (b.R - a.R) * k), (byte)(a.G + (b.G - a.G) * k), (byte)(a.B + (b.B - a.B) * k));

    void ThemeChanged()
    {
        var light = Root.ActualTheme == ElementTheme.Light;
        _light = light;
        ThemeChrome();
        RefreshAll();
        if (_step == GalleryScreen) BuildGallery();
    }

    // the status bars in a tint of their colour and the window's caption buttons, for the theme in use
    void ThemeChrome()
    {
        foreach (var (key, dark, light) in new[] { ("Success", 0xFF14231Bu, 0xFFE4F3EAu), ("Warning", 0xFF2A2214u, 0xFFFBF1DFu), ("Error", 0xFF2C1718u, 0xFFFBE6E5u), ("Informational", 0xFF1E1E1Eu, 0xFFEFEFEFu) })
            Root.Resources[$"InfoBar{key}SeverityBackgroundBrush"] = new SolidColorBrush(Pick(dark, light));
        if (AppWindow?.TitleBar is { } bar)
        {
            var fg = _light ? Microsoft.UI.Colors.Black : Microsoft.UI.Colors.White;
            bar.ButtonForegroundColor = fg;
            bar.ButtonHoverForegroundColor = fg;
            bar.ButtonPressedForegroundColor = fg;
            bar.ButtonInactiveForegroundColor = Pick(0xFF8E8E8E, 0xFF8C8C8C);
            bar.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent;
            bar.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent;
            bar.ButtonHoverBackgroundColor = Pick(0x1AFFFFFF, 0x12000000);
            bar.ButtonPressedBackgroundColor = Pick(0x10FFFFFF, 0x0C000000);
        }
    }

    static void CardHeader(Panel card, string glyph, string brush, string title, string sub)
    {
        var head = new Grid { Padding = new Thickness(18, 14, 18, 14), ColumnSpacing = 12 };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.Children.Add(new FontIcon { Glyph = glyph, FontSize = 18, Foreground = Res(brush), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 2, 0, 0) });
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock { Text = title, FontSize = 15, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        foreach (var line in sub.Split('\n'))
            text.Children.Add(new TextBlock { Text = line, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(text, 1);
        head.Children.Add(text);
        card.Children.Add(head);
    }

    static void CardRow(Panel card, FrameworkElement left, string right, string brush)
    {
        var row = new Grid { Padding = new Thickness(18, 10, 18, 10), ColumnSpacing = 12, BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        left.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(left);
        var v = new TextBlock { Text = right, FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Foreground = Res(brush) };
        Grid.SetColumn(v, 1);
        row.Children.Add(v);
        card.Children.Add(row);
    }

    static void CardStrip(Panel card, string text, FrameworkElement? action)
    {
        var strip = new Grid { Padding = new Thickness(18, 10, 18, 10), ColumnSpacing = 12, Background = Res("ApexStripBrush"),
            BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        strip.ColumnDefinitions.Add(new ColumnDefinition());
        strip.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        strip.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center });
        if (action is not null) { Grid.SetColumn(action, 1); strip.Children.Add(action); }
        card.Children.Add(strip);
    }

    // ---------- refresh ----------

    void RefreshAll()
    {
        LangText.Text = Languages.First(l => l.Code == Lang).Name;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(LangButton, T("Language"));
        RefreshGame();
        RefreshWelcome();
        RefreshFirstRun();
        RefreshComponents();
        RefreshManage();
        RefreshAbout();
        RefreshLauncher();
        UpdateFooter();
    }

    void RefreshWelcome()
    {
        var latest = Latest(Components.Apex);
        WelcomeTitle.Text = "Apex Radiance" + (latest is null ? "" : " " + latest);
        var inst = _game is null ? null : _state?.ApexVersion;
        if (_offline)
        {
            if (_releaseError is { } limit) SetInfo(WelcomeInfo, InfoBarSeverity.Error, T("The launcher can't reach GitHub"), T(limit.Key, limit.Args), T("Try again"), async () => await LoadReleasesAsync());
            else SetInfo(WelcomeInfo, InfoBarSeverity.Error, T("No internet connection"), T("Check your internet connection, then try again."), T("Try again"), async () => await LoadReleasesAsync());
        }
        else if (_loading && _game is not null)
            SetInfo(WelcomeInfo, InfoBarSeverity.Informational, T("Checking for updates\u2026"), "");
        else if (_updated is not null || _inline || Updates().Count > 0 && _state?.ApexVersion is not null)
            WelcomeInfo.IsOpen = false; // (the update card says it)
        else if (Updates() is { Count: > 0 } ups && _game is { } g)
        {
            // checked at every start: the player learns about a new release here, without visiting GitHub
            var lines = ups.Select(c => T("{0} {1} → {2}", Name(c), Known(c) is { Length: > 0 } v ? v : "?", Latest(c)!));
            SetInfo(WelcomeInfo, InfoBarSeverity.Informational, T("Updates available"),
                string.Join(", ", lines) + ". " + (_running ? T("Close The Sims 3, its launcher, the restart host (ts3wrap.exe) and the RTX Remix bridge (NvRemixBridge.exe) to update.") : T("Update now installs them.")));
        }
        // (a game with Apex, checked and current: the status card's header says so)
        else WelcomeInfo.IsOpen = false;
        RefreshStatus();
        RefreshUpdateCard();
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
        var muted = App.Brush("ApexMutedBrush");
        var ok = App.Brush("ApexSuccessBrush");
        var accent = App.Brush("ApexAccentBrush");
        string State(string c) => Known(c) is not { Length: > 0 } v ? T("Installed") : Newer(Latest(c), v) ? T("{0} \u2192 {1}", v, Latest(c)!) : T("{0} \u00b7 up to date", v);
        var apexRelease = _latest.GetValueOrDefault(Repo[Components.Apex]);
        var test = Verify.TestBuild(g, s, apexRelease);
        var found = GpuFindings(s).Concat(Verify.Files(g, s, apexRelease)).Concat(_found.Where(x => x.Suggest is null)).ToList();
        var clean = found.Count == 0;
        // (the header already says the game is current: "in place" only after a check the player asked for)
        var check = clean ? (_checkedAt is null ? "" : T("Everything is in place (checked at {0}).", _checkedAt))
            : _checkedAt is not null ? T("Checked at {0}.", _checkedAt) : "";
        CardHeader(StatusList, clean ? "\uE73E" : "\uE7BA", clean ? "ApexSuccessBrush" : "ApexWarningBrush", T("The Sims 3 \u00b7 {0}", g.Label),
            string.Join("\n", new[] { g.Root, !_loading && !_offline && Updates().Count == 0 ? T("Everything in this game is on its latest version.") : "", check }.Where(x => x.Length > 0)));
        void Row(string name, string value, bool good) =>
            CardRow(StatusList, new TextBlock { Text = name, FontSize = 14 }, value, good ? "ApexMutedBrush" : "ApexAccentBrush");
        Row("Apex Radiance", test ? T("{0} \u00b7 test build", s.ApexVersion!) : State(Components.Apex), !test && !Updates().Contains(Components.Apex));
        Row(T("Graphics"), LayerName(s.Layer), true);
        foreach (var (c, name, present) in new[] { (Components.S3ss, "Sims3SettingsSetter", s.S3ss), (Components.Modernization, "Sims 3 Modernization", s.Modernization), (Components.MonoPatcher, "MonoPatcher", s.MonoPatcher),
                     (Components.RestartHost, T("Restart host"), s.RestartHost) })
            if (present) Row(name, c == Components.Modernization && Releases.IsExperimental(Known(c) ?? "") ? State(c) + " (" + T("Experimental") + ")" : State(c), !Updates().Contains(c));

        // the problems and suggestions, between the rows and the buttons
        var notes = new StackPanel { Spacing = 8, Padding = new Thickness(18, 12, 18, 12), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        foreach (var x in found)
        {
            var bar = new InfoBar
            {
                IsOpen = true, IsClosable = false, Message = T(x.Text, x.Args), Title = x.CanFix ? T("The launcher fixes this") : "",
                Severity = x.Severity switch { Severity.Error => InfoBarSeverity.Error, Severity.Warning => InfoBarSeverity.Warning, _ => InfoBarSeverity.Informational },
            };
            if (x.Link is { } link) bar.ActionButton = new HyperlinkButton { Content = T("Read more"), NavigateUri = new Uri(link) };
            notes.Children.Add(bar);
        }
        // (the suggestions show on Components, not on the status)
        if (notes.Children.Count > 0) StatusList.Children.Add(notes);

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
        CardStrip(StatusList, "", buttons); // (the settings line sits under the card)
    }

    // the welcome page's one-click update: the installed components with a newer release, when Setup can download them now
    List<string> WelcomeUpdates() => _offline || _running || _game is null ? new List<string>() : Updates().Where(c => !_skipUpdate.Contains(c)).ToList();

    // installed components with a newer release (only those: nothing new is added from the welcome page)
    List<string> Updates()
    {
        var l = new List<string>();
        if (_state is not { } s || _game is null) return l;
        if (s.ApexVersion is { } a && (a == "?" || Newer(Latest(Components.Apex), a) && !TestBuildNow())) l.Add(Components.Apex);
        if (s.Layer == Layer.Bridge && Newer(Latest(Components.Bridge), s.Installed(Components.Bridge) ?? "")) l.Add(Components.Bridge);
        if (s.Layer == Layer.Dxvk && s.Installed(Components.Dxvk) is { } d && Newer(Latest(Components.Dxvk), d)) l.Add(Components.Dxvk);
        foreach (var c in new[] { Components.S3ss, Components.Modernization, Components.MonoPatcher, Components.RestartHost })
            if (Known(c) is { Length: > 0 } v && Newer(Latest(c), v)) l.Add(c);
        return l;
    }

    static void SetInfo(InfoBar bar, InfoBarSeverity sev, string title, string msg, string? action = null, Action? onAction = null)
    {
        bar.Severity = sev; bar.Title = title; bar.Message = msg; bar.IsOpen = true; bar.Content = null;
        if (action is null) { bar.ActionButton = null; return; }
        var b = new Button { Content = action };
        b.Click += (_, _) => onAction?.Invoke();
        bar.ActionButton = b;
    }

    // the game cards: the chosen one outlined in the accent (as the renderer cards), a hovered one lighter
    void OnGameCard(ListViewBase sender, ContainerContentChangingEventArgs e)
    {
        if (e.ItemContainer is not ListViewItem c || c.Tag is "hooked") return;
        c.Tag = "hooked";
        c.PointerEntered += (_, _) => PaintGame(c, true);
        c.PointerExited += (_, _) => PaintGame(c, false);
        c.Loaded += (_, _) => PaintGame(c, false);
    }

    void PaintGame(ListViewItem c, bool hover)
    {
        if (c.ContentTemplateRoot is not Border card) return;
        var on = c.IsSelected;
        card.BorderBrush = Res(on ? "ApexAccentBrush" : "ApexCardBorderBrush");
        card.Background = on ? Res("ApexAccentSoftBrush")
            : hover ? Res("ApexHoverBrush") : Res("ApexCardBrush");
    }

    void PaintGames()
    {
        foreach (var it in _games)
            if (GameList.ContainerFromItem(it) is ListViewItem c) PaintGame(c, false);
    }

    void OnGamePicked(object sender, SelectionChangedEventArgs e)
    {
        PaintGames();
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
        _health = _state is null ? [] : Verify.Files(_game!, _state, _latest.GetValueOrDefault(Releases.Apex)).Concat(_found)
            .Select(f => new Health.Item(f, Health.AreaOf(f), [])).ToList();
        CheckHealth(); // (the full check, off the window's thread; this quick list shows until it is done)
        _healthState = Health.StateOf(_health);
        _checked = DateTime.Now;
        GameDetails.Visibility = _state is null ? Visibility.Collapsed : Visibility.Visible;
        FillProblems();
    }

    // the Game page's problems and suggestions, from the last scan (redrawn without a new scan when the card is known)
    void FillProblems()
    {
        if (_state is not { } s) return;
        ProblemList.Children.Clear();
        var found = GpuFindings(s).Concat(_found.Where(x => x.Suggest is null)).ToList();
        foreach (var x in found)
        {
            var bar = new InfoBar
            {
                IsOpen = true, IsClosable = false, Message = T(x.Text, x.Args),
                Severity = x.Severity switch { Severity.Error => InfoBarSeverity.Error, Severity.Warning => InfoBarSeverity.Warning, _ => InfoBarSeverity.Informational },
                Title = x.CanFix ? T("The launcher fixes this") : "",
            };
            if (x.Link is { } link) bar.ActionButton = new HyperlinkButton { Content = T("Read more"), NavigateUri = new Uri(link) };
            ProblemList.Children.Add(bar);
        }
        // (the fixes run with Install / Apply, or Fix settings when nothing else changes)
        if (found.Count == 0)
            ProblemList.Children.Add(new TextBlock { Text = T("No incompatible settings found."), FontSize = 12, Foreground = App.Brush("ApexSuccessBrush") });
        SuggestList.Children.Clear();
        FillSuggestions(SuggestList, GraphicsInfo, onStatus: false);
        SuggestList.Visibility = SuggestList.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed; // (an empty one still takes a gap)
    }

    // "Suggested for Apex Radiance": what Apex recommends in game, each applied only by its own click (or Apply all
    // suggestions), each in a restore point; a component's suggestion points to Components
    void FillSuggestions(Panel list, InfoBar resultBar, bool onStatus)
    {
        // a component to choose (DXVK, the bridge, Sims3SettingsSetter) is already a choice on Components: only what can be applied shows
        var sug = _found.Where(x => x.Suggest is not null && x.CanApply).ToList();
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
            // (a component to choose: the sidebar's Components; no button here, the status card keeps one)
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

    // the renderer choice: the RTX Remix bridge (Apex recommends it), DXVK, the game's own Direct3D 9, or another d3d9.dll kept as it is
    const string RDxvk = "dxvk", RBridge = "bridge", RNative = "native", RKeep = "keep";
    string _renderer = RBridge;

    static string RendererOf(State s) => s.Layer switch { Layer.Dxvk => RDxvk, Layer.Bridge => RBridge, Layer.Native => RNative, _ => RKeep };

    void SetDefaults()
    {
        if (_state is not { } s || _defaultsFor == _game) return;
        _defaultsFor = _game;
        // what is installed stays on; a first install takes what Apex recommends (the RTX Remix bridge, Sims3SettingsSetter); a
        // game that already has Apex keeps its renderer and its choices (the card check, once known, moves a blocked bridge to DXVK)
        _renderer = RendererOf(s with { Layer = Engine.DefaultLayer(s, VulkanBlocked) });
        S3ssToggle.IsOn = s.S3ss; // (optional mods start off unless installed)
        ModToggle.IsOn = s.Modernization;
        _modExp = s.Manifest.ModernizationChannel == "experimental";
        (_modExp ? ModExperimental : ModStable).IsChecked = true;
        MonoToggle.IsOn = s.MonoPatcher;
        RestartToggle.IsOn = s.RestartHost; // (experimental: off unless on already)
        // no longer offered (no mod Setup installs needs it, and it crashed the launcher on some PCs): shown only to keep or remove it
        MonoCard.Visibility = s.MonoPatcher ? Visibility.Visible : Visibility.Collapsed;
        S3ppToggle.IsOn = true;
    }

    void OnToggle(object sender, RoutedEventArgs e) => RefreshComponents();

    // NVIDIA Smooth Motion, AMD Fluid Motion Frames and Apex's frame limit: their switches and picker only plan a change (pending,
    // like the components); Apply writes them as task rows (SettingChanges). Switching back to the state now drops the change.
    SmoothMotion.Status? _smooth;
    string? _smoothFor, _smoothError;
    bool _smoothFps; // the last apply set Apex's frame limit to 60 when it turned Smooth Motion on
    bool _fillingSmooth;
    string? _smoothPending; // why a removal could not turn Smooth Motion off, shown once the row reads the driver again
    bool? _smoothWant;
    bool _smoothErrorOn = true, _afmfErrorOn = true; // (a failed change's direction: the manual steps say turn it on, or off)

    // after a removal or an apply: the rows read the drivers again; a failure is logged and shown on its own row (removals only
    // ever turn frame generation off)
    void AfterFrameGenOff()
    {
        _smoothFor = null;
        _afmf = null;
        var (nv, amd) = Engine.TakeFrameGenOffError();
        if (nv is not null) { _smoothPending = nv; _smoothErrorOn = false; App.Log(new InvalidOperationException("Smooth Motion off: " + nv)); }
        if (amd is not null) { _afmfPending = amd; _afmfErrorOn = false; App.Log(new InvalidOperationException("AFMF off: " + amd)); }
    }

    // the pending state of a switch, shown on its row
    (string, string)? PendingLine(bool? want) => want is { } w
        ? (T(w ? "Turns on when you apply the changes." : "Turns off when you apply the changes."), "ApexAccentBrush") : null;

    // "Apex's frame limit is now 60 fps" only while it still is
    bool LimitNow60 => _game is { } lg && ReadFpsLimit(lg) == Engine.FrameGenFps; // (read now: _fpsNow is refreshed after the rows)

    // Apex's frame limit in the game's ApexRadiance.toml: 0 for none, null when the game has not made the file
    static int? ReadFpsLimit(Game g)
    {
        var toml = Engine.ApexToml(Scan.DocumentsFor(g));
        try { return File.Exists(toml) ? Engine.FpsLimitKey(File.ReadAllText(toml)) ?? 0 : null; }
        catch (IOException) { return null; }
    }

    void RefreshSmooth(State s, Game g)
    {
        if (_smoothFor != g.Exe) { _smoothFor = g.Exe; _smooth = SmoothMotion.Read(); _smoothError = _smoothPending; _smoothPending = null; _smoothWant = null; }
        var sm = _smooth!;
        // no NVIDIA card (or no driver to ask): no row; an NVIDIA card that cannot run it: the row, off, with the reason
        SmoothCard.Visibility = sm.Card is null && sm.Error is null ? Visibility.Collapsed : Visibility.Visible;
        // (the bridge installed and staying: a pending change of renderer away from it blocks a turn-on)
        var blocked = SmoothMotion.Blocked(sm, s.Layer == Layer.Bridge && _renderer == RBridge, SmoothMotion.SetupTurnedOn);
        if (blocked is not null) _smoothWant = null;
        _fillingSmooth = true;
        SmoothToggle.IsOn = _smoothWant ?? sm.On;
        SmoothToggle.IsEnabled = blocked is null;
        // the manual steps when the driver did not accept the change (not for the dry run or a refusal)
        SmoothHelp.NavigateUri = new Uri(SmoothMotion.ManualUrl);
        SmoothHelp.Visibility = _smoothWant is null && _smoothError is { } se && Engine.FriendlyFrameGenError(se, false).StartsWith("The NVIDIA driver")
            ? Visibility.Visible : Visibility.Collapsed;
        _fillingSmooth = false;
        SetState(SmoothPill, PendingLine(_smoothWant)
            ?? (_smoothError == "Dry run: Setup changes nothing." ? (T(_smoothError), "ApexMutedBrush")
            : _smoothError is not null ? (T(Engine.FriendlyFrameGenError(_smoothError, false, _smoothErrorOn)), "ApexWarningBrush")
            : blocked is { } b ? (T(b.Key, b.Arg), "ApexMutedBrush")
            : sm.On ? (T(_smoothFps && LimitNow60 ? "On for The Sims 3 in the NVIDIA driver. Apex's frame limit is now 60 fps, the pace it works best at."
                                  : "On for The Sims 3 in the NVIDIA driver. Works best with Apex's frame limit at 60 to 90 fps."), "ApexSuccessBrush")
            : (T("Off for The Sims 3."), "ApexMutedBrush")));
    }

    // AMD Fluid Motion Frames: one setting for the whole system in the AMD driver (Afmf.cs); read once per Setup run and after changes
    Afmf.Status? _afmf;
    string? _afmfError, _afmfPending;
    bool _afmfFps, _fillingAfmf;
    bool? _afmfWant;

    void RefreshAfmf()
    {
        if (_afmf is null) { _afmf = Afmf.Read(); _afmfError = _afmfPending; _afmfPending = null; _afmfWant = null; }
        var a = _afmf;
        // no AMD driver: no row; an AMD card ADLX says cannot run it: the row, off, with the reason (no renderer needed: AFMF 2
        // covers Vulkan; under the bridge it is unverified)
        AfmfCard.Visibility = a.Present ? Visibility.Visible : Visibility.Collapsed;
        var blocked = Afmf.Blocked(a);
        if (blocked is not null) _afmfWant = null;
        _fillingAfmf = true;
        AfmfToggle.IsOn = _afmfWant ?? a.On;
        AfmfToggle.IsEnabled = blocked is null;
        _fillingAfmf = false;
        SetState(AfmfPill, PendingLine(_afmfWant)
            ?? (_afmfError == "Dry run: Setup changes nothing." ? (T(_afmfError), "ApexMutedBrush")
            : _afmfError is not null ? (T(Engine.FriendlyFrameGenError(_afmfError, true, _afmfErrorOn)), "ApexWarningBrush")
            : blocked is { } b ? (T(b.Key, b.Arg), "ApexMutedBrush")
            : a.On ? (T(_afmfFps && LimitNow60 ? "On in the AMD driver, for every game. Apex's frame limit is now 60 fps, the pace it works best at."
                                 : "On in the AMD driver, for every game. Works best with Apex's frame limit at 60 to 90 fps."), "ApexSuccessBrush")
            : (T("Off in the AMD driver."), "ApexMutedBrush")));
    }

    void OnAfmfToggle(object sender, RoutedEventArgs e)
    {
        if (_fillingAfmf || _afmf is null) return;
        _afmfWant = Engine.PendingSwitch(AfmfToggle.IsOn, _afmf.On);
        _afmfError = null;
        Later(RefreshComponents);
    }

    void OnSmoothToggle(object sender, RoutedEventArgs e)
    {
        if (_fillingSmooth || _smooth is null) return;
        _smoothWant = Engine.PendingSwitch(SmoothToggle.IsOn, _smooth.On);
        _smoothError = null;
        Later(RefreshComponents);
    }

    // Apex's frame limit beside frame generation: shown while either row can be used and the game has made ApexRadiance.toml.
    // Items: Keep (the value now), Engine.FpsChoices, Custom (a number box, 20 to 1000).
    bool _fillingFps, _fpsCustomOpen;
    string? _fpsItemsKey;
    int? _fpsNow, _fpsWant;
    string? _fpsError;
    const int FpsCustomTag = -2;

    string FpsLabel(int v) => v == 0 ? T("Off") : T(Engine.FpsRecommended(v) ? "{0} fps (recommended)" : "{0} fps", v);

    // the frame generation that is on or turns on with the pending change, by name; null when none is
    string? ActiveFrameGen() =>
        (_smoothWant ?? _smooth?.On) == true ? "NVIDIA Smooth Motion" : (_afmfWant ?? _afmf?.On) == true ? "AMD Fluid Motion Frames" : null;

    void RefreshFps(Game g)
    {
        var usable = SmoothCard.Visibility == Visibility.Visible && (SmoothToggle.IsEnabled || SmoothToggle.IsOn)
                  || AfmfCard.Visibility == Visibility.Visible && (AfmfToggle.IsEnabled || AfmfToggle.IsOn);
        var now = ReadFpsLimit(g);
        FpsCard.Visibility = usable && now is not null ? Visibility.Visible : Visibility.Collapsed;
        // a divider above every row shown but the first; no section when no row shows
        var rows = SmoothList.Children.OfType<Border>().Where(x => x.Visibility == Visibility.Visible).ToList();
        for (int i = 0; i < rows.Count; i++) { rows[i].BorderBrush = App.Brush("ApexCardBorderBrush"); rows[i].BorderThickness = new Thickness(0, i == 0 ? 0 : 1, 0, 0); }
        SmoothSection.Visibility = rows.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (now is null) { _fpsWant = null; _fpsCustomOpen = false; return; }
        if (_fpsNow != now) _fpsWant = null; // (the file changed under the picker: start again from it)
        _fpsNow = now;
        _fillingFps = true;
        // the list is built again only when its labels change (the limit now, the language): clearing a ComboBox's items
        // while its own SelectionChanged runs makes WinUI fail fast (0x80070490 / 0x8000FFFF in Microsoft.UI.Xaml.dll)
        var itemsKey = now + "|" + Lang;
        if (itemsKey != _fpsItemsKey)
        {
            _fpsItemsKey = itemsKey;
            FpsCombo.Items.Clear();
            FpsCombo.Items.Add(new ComboBoxItem { Content = T("Keep: {0}", now == 0 ? T("no limit") : T("{0} fps", now.Value)), Tag = -1 });
            foreach (var v in Engine.FpsChoices) FpsCombo.Items.Add(new ComboBoxItem { Content = FpsLabel(v), Tag = v });
            FpsCombo.Items.Add(new ComboBoxItem { Content = T("Custom"), Tag = FpsCustomTag });
        }
        // the pending choice stays selected; a custom one (or Custom chosen, no number yet) shows its number box
        var at = _fpsWant is { } w ? Array.IndexOf(Engine.FpsChoices, w) : -1;
        var custom = _fpsCustomOpen || _fpsWant is not null && at < 0;
        FpsCombo.SelectedIndex = custom ? FpsCombo.Items.Count - 1 : _fpsWant is null ? 0 : at + 1;
        FpsCustom.Visibility = custom ? Visibility.Visible : Visibility.Collapsed;
        FpsCustom.Value = _fpsWant ?? (now > 0 ? now.Value : Engine.FrameGenFps);
        _fillingFps = false;
        var shown = _fpsWant ?? now.Value; // what the player sees, with the pending choice
        // the frame generation the row stands beside, by name: the one that is on (or turns on), else the row shown
        var active = ActiveFrameGen();
        var fg = active ?? (SmoothCard.Visibility == Visibility.Visible && SmoothToggle.IsEnabled ? "NVIDIA Smooth Motion" : "AMD Fluid Motion Frames");
        FpsDesc.Text = T("{0} doubles what you see: a 60 fps limit shows about 120 on screen. Works best at 60 to 90 fps.", fg);
        // the doubled figure only while frame generation is on; otherwise the limit as it is
        string Seen(int v) => active is null ? (v > 0 ? T("{0} fps", v) : T("no limit"))
            : v > 0 ? T("{0} fps → about {1} fps on screen with {2}", v, v * 2, active) : T("No limit: about double your frame rate on screen with {0}", active);
        SetState(FpsPill, _fpsWant is { } p ? (T("Changes to {0} when you apply the changes.", p == 0 ? T("no limit") : T("{0} fps", p))
                + (active is null ? "" : " " + Seen(p)), "ApexAccentBrush")
            : _fpsError == "Dry run: Setup changes nothing." ? (T(_fpsError), "ApexMutedBrush")
            : _fpsError is not null ? (T("Could not change Apex's frame limit: {0}", _fpsError), "ApexWarningBrush")
            : (Seen(shown), active is null ? "ApexMutedBrush" : "ApexAccentBrush"));
    }

    void OnFpsPicked(object sender, SelectionChangedEventArgs e)
    {
        if (_fillingFps || FpsCombo.SelectedItem is not ComboBoxItem { Tag: int v } || _fpsNow is not { } now) return;
        _fpsCustomOpen = v == FpsCustomTag;
        if (_fpsCustomOpen)
        {
            // the number shown is the choice: pending at once (60, or the limit now), so it can be applied as it is
            FpsCustom.Visibility = Visibility.Visible;
            _fpsWant = Engine.PendingFps(double.IsNaN(FpsCustom.Value) ? Engine.FrameGenFps : (int)Math.Round(FpsCustom.Value), now);
            _fpsError = null;
            Later(RefreshComponents);
            return;
        }
        _fpsWant = v < 0 ? null : Engine.PendingFps(v, now);
        _fpsError = null;
        Later(RefreshComponents); // (after the list's own selection has finished)
    }

    void OnFpsCustom(NumberBox sender, NumberBoxValueChangedEventArgs e)
    {
        if (_fillingFps || double.IsNaN(e.NewValue) || FpsCustom.Visibility != Visibility.Visible || _fpsNow is not { } now) return;
        _fpsWant = Engine.PendingFps((int)Math.Round(e.NewValue), now);
        _fpsError = null;
        Later(RefreshComponents);
    }

    // the pending driver and frame limit changes, in the order Apply runs them (Engine.PlanFrameGen); each returns why it failed in
    // the player's words for the frame limit, or the driver's raw text (worded by FriendlyFrameGenError on its row), or null. A
    // failed automatic frame limit is logged, never the driver change's error.
    public sealed record SettingChange(string Label, string Kind, bool? On, Func<string?> Run);
    List<SettingChange> SettingChanges(IEnumerable<string>? removals = null)
    {
        var l = new List<SettingChange>();
        if (_game is not { } g) return l;
        string? Safe(Func<string?> f) { try { return f(); } catch (Exception ex) { App.Log(ex); return ErrorText(ex); } }
        void AutoLimit(string feature, ref bool flag)
        {
            try { flag = Engine.SetFpsLimitIfNone(g, feature); }
            catch (Exception ex) { App.Log(ex); }
        }
        foreach (var c in Engine.PlanFrameGen(_fpsWant, _smoothWant, _afmfWant, removals ?? [], _smooth?.Card is not null))
            switch (c.Kind)
            {
                case "fps":
                    var fps = c.Fps!.Value;
                    l.Add(new(T("Apex frame limit: {0}", fps == 0 ? T("no limit") : T("{0} fps", fps)), c.Kind, null, () => Safe(() => { Engine.SetFpsLimit(g, fps); return null; })));
                    break;
                case "nv":
                    var sw = c.On!.Value;
                    l.Add(new(T(sw ? "Turn on {0}" : "Turn off {0}", "NVIDIA Smooth Motion"), c.Kind, sw, () => Safe(() =>
                    {
                        // read back in a new session without dropping Setup's record
                        var err = Engine.VerifyChange(sw, () => SmoothMotion.Set(sw), () => SmoothMotion.Read(tidy: false) is var r ? (r.On, r.Error) : default,
                            () => SmoothMotion.Undo(true), SmoothMotion.KeepRecord);
                        if (err is null && c.AutoLimit) AutoLimit("NVIDIA Smooth Motion", ref _smoothFps);
                        return err;
                    })));
                    break;
                case "amd":
                    var aw = c.On!.Value;
                    l.Add(new(T(aw ? "Turn on {0}" : "Turn off {0}", "AMD Fluid Motion Frames"), c.Kind, aw, () => Safe(() =>
                    {
                        var err = Engine.VerifyChange(aw, () => Afmf.Set(aw), () => Afmf.Read() is var r ? (r.On, r.Error) : default, () => Afmf.Set(false));
                        if (err is null && c.AutoLimit) AutoLimit("AMD Fluid Motion Frames", ref _afmfFps);
                        return err;
                    })));
                    break;
            }
        return l;
    }

    // after Apply ran them: each error to its row, the pending state cleared, the drivers and the file read again
    void AfterSettingChanges(List<(SettingChange Change, string? Error)> done)
    {
        foreach (var (c, err) in done)
            switch (c.Kind)
            {
                case "nv": _smoothPending = err; _smoothErrorOn = c.On ?? true; break;
                case "amd": _afmfPending = err; _afmfErrorOn = c.On ?? true; break;
                case "fps": _fpsError = err; _smoothFps = _afmfFps = false; break; // (the player's own limit now: no "is now 60 fps")
            }
        foreach (var (c, err) in done) if (err is not null && err != "Dry run: Setup changes nothing.") App.Log(new InvalidOperationException(c.Label + ": " + err));
        _lastChanges = done.Select(d => (d.Change.Label, d.Change.Kind, d.Change.On, d.Error)).ToList();
        _smoothWant = _afmfWant = null;
        _fpsWant = null;
        _fpsCustomOpen = false;
        _smoothFor = null;
        _afmf = null;
    }

    // the last Apply's driver and frame limit changes and their errors, for the Done screen
    List<(string Label, string Kind, bool? On, string? Error)> _lastChanges = new();

    // a change's error in the player's words, for the Done screen (the frame limit's is worded already)
    string ChangeError(string kind, bool? on, string error) =>
        kind == "nv" ? T(Engine.FriendlyFrameGenError(error, false, on ?? true))
        : kind == "amd" ? T(Engine.FriendlyFrameGenError(error, true, on ?? true))
        : T("Could not change Apex's frame limit: {0}", error);

    void OnModChannel(object sender, RoutedEventArgs e)
    {
        _modExp = ModExperimental.IsChecked == true;
        RefreshComponents();
    }

    bool _fillingRenderer;
    // the renderer choices as cards: a radio button over the whole card, the chosen card outlined in the accent
    readonly Dictionary<string, (Border Card, RadioButton Radio)> _rendererCards = new();

    void FillRenderer(State s)
    {
        // DXVK and the bridge need what DXVK asks of the card and its driver (Gpu); one already installed stays listed, so it can
        // be removed
        var tags = new List<string> { RDxvk, RBridge, RNative };
        if (VulkanBlocked && s.Layer != Layer.Bridge) tags.Remove(RBridge);
        if (VulkanBlocked && s.Layer != Layer.Dxvk) tags.Remove(RDxvk);
        if (s.Layer == Layer.Other) tags.Add(RKeep);
        // each choice: its name, what it does, and Installed on the one the game has now (rebuilt when the list, the language
        // or the installed one changes)
        var cur = RendererOf(s);
        var key = string.Join("|", tags) + "|" + cur + "|" + Lang;
        _fillingRenderer = true;
        if (key != _rendererKey)
        {
            _rendererKey = key;
            RendererCards.Children.Clear();
            _rendererCards.Clear();
            foreach (var r in tags)
            {
                var radio = new RadioButton { GroupName = "renderer", Content = RendererItem(r, r == cur), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
                radio.Checked += (_, _) => { if (_fillingRenderer) return; _renderer = r; RefreshComponents(); };
                var card = new Border { Child = radio, CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), Padding = new Thickness(16, 12, 16, 10) };
                RendererCards.Children.Add(card);
                _rendererCards[r] = (card, radio);
            }
        }
        foreach (var (r, (card, radio)) in _rendererCards)
        {
            var on = r == _renderer;
            radio.IsChecked = on;
            card.BorderBrush = Res(on ? "ApexAccentBrush" : "ApexCardBorderBrush");
            card.Background = on ? Res("ApexAccentSoftBrush") : Res("ApexCardBrush");
        }
        _fillingRenderer = false;
    }

    string? _rendererKey;

    StackPanel RendererItem(string r, bool installed)
    {
        var (title, desc) = r switch
        {
            RDxvk => (T("Vulkan, DXVK"), T("Fewer stutters; choose it if the game misbehaves with the bridge.")),
            RBridge => (T("Vulkan with more memory (RTX Remix bridge, recommended)"), T("Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK.")),
            RNative => (T("Windows default (Direct3D 9)"), T("Works, but the RTX Remix bridge or DXVK run Apex Radiance better.")),
            _ => (T("Keep my d3d9.dll"), T("Your d3d9.dll stays as it is.")),
        };
        var item = new StackPanel { Spacing = 4, Margin = new Thickness(4, 0, 0, 0) };
        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        head.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        if (installed) head.Children.Add(new TextBlock { Text = T("Installed"), FontSize = 12, Foreground = Res("ApexSuccessBrush"), VerticalAlignment = VerticalAlignment.Center });
        item.Children.Add(head);
        item.Children.Add(new TextBlock { Text = desc, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, MaxWidth = 640 });
        return item;
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

        if (VulkanBlocked && _renderer == RBridge && s.Layer != Layer.Bridge) _renderer = RDxvk;
        if (VulkanBlocked && _renderer == RDxvk && s.Layer != Layer.Dxvk) _renderer = RNative;
        FillRenderer(s);
        var cur = RendererOf(s);
        var changed = _renderer != cur;
        RendererName.Text = T("Graphics") + ": " + RendererShort(_renderer);
        SetState(RendererState, changed ? (T("Replaces {0}", LayerName(s.Layer)), "ApexAccentBrush")
            : _renderer == RDxvk ? StateLine(Components.Dxvk, true, true, false)
            : _renderer == RBridge ? StateLine(Components.Bridge, true, true, false)
            : _renderer == RNative ? (T("Works, but the RTX Remix bridge or DXVK run Apex Radiance better."), "ApexMutedBrush")
            : (T("Your d3d9.dll stays as it is."), "ApexMutedBrush"));
        GpuLine.Text = _vulkan is { } gpu && gpu.Card is not null ? T("Your graphics card: {0}", $"{gpu.Card} \u00b7 Vulkan {gpu.Version}") : "";
        BridgeDetail.Text = "";
        if (OldCard)
            BridgeDetail.Text = T("DXVK and the RTX Remix bridge need a graphics card with Vulkan 1.3 (found: {0}).", CardFound);
        else if (OldDriver)
            BridgeDetail.Text = T("Your graphics driver is too old for DXVK and the RTX Remix bridge (missing: {0}). Update the graphics driver, then open the launcher again.", MissingList);

        // the game resets its graphics options when the card it sees changes: DXVK's TS3W profile and the bridge report a
        // GeForce GTX 580; the game's own D3D9 the real card (DXVK on TS3.exe: unverified, so treated as a change)
        string Card(string r) => r switch { RBridge => "580", RDxvk when g.Exe == "TS3W.exe" => "580", RNative => "real", _ => "?" };
        if (changed && (Card(_renderer) != Card(cur) || Card(cur) == "?"))
            SetInfo(GraphicsInfo, InfoBarSeverity.Warning, "",
                T("Your in-game graphics settings may reset once. The launcher keeps a copy and puts it back if you switch back."));
        else GraphicsInfo.IsOpen = false;
        RendererResetNote.Visibility = GraphicsInfo.IsOpen ? Visibility.Visible : Visibility.Collapsed;

        // An installed component shows a Remove button (Keep once marked: Apply removes it, and its pill says so); a missing one
        // its switch, which means Install. A switch that meant "keep" on an installed component, off meaning uninstall, read
        // as a control that did nothing.
        SetState(S3ssPill, StateLine(Components.S3ss, S3ssToggle.IsOn, s.S3ss, false));
        SetState(ModPill, StateLine(Components.Modernization, ModToggle.IsOn, s.Modernization, s.Ts3NativeWrongBuild));
        SetState(MonoPill, StateLine(Components.MonoPatcher, MonoToggle.IsOn, s.MonoPatcher, false));
        // Setup does not write Apex's settings before Apex has copied the older ones (Engine.RestartHostWaits)
        var waits = !s.RestartHost && Engine.RestartHostWaits(Scan.DocumentsFor(g));
        RestartToggle.IsEnabled = !waits;
        if (waits) RestartToggle.IsOn = false;
        SetState(RestartPill, waits ? (T(Engine.RestartHostWaitsText), "ApexMutedBrush") : StateLine(Components.RestartHost, RestartToggle.IsOn, s.RestartHost, false));
        SetState(S3ppPill, S3ppPresent ? (S3ppToggle.IsOn ? (T("Installed"), "ApexMutedBrush") : (T("Will be removed"), "ApexWarningBrush"))
            : _s3ppArchive is not null ? (T("Ready to install"), "ApexAccentBrush") : (T("Not installed"), "ApexMutedBrush"));
        S3ppToggle.Visibility = Visibility.Visible;
        if (!S3ppPresent) S3ppToggle.Visibility = Visibility.Collapsed; // (missing: its download buttons below)
        S3ppGet.Visibility = !S3ppPresent && _s3ppArchive is null ? Visibility.Visible : Visibility.Collapsed;
        S3ppWait.Visibility = _s3ppWatch is not null ? Visibility.Visible : Visibility.Collapsed;
        S3ppWaitText.Text = T("Waiting for your download in {0}…", DownloadsFolder());
        RefreshSmooth(s, g);
        RefreshAfmf();
        // only the frame generation row that applies (Engine.FrameGenRows)
        var (nvRow, amdRow) = Engine.FrameGenRows(_smooth!, _afmf!);
        SmoothCard.Visibility = nvRow ? Visibility.Visible : Visibility.Collapsed;
        AfmfCard.Visibility = amdRow ? Visibility.Visible : Visibility.Collapsed;
        RefreshFps(g);
        ModToggle.IsEnabled = g.ModernizationSupported || s.Modernization;
        if (!g.ModernizationSupported && !s.Modernization) ModToggle.IsOn = false;
        ModDesc.Text = g.ModernizationSupported
            ? T("Fixes stutters, Sims getting stuck and crashes.")
            : T("Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47.");
        ModChannel.Visibility = ModToggle.IsOn && g.ModernizationSupported ? Visibility.Visible : Visibility.Collapsed;

        UpdateFooter();
    }

    bool TestBuildNow() => _game is { } g && _state is { } s && Verify.TestBuild(g, s, _latest.GetValueOrDefault(Repo[Components.Apex]));

    // what Apply does to a component, under its name: kept, installed, updated or removed
    (string, string) StateLine(string c, bool on, bool present, bool wrong)
    {
        var v = Known(c);
        var latest = Latest(c);
        if (!present) return on ? (T("Will be installed"), "ApexAccentBrush") : (T("Not installed"), "ApexMutedBrush");
        if (!on) return (v is { Length: > 0 } ? T("{0} \u00b7 will be removed", v) : T("Will be removed"), "ApexWarningBrush");
        if (wrong) return (T("Wrong build"), "ApexWarningBrush");
        if (v is not { Length: > 0 }) return (T("Installed"), "ApexMutedBrush");
        return Newer(latest, v) || c == Components.Modernization && ModChannelChange() ? (T("{0} \u2192 updates to {1}", v, latest!), "ApexAccentBrush") : (T("{0} \u00b7 up to date", v), "ApexMutedBrush");
    }

    static void SetState(TextBlock line, (string Text, string Brush) state)
    {
        line.Text = state.Text;
        line.Foreground = App.Brush(state.Brush);
    }

    string RendererShort(string r) => r switch
    {
        RDxvk => "DXVK", RBridge => "RTX Remix bridge", RNative => T("Windows default (Direct3D 9)"), _ => T("Keep my d3d9.dll"),
    };

    void OnChangeRenderer(object sender, RoutedEventArgs e) => Show(RendererStep);

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
        Optional(Components.RestartHost, RestartToggle.IsOn, s.RestartHost, NeedsUpdate(Components.RestartHost));
        // Sims 3 Performance Patch: installed from the player's own download only
        if (S3ppPresent && !S3ppToggle.IsOn) del.Add(Components.S3pp);
        if (!S3ppPresent && _s3ppArchive is not null && _s3ppFor == g.Bin) add.Add(Components.S3pp);
        Optional(Components.Modernization, ModToggle.IsOn && (g.ModernizationSupported || s.Modernization), s.Modernization,
                 g.ModernizationSupported && (s.Ts3NativeWrongBuild || NeedsUpdate(Components.Modernization) || ModChannelChange()));
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

    static string Name(string c) => T(Components.Name(c));

    // ---------- install ----------

    // inline: an update from the welcome page, shown in its card (no Install and Done screens). settingsOnly: only the pending
    // driver and frame limit changes (SettingChanges), without the components and the settings fixes.
    async Task InstallAsync(List<string> comps, List<string>? remove = null, bool inline = false, bool settingsOnly = false)
    {
        if (_game is not { } g || _busy || _running) return;
        if (Preview is not null) { await Message(T("Preview"), T("This is a preview: nothing was installed.")); return; }
        if (!await CanChangeHere(inline ? "welcome" : "components", drivers: !inline && SettingChanges(remove).Any(c => c.Kind is "nv" or "amd"))) return;
        var from = _step == InstallStep ? ComponentsStep : _step;
        // busy before the checks: a second click (or the Performance Patch watcher) during them starts nothing
        _busy = true;
        if (!settingsOnly && _state is { } ps && !await PreflightOk(g, ps, comps)) { _busy = false; UpdateFooter(); return; }
        remove ??= new List<string>();
        var dels = Components.All.Where(remove.Contains).ToList();
        var order = Components.All.Reverse().Where(comps.Contains).ToList(); // the engine's order
        var before = order.ToDictionary(c => c, c => Known(c) ?? "");
        var installBar = InstallBar;
        Panel host = TaskList;
        if (inline)
        {
            _inline = true;
            _updated = null;
            UpdateList.Children.Clear();
            var head = new StackPanel { Spacing = 10, Padding = new Thickness(18, 14, 18, 14) };
            head.Children.Add(new TextBlock { Text = T("Applying changes"), FontSize = 15, FontWeight = FontWeights.SemiBold });
            installBar = new ProgressBar { Maximum = 1 };
            head.Children.Add(installBar);
            UpdateList.Children.Add(head);
            var list = new StackPanel { BorderBrush = App.Brush("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0), Padding = new Thickness(0, 4, 0, 4) };
            UpdateList.Children.Add(list);
            host = list;
            UpdateFooter();
        }
        else
        {
            InstallTitle.Text = T("Applying changes");
            Show(InstallStep);
            TaskList.Children.Clear();
        }
        installBar.Value = 0;
        // removing the bridge or Apex turns Setup's Smooth Motion profile off (Engine.Remove): its own row
        var smoothOff = _smooth is { On: true } && SmoothMotion.SetupTurnedOn && dels.Any(Engine.TurnsFrameGenOff);
        var rows = dels.Select(c => AddTask(host, T("Remove {0}", Name(c)))).ToList();
        Action<int>? smoothOffRow = null, afmfOffRow = null;
        if (smoothOff) rows.Add(smoothOffRow = AddTask(host, T("Turn off {0}", "NVIDIA Smooth Motion")));
        var afmfOff = _afmf is { On: true } && Afmf.SetupTurnedOn && dels.Any(Engine.TurnsFrameGenOff);
        if (afmfOff) rows.Add(afmfOffRow = AddTask(host, T("Turn off {0}", "AMD Fluid Motion Frames")));
        _lastChanges = new();
        var first = rows.Count;
        rows.AddRange(order.Select(c => AddTask(host, Name(c))));
        if (!settingsOnly) rows.Add(AddTask(host, T("Fix incompatible settings")));
        // the pending driver and frame limit changes: their own rows, after the components; each failure stays on its row
        var changes = inline ? new List<SettingChange>() : SettingChanges(dels);
        var changeRows = changes.Select(c => AddTask(host, c.Label)).ToList();
        foreach (var r in changeRows) r(0);
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
            installBar.Value = p.Fraction;
            // (downloads, before 0.5, install nothing: the first row stays active until the copies start)
            Mark(p.Arg is null || p.Fraction < 0.5 ?(p.Fraction >= 1 ? rows.Count : p.Fraction > 0.9 ? rows.Count - 1 : first) : first + order.IndexOf(p.Arg));
        });
        // Sims 3 Modernization from the chosen channel's release shown (read here: the install runs off the UI thread)
        var shown = new Dictionary<string, Release?>(_latest) { [Releases.Modernization] = ModRelease() ?? _latest.GetValueOrDefault(Releases.Modernization) };
        var modExp = _modExp;
        // one restore point for the whole change (the engine's own calls join it)
        RestorePoints.Scope? point = null;
        var engineDone = false; // (an error after the engine's part is no reason to undo a finished install)
        try
        {
            if (!settingsOnly) await Task.Run(async () =>
            {
                using (point = dels.Count == 0 && order.Count == 0 ? RestorePoints.Begin(g.Bin, "Fix incompatible settings")
                    : dels.Count == 0 ? RestorePoints.Begin(g.Bin, "Install {0}", string.Join(", ", order.Select(Components.Name)))
                    : order.Count == 0 ? RestorePoints.Begin(g.Bin, "Remove {0}", string.Join(", ", dels.Select(Components.Name)))
                    : RestorePoints.Begin(g.Bin, "Change components"))
                {
                    foreach (var c in dels) { Engine.Remove(g, c); touched = true; }
                    try { await Engine.InstallAsync(g, order, new GitHubSource { S3ppArchive = _s3ppArchive, Shown = shown, ModernizationExperimental = modExp, Log = App.Log }, progress); }
                    finally { try { Directory.Delete(GitHubSource.TempRoot, true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
                }
            });
            engineDone = true;
            var done = new List<(SettingChange, string?)>();
            for (int i = 0; i < changes.Count; i++)
            {
                changeRows[i](1);
                var err = await Task.Run(changes[i].Run);
                done.Add((changes[i], err));
                changeRows[i](err is null ? 2 : 3);
                installBar.Value = Math.Max(installBar.Value, (i + 1.0) / changes.Count);
            }
            if (changes.Count > 0) AfterSettingChanges(done);
            // a turn-off the removal could not make: its row and the Done screen say so
            var (nvOff, amdOff) = Engine.PeekFrameGenOffError();
            if (nvOff is not null) { smoothOffRow?.Invoke(3); _lastChanges.Add((T("Turn off {0}", "NVIDIA Smooth Motion"), "nv", false, nvOff)); }
            if (amdOff is not null) { afmfOffRow?.Invoke(3); _lastChanges.Add((T("Turn off {0}", "AMD Fluid Motion Frames"), "amd", false, amdOff)); }
            _lastInstall = order;
            _lastRemove = dels;
            if (order.Contains(Components.S3pp)) _s3ppArchive = null;
            _defaultsFor = null; // (the switches follow the game as it is now)
            AfterFrameGenOff();
            _busy = false;
            if (inline)
            {
                _inline = false;
                _state = Scan.Run(g); // (the versions now installed)
                _updated = (order.Where(c => c != Components.Loader).Select(c => (Name(c), before[c], Known(c) ?? Latest(c) ?? "")).ToList(), point?.Committed);
                _skipUpdate.Clear();
                RefreshAll();
            }
            else Show(DoneStep);
        }
        catch (Exception ex)
        {
            // (busy until the dialogs close: a background install cannot start and close the undo dialog)
            _inline = false;
            _defaultsFor = null;
            App.Log(ex);
            AfterFrameGenOff(); // (the drivers read again, a removal's turn-off error taken once, not left for the next Apply)
            var net = Engine.IsNetwork(ex);
            var title = net ? T("The launcher can't reach GitHub") : T("The launcher could not finish");
            var text = ErrorText(ex);
            // the files it changed before the error are put back: half an install is the worse state. Keeping them is the
            // second choice (Repair or remove sets them right later)
            if (!engineDone && point?.Committed is { } p)
            {
                var keep = await ShowDialog(new ContentDialog { Title = title, Content = ErrorContent(text + "\n\n" + T("The launcher puts back the files it changed.")),
                    PrimaryButtonText = T("Undo these changes"), SecondaryButtonText = T("Keep what was installed"),
                    DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot }) == ContentDialogResult.Secondary;
                if (!keep)
                {
                    Show(Manage);
                    // (Restore needs the game, its launcher and the restart host closed: said, not skipped in silence)
                    if (_running || Games.IsRunning())
                    {
                        await ShowDialog(new ContentDialog { Title = title, Content = ErrorContent(T("The launcher could not put the files back while The Sims 3, the game's own launcher, the restart host or the RTX Remix bridge runs. Close them, then click Restore on the newest point in Repair or remove.")),
                            CloseButtonText = T("OK"), XamlRoot = Content.XamlRoot });
                        _busy = false;
                        return;
                    }
                    _busy = false;
                    // (a newer point, such as a frame limit change, would be undone too: asked first)
                    await RestoreAsync(p, ask: RestorePoints.List(g.Bin).FirstOrDefault()?.Dir != p.Dir);
                    // the driver's frame generation is not in a restore point: the removal turned it off, and it stays off
                    if (smoothOff || afmfOff)
                        await Message(T("The launcher could not finish"), T("Frame generation was turned off with the removal and stays off; turn it on again in Components if you want it."));
                    return;
                }
            }
            else await ShowDialog(new ContentDialog { Title = title, Content = ErrorContent(text + (touched ? "\n\n" + T("Repair or remove sets the game right.") : "")),
                CloseButtonText = T("OK"), XamlRoot = Content.XamlRoot });
            _busy = false;
            Show(engineDone && !inline ? DoneStep : from);
        }
    }

    // a row of the task list; the returned action sets it to 0 pending, 1 working, 2 done, 3 failed
    Action<int> AddTask(Panel host, string text)
    {
        var row = new Grid { Padding = new Thickness(18, 10, 18, 10), ColumnSpacing = 12 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(20) });
        row.ColumnDefinitions.Add(new ColumnDefinition());
        var ring = new ProgressRing { Width = 16, Height = 16, IsActive = false };
        var icon = new FontIcon { FontSize = 15 };
        var label = new TextBlock { Text = text, FontSize = 13, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(label, 1);
        row.Children.Add(ring); row.Children.Add(icon); row.Children.Add(label);
        host.Children.Add(row);
        return state =>
        {
            ring.IsActive = state == 1;
            ring.Visibility = state == 1 ? Visibility.Visible : Visibility.Collapsed;
            icon.Visibility = state == 1 ? Visibility.Collapsed : Visibility.Visible;
            icon.Glyph = state == 3 ? "" : state == 2 ?"" : "";
            icon.Foreground = App.Brush(state == 3 ? "ApexWarningBrush" : state == 2 ? "ApexSuccessBrush" : "ApexMutedBrush");
            label.Foreground = App.Brush(state == 0 ? "ApexMutedBrush" : "ApexSoftTextBrush");
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
            DoneSummary.Children.Add(new TextBlock { Text = T("Installed: {0} {1}", Name(c), _state?.Installed(c) ?? ""), FontSize = 12, Foreground = App.Brush("ApexSoftTextBrush") });
        foreach (var c in _lastRemove)
            DoneSummary.Children.Add(new TextBlock { Text = T("Removed: {0}", Name(c)), FontSize = 12, Foreground = App.Brush("ApexSoftTextBrush") });
        // the driver and frame limit changes: done ones as their task, failed ones with the reason in the player's words
        foreach (var (label, kind, on, err) in _lastChanges)
            DoneSummary.Children.Add(new TextBlock
            {
                Text = err is null ? label : T("Not changed: {0}", label) + "\n" + ChangeError(kind, on, err),
                FontSize = 12, TextWrapping = TextWrapping.Wrap,
                Foreground = App.Brush(err is null ? "ApexSoftTextBrush" : "ApexWarningBrush"),
            });
        if (_lastChanges.Any(p => p.Error is not null && p.Error != "Dry run: Setup changes nothing.")) { DoneTitle.Text = T("Some changes did not apply"); DoneText.Text = ""; }
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
        if (s.RestartHost) ManageRow(T("Restart host"), T("Removes ts3wrap.exe and turns the restart host off in ApexRadiance.toml."), T("Remove"), () => RemoveAsync(Components.RestartHost));
        if (s.Modernization) ManageRow("Sims 3 Modernization", T("Removes ts3native and ts3fixes.package."), T("Remove"), () => RemoveAsync(Components.Modernization));
        ManageRow(T("Uninstall everything"), T("Removes what the launcher installed, and Apex Radiance, and puts back every file the launcher replaced."), T("Uninstall"), UninstallAll, danger: true);

        foreach (var p in RestorePoints.List(g.Bin))
        {
            var when = p.Created.ToLocalTime().ToString("g", LangCulture());
            ManageRow(T("Before: {0}", T(p.Label, TArgs(p.Args))), p.FileCount == 1 ? T("{0} \u00b7 1 file", when) : T("{0} \u00b7 {1} files", when, p.FileCount),
                T("Restore"), () => RestoreAsync(p), list: RestoreList);
        }
        if (RestoreList.Children.Count == 0)
            RestoreList.Children.Add(new TextBlock { Text = T("No restore points yet. The launcher makes one before every change."), Margin = new Thickness(18, 14, 18, 14),
                FontSize = 12, TextWrapping = TextWrapping.Wrap, Foreground = App.Brush("ApexMutedBrush") });
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
        var row = new Grid { Padding = new Thickness(18, 14, 18, 14), ColumnSpacing = 14, BorderBrush = App.Brush("ApexCardBorderBrush"), BorderThickness = new Thickness(0, list.Children.Count == 0 ? 0 : 1, 0, 0) };
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        text.Children.Add(new TextBlock { Text = desc, FontSize = 12, TextWrapping = TextWrapping.Wrap, Foreground = App.Brush("ApexMutedBrush") });
        var b = new Button { Content = action, MinWidth = 96, VerticalAlignment = VerticalAlignment.Center, IsEnabled = !_running && !_busy };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, action + " " + title + (restore ? ", " + desc : "")); // (two points can share a title)
        if (danger) { b.Background = Res("ApexDangerSoftBrush"); b.Foreground = Res("ApexDangerTextBrush"); }
        b.Click += async (_, _) => await run();
        Grid.SetColumn(b, 1);
        row.Children.Add(text); row.Children.Add(b);
        list.Children.Add(row);
    }

    // the point and every newer one, back as they were; the result on the bar of the screen it was started from
    async Task RestoreAsync(RestorePoint p, bool ask = true, InfoBar? bar = null)
    {
        if (_game is not { } g || _busy) return;
        if (_running) { SetInfo(bar ?? ManageInfo, InfoBarSeverity.Warning, T("Close The Sims 3, its launcher, the restart host (ts3wrap.exe) and the RTX Remix bridge (NvRemixBridge.exe) to continue."), ""); return; }
        if (ask && !await Confirm(T("Restore this point?"), T("The launcher puts back the files as they were before this change, and undoes every change made after it. Close the game first."), T("Restore"))) return;
        await Guarded(() =>
        {
            if (RestorePoints.Restore(g.Bin, p) is { Count: > 0 } failed)
                throw new IOException(T("The launcher could not put back {0}.", string.Join(", ", failed)));
        }, T("Changes undone."), bar);
    }

    async Task RemoveAsync(string c)
    {
        if (_game is not { } g || _busy || _running) return;
        if (!await Confirm(T("Remove {0}?", Name(c)), T("Close the game first. The launcher removes the files and puts back the ones it replaced."), T("Remove"), danger: true)) return;
        await Guarded(() => Engine.Remove(g, c), T("{0} removed.", Name(c)), label: "Remove {0}", args: Components.Name(c));
        AfterFrameGenOff();
        RefreshComponents();
    }

    async Task UninstallAll()
    {
        if (_game is not { } g || _busy || _running) return;
        if (!await Confirm(T("Uninstall everything?"), T("The launcher removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."), T("Uninstall"), danger: true)) return;
        await Guarded(() => Engine.RemoveAll(g), T("Everything the launcher installed was removed."), label: "Uninstall everything");
        AfterFrameGenOff();
        RefreshComponents();
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
        if (s.RestartHost) l.Add(Components.RestartHost);
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
        if (!await CanChangeHere(_step is HealthScreen or Home ? "health" : "manage")) return false;
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
        catch (Exception ex) { App.Log(ex); error = ErrorText(ex); }
        finally
        {
            _busy = false;
            if (!keepChoices) _defaultsFor = null;
            RefreshAll();
        }
        // after the refresh, which rewrites the screens' bars; Components' bar open again holds the graphics-reset warning for
        // the choices kept, so it stays (the suggestion's card going shows the result)
        if (ok) { if (!(bar == GraphicsInfo && GraphicsInfo.IsOpen)) SetInfo(bar, InfoBarSeverity.Success, done, ""); }
        else if (point?.Committed is { } p) SetInfo(bar, InfoBarSeverity.Error, T("The launcher could not finish"), error!, T("Undo these changes"),
            // a change made since then would be undone too: asked first
            () => _ = RestoreAsync(p, ask: RestorePoints.List(g.Bin).FirstOrDefault()?.Dir != p.Dir, bar));
        else SetInfo(bar, InfoBarSeverity.Error, T("The launcher could not finish"), error!);
        if (!ok) bar.Content = ErrorLinks(inBar: true); // (Copy details, Open log folder, as the dialogs have)
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
            var row = new Grid { Padding = new Thickness(18, 8, 12, 8), ColumnSpacing = 8, BorderBrush = App.Brush("ApexCardBorderBrush"), BorderThickness = new Thickness(0, LicenceList.Children.Count == 0 ? 0 : 1, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(new TextBlock { Text = T(name), FontSize = 13, VerticalAlignment = VerticalAlignment.Center });
            var l = new TextBlock { Text = lic, FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Foreground = App.Brush("ApexMutedBrush") };
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

    // a refresh after the control that asked for it has finished its own event: rebuilding a control from inside its own
    // handler is what WinUI fails fast on. Any exception is logged and shown, never left to end Setup.
    void Later(Action a) => DispatcherQueue.TryEnqueue(() =>
    {
        try { a(); }
        catch (Exception e) { ShowUnexpected(e); }
    });

    // an exception no handler caught: logged, then shown with Copy details (once at a time)
    bool _showingUnexpected;
    public void ShowUnexpected(Exception e, bool log = true)
    {
        if (log) App.Log(e);
        if (_showingUnexpected || Content?.XamlRoot is null) return;
        _showingUnexpected = true;
        DispatcherQueue.TryEnqueue(async () =>
        {
            try { await ShowDialog(new ContentDialog { Title = T("The launcher could not finish"), Content = ErrorContent(ErrorText(e)), CloseButtonText = T("OK"), XamlRoot = Content.XamlRoot }); }
            catch (Exception x) { App.Log(x); }
            finally { _showingUnexpected = false; }
        });
    }

    // ---------- checks before a change, errors ----------

    // Before any download (Engine.Preflight): what would stop the change is shown and the change waits; what it will do to the
    // player's own files (a foreign d3d9.dll, a second loader) is shown with Continue
    async Task<bool> PreflightOk(Game g, State s, List<string> comps)
    {
        var need = Engine.DownloadEstimate(comps, c => Repo.GetValueOrDefault(c) is { } r ? _latest.GetValueOrDefault(r) : null);
        List<Engine.Check> found;
        try { found = await Task.Run(() => Engine.Preflight(g, s, Scan.DocumentsFor(g), need, comps.Contains(Components.Dxvk) || comps.Contains(Components.Bridge), downloads: comps.Count > 0)); }
        catch (Exception e) { App.Log(e); return true; } // (a check that cannot run is no reason to stop)
        if (found.Count == 0) return true;
        var text = string.Join("\n\n", found.Select(f => "• " + T(f.Key, f.Args)));
        if (found.Any(f => f.Blocks)) { await Message(T("Before the launcher can continue"), text); return false; }
        return await Confirm(T("Before the launcher continues"), text, T("Continue"));
    }

    // an error in the player's words, then the raw text for a report
    string ErrorText(Exception ex)
    {
        var plain = ex switch
        {
            SetupException se => T(se.Key, se.Args),
            _ when Engine.IsNetwork(ex) => T("Check your internet connection, then try again."),
            UnauthorizedAccessException => T("Windows did not let the launcher change a file. If Windows Security's Controlled folder access is on, allow ApexLauncher.exe there; a read-only or locked file can also cause this."),
            IOException when Engine.IsVirusBlock(ex) => T("Your antivirus blocked a file the launcher needs; add an exception for {0} and open the launcher again.", GitHubSource.TempRoot + ", " + (_game?.Bin ?? "Game\\Bin")),
            IOException => T("A file the launcher needed is in use or could not be written. Close the game and other programs using it, then try again."),
            // an English text Setup throws and translates (Close The Sims 3 first, the restart host's wait): that text alone
            _ when Has(ex.Message) => T(ex.Message),
            _ => T("The launcher ran into a problem it did not expect."),
        };
        return ex is SetupException || Has(ex.Message) || plain == ex.Message ? plain : plain + "\n\n" + ex.Message;
    }

    // a dialog's text with "Copy details" (the log's context and last lines, for a report) and "Open log folder"
    StackPanel ErrorContent(string text)
    {
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true });
        p.Children.Add(ErrorLinks());
        return p;
    }

    // inBar: under an InfoBar's title, on the title's left edge (a link's own padding taken back) with the bar's bottom spacing
    StackPanel ErrorLinks(bool inBar = false)
    {
        var links = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, Margin = inBar ? new Thickness(-2, -4, 0, 14) : new Thickness(0) };
        var copy = new HyperlinkButton { Content = T("Copy details"), Padding = new Thickness(0) };
        copy.Click += (_, _) =>
        {
            var d = new Windows.ApplicationModel.DataTransfer.DataPackage();
            d.SetText(App.Details());
            Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(d);
        };
        var open = new HyperlinkButton { Content = T("Open log folder"), Padding = new Thickness(0) };
        open.Click += (_, _) =>
        {
            try { OpenFolder(App.LogDir); }
            catch (Exception e) { App.Log(e); }
        };
        links.Children.Add(copy);
        links.Children.Add(open);
        return links;
    }

    // ---------- dialogs and language ----------

    async Task Message(string title, string text) =>
        await ShowDialog(new ContentDialog { Title = title, Content = text, CloseButtonText = T("OK"), XamlRoot = Content.XamlRoot });

    // a button whose action removes or deletes: its text in red
    static Style DangerButton => new(typeof(Button)) { BasedOn = (Style)Application.Current.Resources["DefaultButtonStyle"], Setters = { new Setter(Control.ForegroundProperty, Res("ApexErrorBrush")) } };

    // the action in the accent colour; one that deletes or removes says so in red and lights neither button (Enter does nothing)
    async Task<bool> Confirm(string title, string text, string yes, bool danger = false)
    {
        var d = new ContentDialog { Title = title, Content = text, PrimaryButtonText = yes, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot,
            DefaultButton = danger ? ContentDialogButton.None : ContentDialogButton.Primary };
        if (danger) d.PrimaryButtonStyle = DangerButton;
        return await ShowDialog(d) == ContentDialogResult.Primary;
    }

    // every dialog in the launcher's near-black instead of Windows' grey: the body a step above the button strip, a quiet
    // border, a deeper shade behind (the dialog's template reads these from its own resources first)
    static void DarkDialog(ContentDialog d)
    {
        static SolidColorBrush B(uint argb) => new(Windows.UI.Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb));
        // (light: Windows' own dialog colours)
        if (_light) return;
        d.Resources["ContentDialogTopOverlay"] = B(0xFF161616);
        d.Resources["ContentDialogBackground"] = B(0xFF101010);
        d.Resources["ContentDialogBorderBrush"] = B(0xFF2C2C2C);
        d.Resources["ContentDialogSmokeFill"] = B(0xB3050507);
        d.Background = B(0xFF101010);
        d.BorderBrush = B(0xFF2C2C2C);
    }

    // WinUI allows one open ContentDialog: a newer one closes the open one (a Confirm closed so reads as Cancel)
    ContentDialog? _dialog;
    Task<ContentDialogResult>? _dialogShown;
    async Task<ContentDialogResult> ShowDialog(ContentDialog d)
    {
        while (_dialog is { } open) { open.Hide(); await _dialogShown!; }
        _dialog = d;
        d.Language = Root.Language; // (a dialog is a popup: it does not inherit the window's language)
        DarkDialog(d);
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
