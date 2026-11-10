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
        [Components.Apex] = Releases.Apex, [Components.Bridge] = Releases.Apex, [Components.Loader] = Releases.AsiLoader,
        [Components.S3ss] = Releases.S3ss, [Components.Modernization] = Releases.Modernization,
    };

    public MainWindow()
    {
        InitializeComponent();
        _screens = new UIElement[] { ScreenWelcome, ScreenGame, ScreenComponents, ScreenInstall, ScreenDone, ScreenManage, ScreenAbout };
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(DragArea);
        var scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new SizeInt32((int)(980 * scale), (int)(680 * scale)));
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

    string? Latest(string c) => _latest.GetValueOrDefault(Repo[c])?.Version;

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
            case Welcome: Show(GameStep); break;
            case GameStep: if (CanLeaveGame()) Show(ComponentsStep); break;
            case ComponentsStep: await InstallAsync(Selected()); break;
            case DoneStep: Close(); break;
        }
    }

    void UpdateFooter()
    {
        BackButton.Visibility = _step is GameStep or ComponentsStep ? Visibility.Visible : Visibility.Collapsed;
        NextButton.Visibility = _step is Welcome or GameStep or ComponentsStep or DoneStep ? Visibility.Visible : Visibility.Collapsed;
        NextText.Text = _step switch { Welcome => T("Get started"), ComponentsStep => T("Install"), DoneStep => T("Finish"), _ => T("Next") };
        NextButton.IsEnabled = _step switch
        {
            GameStep => CanLeaveGame(),
            ComponentsStep => !_offline && !_running && _game is not null,
            _ => true,
        };
        FooterNote.Text = _step switch
        {
            GameStep when _running => T("Close The Sims 3 to continue."),
            ComponentsStep when _offline => T("Setup needs the internet to download the files."),
            ComponentsStep => T("Nothing changes until you click Install."),
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
        else if (Newer(latest, inst))
            SetInfo(WelcomeInfo, InfoBarSeverity.Informational, T("Update available"), T("Apex Radiance {0} is installed; {1} is out.", inst!, latest!));
        else WelcomeInfo.IsOpen = false;
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
        AddInstalled("Apex Radiance", s.ApexVersion == "?" ? "" : s.ApexVersion, Components.Apex);
        AddInstalled("RTX Remix bridge", s.Layer == Layer.Bridge ? s.Installed(Components.Bridge) ?? "" : null, Components.Bridge);
        AddInstalled("Sims3SettingsSetter", s.S3ss ? s.Installed(Components.S3ss) ?? "" : null, Components.S3ss);
        AddInstalled("Sims 3 Modernization", s.Modernization ? s.Installed(Components.Modernization) ?? "" : null, Components.Modernization);

        ProblemList.Children.Clear();
        foreach (var p in s.Problems)
        {
            var parts = p.Split('|');
            ProblemList.Children.Add(new InfoBar { IsOpen = true, IsClosable = false, Severity = InfoBarSeverity.Warning, Message = T(parts[0], parts.Skip(1).ToArray()) });
        }
        if (s.Ts3NativeWrongBuild)
            ProblemList.Children.Add(new InfoBar { IsOpen = true, IsClosable = false, Severity = InfoBarSeverity.Warning,
                Message = T("ts3native.log says the installed Sims 3 Modernization is the build for the other edition. Setup installs the right one when you turn it on.") });
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

    void SetDefaults()
    {
        if (_state is not { } s || _defaultsFor == _game) return;
        _defaultsFor = _game;
        BridgeToggle.IsOn = s.Layer == Layer.Bridge;
        S3ssToggle.IsOn = s.S3ss && Newer(Latest(Components.S3ss), s.Installed(Components.S3ss));
        ModToggle.IsOn = s.Modernization && (s.Ts3NativeWrongBuild || Newer(Latest(Components.Modernization), s.Installed(Components.Modernization)));
    }

    void OnToggle(object sender, RoutedEventArgs e) => RefreshComponents();

    void RefreshComponents()
    {
        if (_state is not { } s || _game is not { } g) return;
        SetDefaults();
        var latest = Latest(Components.Apex) ?? "";
        ApexDesc.Text = s.ApexVersion is null ? T("Installs {0} into Game\\Bin.", latest)
            : s.ApexVersion == "?" ? T("Replaces the installed copy with {0}. Your settings stay.", latest)
            : Newer(latest, s.ApexVersion) ? T("Updates {0} to {1}. Your settings stay.", s.ApexVersion, latest)
            : T("{0} is installed and up to date. Setup copies it again.", s.ApexVersion);

        BridgeName.Text = T("RTX Remix bridge · optional");
        BridgeDetail.Visibility = BridgeToggle.IsOn ? Visibility.Visible : Visibility.Collapsed;
        BridgeDetail.Text = (s.Layer switch
        {
            Layer.Bridge => T("Installed: Setup updates it."),
            Layer.Native => T("Adds d3d9.dll to Game\\Bin; Remove takes it out again."),
            _ => T("Replaces your d3d9.dll ({0}); Setup keeps it and puts it back when you remove the bridge.", LayerName(s.Layer)),
        }) + " " + T("Set for {0}. Needs a Vulkan graphics card, like DXVK.", g.Exe);

        if (BridgeToggle.IsOn && s.Layer is Layer.Native or Layer.Other)
            SetInfo(GraphicsInfo, InfoBarSeverity.Warning, T("Graphics options will reset once"),
                T("The game resets its graphics options when it sees a different graphics card. The bridge reports a GeForce GTX 580, like DXVK; your game now uses {0}. Setup keeps a copy of Options.ini and puts it back if you remove the bridge.", LayerName(s.Layer)));
        else if (BridgeToggle.IsOn && s.Layer == Layer.Dxvk)
            SetInfo(GraphicsInfo, InfoBarSeverity.Informational, T("Your graphics options stay"),
                T("DXVK and the bridge both report a GeForce GTX 580, so the game keeps its settings."));
        else GraphicsInfo.IsOpen = false;

        S3ssPill.Text = Pill(s.S3ss, s.Installed(Components.S3ss), Latest(Components.S3ss), false);
        ModPill.Text = Pill(s.Modernization, s.Installed(Components.Modernization), Latest(Components.Modernization), s.Ts3NativeWrongBuild);
        ModToggle.IsEnabled = g.ModernizationSupported;
        if (!g.ModernizationSupported) ModToggle.IsOn = false;
        ModDesc.Text = g.ModernizationSupported
            ? T("Stutter, route and crash fixes by rodneypantonial: ts3native in Game\\Bin and ts3fixes.package in Mods\\Packages, from its GitHub release.")
            : T("Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47.");

        bool need = Engine.NeedsLoader(s, BridgeToggle.IsOn);
        LoaderPlanIcon.Glyph = need ? "" : "";
        LoaderPlanIcon.Foreground = (Brush)Application.Current.Resources[need ? "ApexAccentBrush" : "ApexSuccessBrush"];
        LoaderPlan.Text = !need ? T("ASI loader found: {0}.", s.Loader!)
            : s.LoaderIsD3d9 ? T("Your ASI loader is d3d9.dll, which the bridge replaces: Setup adds Ultimate ASI Loader as wininet.dll.")
            : T("No ASI loader found: Setup adds Ultimate ASI Loader as wininet.dll, from its GitHub release.");
        UpdateFooter();
    }

    string Pill(bool present, string? version, string? latest, bool wrong) =>
        wrong ? T("Wrong build") : !present ? T("Not installed") : version is null ? T("Installed")
        : Newer(latest, version) ? T("Update {0}", latest!) : T("Up to date");

    List<string> Selected()
    {
        var l = new List<string> { Components.Apex };
        if (BridgeToggle.IsOn) l.Add(Components.Bridge);
        if (S3ssToggle.IsOn) l.Add(Components.S3ss);
        if (ModToggle.IsOn && ModToggle.IsEnabled) l.Add(Components.Modernization);
        if (_state is { } s && Engine.NeedsLoader(s, BridgeToggle.IsOn)) l.Add(Components.Loader);
        return l;
    }

    static string Name(string c) => c switch
    {
        Components.Apex => "Apex Radiance", Components.Bridge => "RTX Remix bridge", Components.Loader => "Ultimate ASI Loader",
        Components.S3ss => "Sims3SettingsSetter", _ => "Sims 3 Modernization",
    };

    // ---------- install ----------

    async Task InstallAsync(List<string> comps)
    {
        if (_game is not { } g || _busy) return;
        _busy = true;
        var order = Components.All.Reverse().Where(comps.Contains).ToList(); // the engine's order
        InstallTitle.Text = T("Installing");
        Show(InstallStep);
        TaskList.Children.Clear();
        InstallBar.Value = 0;
        var rows = order.Select(c => AddTask(Name(c))).ToList();
        void Mark(int active)
        {
            for (int i = 0; i < rows.Count; i++)
                rows[i](i < active ? 2 : i == active ? 1 : 0);
        }
        Mark(0);
        var progress = new Progress<Progress>(p =>
        {
            InstallBar.Value = p.Fraction;
            Mark(p.Arg is null ? (p.Fraction >= 1 ? rows.Count : 0) : order.IndexOf(p.Arg));
        });
        try
        {
            await Task.Run(() => Engine.InstallAsync(g, order, new GitHubSource(), progress));
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
        if (s.S3ss) l.Add(Components.S3ss);
        if (s.Modernization && _game!.ModernizationSupported) l.Add(Components.Modernization);
        if (Engine.NeedsLoader(s, s.Layer == Layer.Bridge)) l.Add(Components.Loader);
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
