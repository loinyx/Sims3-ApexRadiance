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
    List<Health.Item> _healthAll = [], _health = []; // (all a check found, and without what the player ignored)
    HealthState _healthState;
    DateTime _checked;
    List<ModFile> _mods = [];
    HashSet<string> _modLooks = new(StringComparer.OrdinalIgnoreCase);
    bool _modsLoaded, _modsLoading;
    Session? _session;
    bool _starting;
    Process? _maintenance;
    Tray? _tray;
    Crystal? _homeCrystal, _healthCrystal;

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
            case GalleryScreen: BuildGallery(); break;
            case GameSettingsScreen: BuildGameSettings(); break;
            case WizardSettings: BuildWizardSettings(); break;
        }
    }

    // Compat, Verify and the mod checks, run again (Check now, after a change, at start)
    // (off the window's thread: the saves, caches, script error logs and the conflict scan take seconds; the screens are
    // filled again when it is done, and a newer check replaces an older one still running)
    ConflictCheck? _conflicts;
    int _healthRun;
    bool _healthChecking, _savesRecovered;
    Task _healthDone = Task.CompletedTask;
    // (the task ends when this check's result is in _health; Play awaits it, the rest let it run)
    Task CheckHealth()
    {
        if (_game is not { } g || _state is not { } s) { SetHealth([]); return Task.CompletedTask; }
        var run = ++_healthRun;
        var done = new TaskCompletionSource();
        _healthDone = done.Task;
        var (docs, apex, mods) = (Docs, _latest.GetValueOrDefault(Releases.Apex), _modsLoaded ? _mods.ToList() : null);
        if (_conflicts is null || _conflicts.Offline != _offline) _conflicts = new ConflictCheck { Offline = _offline, Ignored = _settings.IgnoredConflicts };
        var conflicts = _conflicts;
        _healthChecking = true;
        // (the mods are read here when the Mods screen has not read them yet: Health checks them either way)
        List<ModFile>? read = null;
        _ = Task.Run(List<Health.Item>? () =>
        {
            try
            {
                // (a merge cut off during its move, by a closed window or a crash: its originals go back before anything reads the mods or the game starts)
                try { ModMerge.Recover(docs); } catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { App.Log(e); }
                // (the same for a change to a save; only in the first check after the start, so it never meets a change being made)
                if (!_savesRecovered && !Maintenance) { _savesRecovered = true; SaveCopies.Recover(docs); }
                if (mods is null) try { mods = read = Mods.List(g, docs, kinds: false); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); }
                return Health.Gather(g, s, docs, apex, mods, mods is null ? null : conflicts);
            }
            catch (Exception e) { App.Log(e); return null; }
        }).ContinueWith(t =>
        {
            if (!DispatcherQueue.TryEnqueue(() =>
            {
                try
                {
                    if (run != _healthRun) return;
                    _healthChecking = false;
                    if (read is not null && !_modsLoaded) _mods = read; // (for Show in Mods; the Mods screen still reads its own)
                    SetHealth(t.Result ?? _healthAll); // (a check that failed keeps the last findings: an empty list would read "Every check passed")
                    _checked = DateTime.Now;
                    // (only the screens that show health are built again: Mods and the Gallery keep what the player is doing)
                    if (_step is Home or HealthScreen or WorldsScreen or SavesScreen or NewsScreen or SettingsScreen) RefreshLauncher();
                    else HealthBadge.Visibility = _game is not null && _healthState != HealthState.Ready ? Visibility.Visible : Visibility.Collapsed;
                }
                finally { done.TrySetResult(); }
            })) done.TrySetResult();
        });
        return done.Task;
    }

    bool IsIgnored(Health.Item i) => _settings.IgnoredFindings.Contains(i.Id);

    void SetHealth(List<Health.Item> all)
    {
        _healthAll = all;
        _health = all.Where(i => !IsIgnored(i)).ToList();
        _healthState = Health.StateOf(_health);
    }

    // waits for the newest health check, also one started while waiting
    async Task HealthReady()
    {
        while (true) { var t = _healthDone; await t; if (t == _healthDone) return; }
    }

    // ---------- Home ----------

    // how much needs attention, for the crystal's colour: nothing is 0, a blocked game 1; warnings fill the way between
    // (1 or 2 small ones a yellow-green, about 4 yellow, 8 or more orange)
    double HealthLevel => _healthState == HealthState.Blocked ? 1
        : _health.Count(i => i.Finding.Severity != Severity.Info) is var n && n == 0 ? 0 : Math.Min(0.8, 0.15 + n * 0.08);

    // the crystal's colour, at an alpha (0 to 255)
    Windows.UI.Color StateTint(byte alpha)
    {
        var c = Crystal.ColorAt(HealthLevel);
        return Windows.UI.Color.FromArgb(alpha, (byte)(c.X * 255), (byte)(c.Y * 255), (byte)(c.Z * 255));
    }

    // the card's own grey with a little of the state's colour, opaque: a fade from a transparent colour turns into a band of
    // the pure colour (the brush blends the colour before its transparency)
    Windows.UI.Color CardTint(double amount)
    {
        var t = StateTint(255);
        byte Mix(byte card, byte c) => (byte)Math.Round(card + (c - card) * amount);
        var card = Pick(0xFF141414, 0xFFFFFFFF);
        return Windows.UI.Color.FromArgb(255, Mix(card.R, t.R), Mix(card.G, t.G), Mix(card.B, t.B));
    }

    string HealthTitle() => _healthState switch
    {
        HealthState.Blocked => T("Apex Radiance can't start"),
        HealthState.NeedsYou => _health.Count(i => i.Finding.Severity != Severity.Info) is var n && n == 1 ? T("1 thing to look at") : T("{0} things to look at", _health.Count(i => i.Finding.Severity != Severity.Info)),
        _ => T("Ready to play"),
    };

    List<string> Screenshots(int n) => Gallery.List(Docs).Where(p => !p.Video).Take(n).Select(p => p.Path).ToList();

    // a pack's own icon from the player's install (its game definition file), else its short code on its colour
    static FrameworkElement PackBadge(Pack p, string? dir, double size)
    {
        // the pack's own icon; one without (none shipped, or a folder moved) takes the base game's from the folder above, and
        // if that is missing too the badge goes: no placeholder shapes
        var b = new Border { Width = size, Height = size };
        ToolTipService.SetToolTip(b, p.Name);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, p.Name);
        if (dir is null) { b.Visibility = Visibility.Collapsed; return b; }
        _ = Task.Run(() => Expansions.Icon(dir) ?? (Path.GetDirectoryName(dir) is { } up ? Expansions.Icon(up) : null)).ContinueWith(async t =>
        {
            if (await SaveCard.ImageOf(t.Result) is { } img) b.Child = new Image { Source = img, Width = size, Height = size };
            else b.Visibility = Visibility.Collapsed;
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

    // Play starts the game as it is; update (Update and play) puts the ready updates in first. Nothing is installed
    // without that choice.
    async Task PlayAsync(string? save, bool update = false)
    {
        if (_game is not { } g || _busy) return;
        // (a session still open: the game closed and its watch is waiting to see whether it restarts on its own)
        if (_running || _session is not null || Games.IsRunning()) { await Message(T("The Sims 3 is already running"), T("Switch to the game, or close it and press Play again.")); return; }
        if (_starting) return; // (a second press while the first is still checking Health)
        _starting = true;
        try { await StartChecked(g, save, update); }
        finally { _starting = false; }
    }

    async Task StartChecked(Game g, string? save, bool update)
    {
        if (update && WelcomeUpdates() is { Count: > 0 } pending && await UpdatesFirst(pending) is var go && go != true)
        {
            if (go is null) return; // (Cancel)
            update = false; // (Play without updating)
        }
        _playAnyway = false;
        if (update && WelcomeUpdates() is { Count: > 0 } ups2)
        {
            var ups = ups2;
            // (in the card, as an install: then straight to the game, from Home, with no done screen in between)
            _wizardFlow = _updatingForPlay = true;
            try { await InstallAsync(ups); }
            finally { _wizardFlow = _updatingForPlay = false; }
            if (WelcomeUpdates().Count > 0 && !_playAnyway) { if (_step is InstallStep or DoneStep) Show(Home); return; } // (it failed or was cancelled: its card said why)
            Show(Home);
        }
        // Play waits only for the checks that decide the start; the full check (mod conflicts, saves, script errors) runs
        // in the background and fills Health when it is done
        var (docs, apex) = (Docs, _latest.GetValueOrDefault(Releases.Apex));
        Task<List<Health.Item>> BeforePlay() => _state is { } s ? Task.Run(() => Health.Gather(g, s, docs, apex, beforePlay: true).Where(i => !IsIgnored(i)).ToList()) : Task.FromResult(new List<Health.Item>());
        var found = await BeforePlay();
        // the fixes that are safe to make without asking (each keeps a copy of the file it changes). Never an administrator
        // prompt from Play: where the game's folder needs one, they wait on Health. A fix that fails does not stop the start.
        if (_settings.FixSafeAutomatically && found.Any(i => i.Finding.CanFix) && (Elevation.IsElevated || DryRun || Elevation.CanWrite(g.Bin))
            && await Guarded(() => { Engine.CheckWritable(); foreach (var x in Compat.Check(g, Scan.Run(g)).Where(x => x.CanFix)) x.Fix!(); },
                T("Fixed."), null, "Fix incompatible settings"))
        {
            RefreshGame(); // (starts the full check too)
            found = await BeforePlay();
        }
        else _ = CheckHealth();
        if (Health.StateOf(found) == HealthState.Blocked)
        {
            var first = found.First(Health.StopsApex).Finding;
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
        PlayPendingOptions(g);
        try { p = Launch.Start(g, save); }
        catch (Exception e) { App.Log(e); await Message(T("The game did not start"), ErrorText(e)); return; }
        var session = _session = new Session(p, Docs);
        _running = true;
        if (_settings.HideWhilePlaying) { TrayShow(T("Apex Launcher · The Sims 3 is running")); AppWindow.Hide(); }
        RefreshAll();
        var info = await session.WatchAsync(new Progress<Session>(s =>
        {
            _tray?.Show(T("The Sims 3 · {0} · {1} MB", Duration(s.Played), s.MemoryBytes >> 20));
            if (_step == Home && Environment.TickCount64 % 30000 < 5000) BuildHome(); // (the playing time on Home, now and then)
        }));
        // (kept for Health's last sessions, and for Mods' "new since last play")
        try { SessionHistory.Add(info); ModViewPrefs.LastPlay = info.Started + info.Played; } catch (Exception e) { App.Log(e); }
        _session = null;
        _running = Games.IsRunning();
        PlayPendingOptions(g);
        AppWindow.Show();
        Activate();
        _tray?.Hide();
        RefreshGame(); // (starts the full check too)
        RefreshAll();
        if (Session.ShouldOffer(info, _state?.RestartHost ?? false))
        {
            var low = info.End == SessionEnd.LowMemory;
            var again = await Confirm(low ? T("You saved and quit with memory running low") : T("The game closed unexpectedly"),
                low ? T("Starting again frees the memory. The game opens where you saved.") : T("Your last save is safe. Start again where you left off."),
                T("Open this save again"));
            if (again && Saves.Newest(Docs) is { } last) await StartGameAsync(g, last.Name);
        }
    }

    void TrayShow(string tip)
    {
        _tray ??= new Tray(Path.Combine(AppContext.BaseDirectory, "Assets", "apex.ico"), () => DispatcherQueue.TryEnqueue(() => { AppWindow.Show(); Activate(); }),
            () => [(T("Open Apex Launcher"), () => DispatcherQueue.TryEnqueue(() => { AppWindow.Show(); Activate(); })),
                   // (never in the middle of an install: the same refusal as closing the window)
                   (T("Quit"), () => DispatcherQueue.TryEnqueue(() => { if (!QuitForNewerCopy()) _ = Message(T("The launcher is applying changes"), T("Don't close the launcher until it finishes.")); }))]);
        _tray.Show(tip);
    }

    static string Duration(TimeSpan t) => t.TotalHours >= 1 ? $"{(int)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}" : $"{t.Minutes}:{t.Seconds:00}";

    // ---------- the administrator window ----------

    // A change to the game's files: made here when this copy may write there, else in the administrator window, opened on
    // the same screen (Components: with the choices made here, applied there at once); this window waits, hidden, and reads
    // the game again when it closes. False when the change was not made here (the caller stops).
    // drivers: NVIDIA's and AMD's driver settings (Smooth Motion, AFMF) are machine-wide and always need it
    async Task<bool> CanChangeHere(string screen, bool drivers = false)
    {
        // (a dry run changes nothing and refuses in place: it never asks Windows for administrator)
        if (_game is not { } g || Elevation.IsElevated || DryRun || (!drivers && Elevation.CanWrite(g.Bin))) return true;
        if (Environment.ProcessPath is not { } exe) return true;
        var carry = screen == "components";
        if (!await Confirm(T("Administrator permission needed"), carry
                ? T("Windows asks for permission, then your changes apply in a separate window.")
                : T("Windows asks for permission, then this screen opens in a separate window."), T("Continue"))) return false;
        string? choices = null;
        try { if (carry) choices = Elevation.SaveChoices(ChoicesNow()); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); }
        _maintenance = Elevation.StartMaintenance(exe, g.Bin, screen, choices);
        if (_maintenance is null) { await Message(T("Nothing was changed"), T("Windows did not allow the change.")); return false; }
        AppWindow.Hide();
        try { await _maintenance.WaitForExitAsync(); } catch (Exception e) { App.Log(e); }
        _maintenance = null;
        AppWindow.Show();
        Activate();
        _defaultsFor = null;
        _modsLoaded = false;
        RefreshGame(); // (starts the full check too)
        if (_step is Welcome or GameStep or WizardSettings && !FirstInstall) Show(Home); // (a first install finished in the administrator window)
        else RefreshAll();
        return false;
    }

    // what Components shows now, for the administrator window (TakeChoices)
    Dictionary<string, string> ChoicesNow()
    {
        var d = new Dictionary<string, string>
        {
            ["renderer"] = _renderer, ["mod"] = B(ModToggle.IsOn), ["modexp"] = B(_modExp), ["s3ss"] = B(S3ssToggle.IsOn),
            ["mono"] = B(MonoToggle.IsOn), ["s3pp"] = B(S3ppToggle.IsOn), ["restart"] = B(RestartToggle.IsOn),
        };
        if (_smoothWant is { } sm) d["smooth"] = B(sm);
        if (_afmfWant is { } af) d["afmf"] = B(af);
        if (_fpsWant is { } fps) d["fps"] = fps.ToString(System.Globalization.CultureInfo.InvariantCulture);
        if (_s3ppArchive is { } a && _s3ppFor == _game?.Bin) d["s3pparchive"] = a;
        if (FirstInstall && _gsFor == _game?.Bin) foreach (var (k, v) in GameSettingsChanges().Where(c => GameSettings.InBin(c.Key))) d["game." + k] = v;
        return d;
        static string B(bool b) => b ? "1" : "0";
    }

    // the administrator window started with a launcher's choices: they go on Components; true when there were some
    bool TakeChoices()
    {
        var d = Elevation.Choices(Environment.GetCommandLineArgs());
        if (d.Count == 0 || _game is null || _step != ComponentsStep) return false;
        bool On(string k) => d.TryGetValue(k, out var v) && v == "1";
        if (d.TryGetValue("renderer", out var r) && r is RDxvk or RBridge or RNative or RKeep) _renderer = r;
        ModToggle.IsOn = On("mod");
        _modExp = On("modexp");
        (_modExp ? ModExperimental : ModStable).IsChecked = true;
        S3ssToggle.IsOn = On("s3ss");
        MonoToggle.IsOn = On("mono");
        S3ppToggle.IsOn = On("s3pp");
        if (RestartToggle.IsEnabled) RestartToggle.IsOn = On("restart");
        if (d.ContainsKey("smooth")) _smoothWant = On("smooth");
        if (d.ContainsKey("afmf")) _afmfWant = On("afmf");
        if (d.TryGetValue("fps", out var f) && int.TryParse(f, System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out var fps)) _fpsWant = fps;
        if (d.TryGetValue("s3pparchive", out var a) && File.Exists(a)) { _s3ppArchive = a; _s3ppFor = _game.Bin; }
        LoadGameSettings();
        foreach (var (k, v) in d.Where(x => x.Key.StartsWith("game.", StringComparison.Ordinal))) _gsWant[k["game.".Length..]] = v;
        RefreshAll();
        return true;
    }
    static bool DryRun => Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1";

    // the screen the administrator window opens on
    int MaintenanceScreen(string? screen) => screen switch { "manage" => Manage, "welcome" => Welcome, "health" => HealthScreen, _ => ComponentsStep };

    // a click on the sidebar item already highlighted (Settings while Repair or remove is open): back to that screen
    void OnNavInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
    {
        if (args.InvokedItemContainer?.Tag is not string tag || !int.TryParse(tag, out var to)) return;
        DispatcherQueue.TryEnqueue(() => { if (_step != to && !_busy && to >= Home) Show(to); });
    }

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
            var wrap = new CardGrid { MinItemWidth = 240 };
            var installed = Expansions.Installed(g);
            foreach (var w in list)
            {
                var card = new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(0), CornerRadius = new CornerRadius(14) };
                var col = new StackPanel();
                var pic = new Grid { Height = 150, Background = Res("ApexPillBrush"), CornerRadius = new CornerRadius(14, 14, 0, 0) };
                pic.SizeChanged += (_, e) => { var h = Math.Round(e.NewSize.Width * 0.56); if (Math.Abs(pic.Height - h) > 1) pic.Height = h; }; // (the picture keeps its shape as the card widens)
                var world = w;
                var packOf = w.Pack is { } wp ? Expansions.All.First(p => p.Short == wp) : null;
                var smallBadge = packOf is null ? null : PackBadge(packOf, installed.FirstOrDefault(x => x.Pack == packOf).Dir, 28);
                if (smallBadge is not null) smallBadge.Visibility = Visibility.Collapsed;
                _ = Task.Run(() => Worlds.Picture(world)).ContinueWith(async t =>
                {
                    if (await SaveCard.ImageOf(t.Result) is { } img) { pic.Children.Add(new Image { Source = img, Stretch = Stretch.UniformToFill }); if (smallBadge is not null) smallBadge.Visibility = Visibility.Visible; }
                    // a world without its own picture: its pack's icon, large, never a lot's picture
                    else if (packOf is not null) { var big = PackBadge(packOf, installed.FirstOrDefault(x => x.Pack == packOf).Dir, 72); big.HorizontalAlignment = HorizontalAlignment.Center; big.VerticalAlignment = VerticalAlignment.Center; pic.Children.Add(big); }
                    else pic.Children.Add(new FontIcon { Glyph = "\uE909", FontSize = 30, Foreground = Res("ApexIconMutedBrush") });
                }, TaskScheduler.FromCurrentSynchronizationContext());
                col.Children.Add(pic);
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Padding = new Thickness(14, 10, 14, 12) };
                var pack = w.Pack is { } sh ? Expansions.All.First(p => p.Short == sh) : null;
                if (pack is not null && smallBadge is not null) row.Children.Add(smallBadge);
                row.Children.Add(Col(Strong(w.Name), Muted(pack?.Name ?? label)));
                col.Children.Add(row);
                card.Child = col;
                wrap.Children.Add(card);
            }
            host.Children.Add(wrap);
        }
        host.Children.Add(Muted(T("Custom worlds go in Documents\\Electronic Arts\\The Sims 3\\InstalledWorlds; removing one never touches saves made in it.")));
    }

    // ---------- Health ----------

    // one session, crash or freeze of the Stability page: what happened and what caused it and, opened, the clues each log gave
    UIElement TimelineRow(CrashTimeline.Entry e)
    {
        var c = LangCulture();
        var ok = e.End == CrashTimeline.Ending.Normal;
        var gb = (e.Peak / (double)(1L << 30)).ToString("0.0", c);
        string title, body = "", chip = "", chipBrush = "ApexWarningBrush";
        switch (e.End)
        {
            case CrashTimeline.Ending.Normal: title = T("Closed normally"); break;
            case CrashTimeline.Ending.NoReport when e.Why == "driver":
            case CrashTimeline.Ending.Froze when e.Why == "driver":
                title = T("Froze when the graphics driver failed");
                body = T("The graphics driver reported a GPU error at that moment and the game stopped responding. Updating the driver, or turning off overlays while playing, usually helps.");
                chip = T("Graphics driver"); break;
            case CrashTimeline.Ending.Froze:
                title = T("Froze and was closed");
                body = T("The game stopped responding and was closed. No crash report names a file, so no mod is blamed.");
                chip = T("Froze"); chipBrush = "ApexMutedBrush"; break;
            case CrashTimeline.Ending.NoReport:
                title = T("Closed without a crash report");
                body = T("The game stopped without writing a crash report: it froze and was closed, or the graphics driver stopped. Nothing crashed in a mod.");
                chip = T("No report"); chipBrush = "ApexMutedBrush"; break;
            default:
                (title, body, chip) = e.Why switch
                {
                    "driver" => (T("Crashed when the graphics driver failed"), T("The driver reported a GPU error at the same moment. Updating the graphics driver usually helps."), T("Graphics driver")),
                    "memory" => (T("Crashed with memory nearly full"), T("Memory reached {0} of 4 GB. Fewer mods, less CC or a smaller world lowers it.", gb), T("Memory")),
                    "called" when e.Culprit is not null => (T("Crashed in a call from {0}", e.Culprit), T("{0} called {1}, and that call crashed.", e.Culprit, e.Into ?? ""), T("Mod")),
                    _ when e.Culprit is null => (T("Crashed in the game's own code"), T("No mod was calling it."), T("The game")),
                    _ when e.Renderer => (T("Crashed in {0}", e.Culprit), T("In the renderer's own code, while the game was drawing."), T("Renderer")),
                    _ => (T("Crashed in {0}", e.Culprit), T("In its own code."), T("Mod")),
                };
                if (e.Culprit is null && e.Why is null) chipBrush = "ApexMutedBrush";
                if (e.Earlier)
                {
                    body = e.Replaced is { } r ? T("That version was replaced on {0}. No crash in it since.", r.ToString("d MMM", c)) : T("That version was replaced since. No crash in it since.");
                    chip = T("Fixed by an update"); chipBrush = "ApexSuccessBrush";
                }
                break;
        }
        if (e.Count > 1) title += " · " + T("{0} times", e.Count);

        // one line: the day and time, a dot (green closed well, amber crashed or froze, faint when an update fixed it, grey
        // closed without a report), what happened with its cause, and the details on demand
        var row = new Grid { ColumnSpacing = 16, Padding = new Thickness(18, 12, 14, 12), MinHeight = 60 };
        foreach (var w in new[] { new GridLength(96), GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) row.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var day = e.Count > 1 && e.First.Date != e.When.Date ? $"{e.First.ToString("d MMM", c)} – {e.When.ToString("d MMM", c)}" : e.When.Date == DateTime.Today ? T("Today") : e.When.ToString("d MMM", c);
        var time = e.Count > 1 ? T("{0} crashes", e.Count) : e.Played is { } p && p.TotalMinutes >= 1 ? $"{e.When.ToString("t", c)} · {T("{0} min", (int)p.TotalMinutes)}" : e.When.ToString("t", c);
        var when = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
        when.Children.Add(new TextBlock { Text = day, FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        when.Children.Add(new TextBlock { Text = time, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        row.Children.Add(when);
        var dot = e.End switch { CrashTimeline.Ending.Normal => "ApexSuccessBrush", CrashTimeline.Ending.NoReport => "ApexMutedBrush", _ => "ApexWarningBrush" };
        var mark = new Border { Width = 9, Height = 9, CornerRadius = new CornerRadius(5), Background = Res(dot), Opacity = e.Earlier ? 0.45 : 1, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(mark, 1);
        row.Children.Add(mark);
        var what = new TextBlock { Text = title, FontSize = 14, FontWeight = ok ? FontWeights.Normal : FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center,
            Foreground = ok ? Res("ApexSoftTextBrush") : Res("TextFillColorPrimaryBrush") };
        Grid.SetColumn(what, 2);
        row.Children.Add(what);
        var right = ok ? (FrameworkElement)new TextBlock { Text = T("memory {0} of 4 GB", gb), FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center } : Chip(chip, chipBrush);
        Grid.SetColumn(right, 3);
        row.Children.Add(right);
        var all = new StackPanel();
        all.Children.Add(row);
        if (body.Length + e.Clues.Count == 0) return all;

        var inner = new StackPanel { Spacing = 10, Padding = new Thickness(155, 0, 18, 16), Visibility = Visibility.Collapsed };
        if (body.Length > 0) inner.Children.Add(new TextBlock { Text = body, FontSize = 13, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap });
        if (e.Clues.Count > 0)
        {
            // the clues, one per line with the log it came from
            var clues = new StackPanel { Spacing = 6, Padding = new Thickness(14, 12, 14, 12), CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Pick(0x80101010, 0x80F3F3F3)),
                BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(1) };
            string? lastSource = null;
            foreach (var cl in e.Clues)
            {
                var g = new Grid { ColumnSpacing = 10 };
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(140) });
                g.ColumnDefinitions.Add(new ColumnDefinition());
                // (the log's name once for its run of lines)
                g.Children.Add(new TextBlock { Text = cl.Source == lastSource ? "" : T(cl.Source), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
                var text = new TextBlock { Text = T(cl.Text, cl.Args), FontSize = 12, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
                Grid.SetColumn(text, 1);
                g.Children.Add(text);
                clues.Children.Add(g);
                lastSource = cl.Source;
            }
            inner.Children.Add(clues);
        }
        all.Children.Add(inner);
        var how = new Button { Content = T("Details"), MinWidth = 88, VerticalAlignment = VerticalAlignment.Center };
        how.Click += (_, _) =>
        {
            var show = inner.Visibility != Visibility.Visible;
            inner.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            how.Content = show ? T("Close") : T("Details");
        };
        Grid.SetColumn(how, 4);
        row.Children.Add(how);
        return all;
    }

    // a small rounded label in a tint of its colour
    static FrameworkElement Chip(string text, string brush)
    {
        var col = ((SolidColorBrush)Res(brush)).Color;
        return new Border { CornerRadius = new CornerRadius(10), Padding = new Thickness(10, 2, 10, 3), VerticalAlignment = VerticalAlignment.Center,
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x26, col.R, col.G, col.B)),
            Child = new TextBlock { Text = text, FontSize = 12, Foreground = new SolidColorBrush(col) } };
    }

    // a finding's first sentence, and the rest ("" when it is one sentence)
    static (string Title, string Detail) Sentences(string text)
    {
        var i = text.IndexOfAny(['.', '。']);
        while (i >= 0 && i + 1 < text.Length && text[i] == '.' && text[i + 1] != ' ') i = text.IndexOfAny(['.', '。'], i + 1); // "d3d9.dll", "17.11"
        return i < 0 || i + 1 >= text.Length ? (text.TrimEnd('.', '。'), "") : (text[..i], text[(i + 1)..].Trim());
    }

    static string AreaIcon(HealthArea a) => a switch
    {
        HealthArea.ApexRadiance => "sparkles", HealthArea.Loaders => "plug", HealthArea.Graphics => "cpu", HealthArea.GameSettings => "sliders-horizontal",
        HealthArea.Mods => "layout-grid", HealthArea.Stability => "heart-pulse", HealthArea.Saves => "folder", _ => "hard-drive",
    };

    Button? FindingAction(Health.Item it)
    {
        var f = it.Finding;
        // a suggestion (clear the caches, back up a save): offered under its own name, never run by Fix or Repair
        if (f.CanApply && it.Area == HealthArea.Mods) return Button(T(f.Suggest!), async () =>
        {
            try { await ModWork(() => { f.Fix!(); return 0; }); _healthNote = (T("Applied."), null, null); }
            catch (Exception e) { App.Log(e); await Message(T("The launcher could not finish"), ErrorText(e)); }
            _modsLoaded = false;
            RefreshGame();
            BuildHealth();
        });
        if (f.CanApply) return Button(T(f.Suggest!), async () => { if (await Guarded(() => { Engine.CheckWritable(); f.Fix!(); }, T("Applied."), null, "Apply suggestion: {0}", args: f.Suggest!)) { _modsLoaded = false; RefreshGame(); BuildHealth(); } });
        if (f.CanFix) return Button(T("Fix"), async () => { if (await Guarded(() => { Engine.CheckWritable(); f.Fix!(); }, T("Fixed."), null, "Fix incompatible settings")) { RefreshGame(); BuildHealth(); } }, accent: true);
        if (ModsFor(it.Files) is { Count: > 0 } hits) return Button(T("Show in Mods"), () => ShowInMods(hits));
        if (f.Link is { } link) return Button(T("Read more"), () => Process.Start(new ProcessStartInfo(link) { UseShellExecute = true }));
        // (what is missing is installed in Components)
        if (f.Severity == Severity.Error && it.Area is (HealthArea.ApexRadiance or HealthArea.Loaders)) return Button(T("Components"), () => Show(ComponentsStep), accent: true);
        return null;
    }

    // the player's mods a finding names (by full path, game-relative path or file name)
    List<ModFile> ModsFor(IReadOnlyList<string> files) => files.Count == 0 ? [] : _mods.Where(m => files.Any(f =>
        f.Equals(m.Path, StringComparison.OrdinalIgnoreCase) || f.Replace('/', '\\').EndsWith(m.Rel, StringComparison.OrdinalIgnoreCase)
        || f.Equals(m.Name, StringComparison.OrdinalIgnoreCase))).ToList();

    // Mods, opened on those mods: one found by name and chosen, several listed in Needs a look
    void ShowInMods(List<ModFile> hits)
    {
        _modPlace = hits.Count == 1 ? (hits[0].FileType == "asi" ? AsiMods : AllMods) : LookMods;
        ModSearch.Text = hits.Count == 1 ? Path.GetFileNameWithoutExtension(hits[0].Name) : "";
        _folderSig = "";
        Show(ModsScreen);
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            var list = _modsGrid ? (ListViewBase)ModGrid : ModList;
            foreach (var row in _modShown.Where(r => hits.Any(h => h.Path.Equals(r.File.Path, StringComparison.OrdinalIgnoreCase))))
                list.SelectedItems.Add(row);
        });
    }

    void CopyReport()
    {
        var lines = new List<string> { App.Details(), "", "Health: " + _healthState };
        lines.AddRange(_healthAll.Select(i => $"[{i.Finding.Severity}{(IsIgnored(i) ? ", ignored" : "")}] {i.Area}: {string.Format(i.Finding.Text, i.Finding.Args)}"));
        var dp = new Windows.ApplicationModel.DataTransfer.DataPackage();
        dp.SetText(string.Join("\r\n", lines));
        Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(dp);
    }

    // ---------- What's new ----------

    // What's new: one page, a card per project with its newest notes (the current one's layout), Apex Radiance and Sims 3
    // Modernization first, then the optional parts that are installed; the install steps each release repeats are left out
    void BuildNews()
    {
        var host = ScreenNews;
        host.Children.Clear();
        host.Children.Add(Col(Heading(T("What's new")), Muted(T("Release notes from each project's GitHub page."))));
        var s = _state;
        var shown = new List<(string Name, Release? Release, string? Installed)>
        {
            ("Apex Radiance", _latest.GetValueOrDefault(Releases.Apex), s?.ApexVersion),
            ("Sims 3 Modernization", ModRelease() ?? _latest.GetValueOrDefault(Releases.Modernization), Known(Components.Modernization)),
        };
        // (an optional part shows when it is installed: its notes concern the player then)
        if (s?.Loader is not null) shown.Add(("Ultimate ASI Loader", _latest.GetValueOrDefault(Releases.AsiLoader), s.Installed(Components.Loader)));
        if (s?.MonoPatcher == true) shown.Add(("MonoPatcher", _latest.GetValueOrDefault(Releases.MonoPatcher), s.Installed(Components.MonoPatcher)));
        if (s?.Layer == Layer.Dxvk) shown.Add(("DXVK", _latest.GetValueOrDefault(Releases.Dxvk), s.Installed(Components.Dxvk)));
        if (shown.All(x => x.Release is null))
        {
            host.Children.Add(Card(Muted(_offline ? T("No internet connection. The notes show once the launcher can reach GitHub.") : T("Checking the latest versions…"))));
            return;
        }
        foreach (var (name, r, installed) in shown)
        {
            if (r is null) continue;
            host.Children.Add(Section(name));
            var notes = new StackPanel { Spacing = 8 };
            var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            head.Children.Add(new TextBlock { Text = r.Version, FontSize = 20, FontWeight = FontWeights.SemiBold });
            var same = installed is { Length: > 0 } && installed.TrimStart('v', 'V') == r.Version.TrimStart('v', 'V');
            head.Children.Add(Pill(same ? T("Installed") : installed is not { Length: > 0 } ? T("Latest") : T("Newer than yours"), same ? "ApexSuccessBrush" : "ApexAccentBrush"));
            notes.Children.Add(head);
            var lines = NotesLines(r.Body ?? T("The notes didn't load. You can read them on GitHub."), r.Version).ToList();
            // (long notes show about their first screen; the rest opens in place)
            const int Budget = 1400;
            var more = new StackPanel { Spacing = 8, Visibility = Visibility.Collapsed };
            var used = 0;
            foreach (var l in lines)
            {
                (used < Budget ? notes : more).Children.Add(l);
                used += (l as TextBlock)?.Text.Length ?? 0;
            }
            if (more.Children.Count > 0)
            {
                notes.Children.Add(more);
                var toggle = new HyperlinkButton { Content = T("Show all"), Padding = new Thickness(0), FontSize = 13 };
                toggle.Click += (_, _) => { var open = more.Visibility != Visibility.Visible; more.Visibility = open ? Visibility.Visible : Visibility.Collapsed; toggle.Content = open ? T("Show less") : T("Show all"); };
                notes.Children.Add(toggle);
            }
            var repo = r.Repo;
            var tag = r.Tag;
            notes.Children.Add(Link(T("Open on GitHub"), () => Process.Start(new ProcessStartInfo($"https://github.com/{repo}/releases/tag/{Uri.EscapeDataString(tag)}") { UseShellExecute = true })));
            host.Children.Add(Card(notes));
        }
    }

    // a release body's lines as the page shows them: headings, bullets and paragraphs, without the install or download steps,
    // tables and pictures
    IEnumerable<FrameworkElement> NotesLines(string body, string? version = null)
    {
        var skip = false;
        foreach (var raw in body.Replace("\r", "").Split('\n'))
        {
            var line = System.Net.WebUtility.HtmlDecode(raw).TrimEnd(); // (&nbsp; and the like, as GitHub shows them)
            if (line.Length == 0) continue;
            if (line.StartsWith('#'))
            {
                var title = line.TrimStart('#', ' ');
                // (a heading that only repeats the version shown above it)
                if (version is not null && title.TrimStart('v', 'V').StartsWith(version.TrimStart('v', 'V'), StringComparison.OrdinalIgnoreCase)) continue;
                skip = System.Text.RegularExpressions.Regex.IsMatch(title, @"\b(install|installation|download|downloads)\b", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                if (!skip) yield return new TextBlock { Text = Plain(title), FontSize = 15, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 8, 0, 0), TextWrapping = TextWrapping.Wrap };
                continue;
            }
            if (skip || line.Trim().Trim('-', '*', '_').Length == 0 || line.TrimStart().StartsWith('|') || line.TrimStart().StartsWith("![") || line.TrimStart().StartsWith("<")) continue;
            var t = line.TrimStart();
            yield return Body(t.StartsWith("- ") || t.StartsWith("* ") ? "• " + Plain(t[2..]) : Plain(t));
        }
    }

    static string Plain(string md) => System.Text.RegularExpressions.Regex.Replace(md.Replace("**", "").Replace("`", ""), @"\[([^\]]+)\]\([^)]+\)", "$1"); // ([text](link) reads as text)
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
        host.Children.Add(Toggle(T("Apply safe fixes without asking"), T("Like the Resource.cfg and Edge Smoothing fixes. Each file is copied first, so you can undo them."), _settings.FixSafeAutomatically,
            on => { _settings.FixSafeAutomatically = on; _settings.Save(); }));
        host.Children.Add(Section(T("Updates")));
        host.Children.Add(Row(T("Apex Launcher {0}", App.Version), T("Updates itself from each Apex Radiance release."), null));
        host.Children.Add(Toggle(T("Install updates when I press Play"), T("Ready updates install first, then the game starts. When off, Play starts the game as it is."), _settings.InstallUpdatesOnPlay,
            on => { _settings.InstallUpdatesOnPlay = on; _settings.Save(); }));
        host.Children.Add(InstallModeRow());
        host.Children.Add(Section(T("Home")));
        var picture = new ComboBox { MinWidth = 200 };
        string[] kinds = ["gallery", "household", "none"];
        foreach (var k in kinds) picture.Items.Add(k switch { "gallery" => T("A picture from the gallery"), "household" => T("The household's photo"), _ => T("No picture") });
        picture.SelectedIndex = Math.Max(0, Array.IndexOf(kinds, _settings.HomePicture));
        picture.SelectionChanged += (_, _) => { _settings.HomePicture = kinds[Math.Max(0, picture.SelectedIndex)]; _settings.Save(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(picture, T("Picture on top"));
        host.Children.Add(Row(T("Picture on top"), T("The gallery picture you chose there or the newest one, the photo the game keeps of the household, or none."), picture));
        var chosen = (_gal ??= Gallery.Load()).HomePicture is { } hp && File.Exists(hp);
        var choose = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        choose.Children.Add(Button(T("Choose in the gallery"), () => Show(GalleryScreen)));
        if (chosen) choose.Children.Add(Button(T("Use the newest"), () => { _gal!.HomePicture = null; Gallery.Save(_gal); BuildSettings(); }));
        host.Children.Add(Row(T("Gallery picture"), chosen ? T("The one you chose in the gallery (Use on Home).") : T("The newest picture you took. In the gallery, Use on Home keeps one."), choose));
        int[] heights = [240, 330, 440];
        var height = new ComboBox { MinWidth = 200 };
        foreach (var label in new[] { T("Low"), T("Medium"), T("Tall") }) height.Items.Add(label);
        height.SelectedIndex = Math.Max(0, Array.IndexOf(heights, _settings.HomePictureHeight) is var hi && hi >= 0 ? hi : 1);
        height.SelectionChanged += (_, _) => { _settings.HomePictureHeight = heights[Math.Max(0, height.SelectedIndex)]; _settings.Save(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(height, T("Picture height"));
        host.Children.Add(Row(T("Picture height"), T("How much of the page the picture takes."), height));
        var strength = new Slider { Minimum = 30, Maximum = 100, StepFrequency = 5, Width = 200, Value = Math.Clamp(_settings.HomePictureStrength, 30, 100) };
        strength.ValueChanged += (_, e) => { _settings.HomePictureStrength = (int)e.NewValue; _settings.Save(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(strength, T("Picture strength"));
        host.Children.Add(Row(T("Picture strength"), T("Lower it for a quieter picture behind the save's name."), strength));
        host.Children.Add(Section(T("Window")));
        string[] backs = ["mica", "micaalt", "acrylic", "solid"];
        var back = new ComboBox { MinWidth = 200 };
        foreach (var label in new[] { T("Mica (Windows default)"), T("Mica, stronger"), T("Acrylic"), T("Solid colour") }) back.Items.Add(label);
        back.SelectedIndex = Math.Max(0, Array.IndexOf(backs, _settings.Backdrop));
        back.SelectionChanged += (_, _) => { _settings.Backdrop = backs[Math.Max(0, back.SelectedIndex)]; _settings.Save(); ApplyBackdrop(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(back, T("Window background"));
        host.Children.Add(Row(T("Window background"), T("Mica and Acrylic take a tint from your desktop wallpaper; Solid colour takes none."), back));
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
        GroupCards(host);
    }

    // ---------- uninstall ----------

    // Settings' Uninstall and Windows' Installed apps (--uninstall)
    public async Task UninstallAsync(bool fromWindows = false)
    {
        if (Games.IsRunning()) { await Message(T("Close the game first"), T("Close The Sims 3, then uninstall.")); return; }
        var keep = new CheckBox { Content = T("Keep my settings, albums and mod notes"), IsChecked = true };
        var text = new StackPanel { Spacing = 12 };
        text.Children.Add(Body(T("Apex Radiance and every component the launcher installed are removed, and every file they replaced is put back. Mods you switched off are put back on. Your saves and your own mods stay.")));
        text.Children.Add(keep);
        var ok = await ShowDialog(new ContentDialog { Title = T("Uninstall Apex Launcher"), Content = text, PrimaryButtonText = T("Uninstall"), CloseButtonText = T("Cancel"),
            DefaultButton = ContentDialogButton.None, XamlRoot = Content.XamlRoot, PrimaryButtonStyle = DangerButton }) == ContentDialogResult.Primary;
        if (!ok) { if (fromWindows) Application.Current.Exit(); return; }
        _busy = true;
        var keepSettings = keep.IsChecked == true;
        // (from Installed apps a launcher can still be open, in the tray too: it closes first, or its exe and folder stay)
        if (fromWindows) await Task.Run(CloseOtherCopies);
        var left = new List<string>();
        try
        {
            var games = await Task.Run(Uninstall.Games);
            var needAdmin = !Elevation.IsElevated && games.Any(g => !Elevation.CanWrite(g.Bin));
            if (needAdmin && Environment.ProcessPath is { } exe)
            {
                var p = Elevation.StartMaintenance(exe, "*", "uninstall");
                if (p is null) { _busy = false; await Message(T("Nothing was changed"), T("Windows did not allow the change.")); return; }
                await p.WaitForExitAsync();
                if (p.ExitCode != 0) left.Add(T("Some files weren't restored. See the log for details."));
            }
            left.AddRange(await Task.Run(() =>
            {
                var stayed = new List<string>();
                foreach (var g in games)
                {
                    try { if (!needAdmin) stayed.AddRange(Uninstall.FromGame(g, Scan.DocumentsFor(g))); }
                    catch (Exception e) { App.Log(e); stayed.Add(g.Root + ": " + ErrorText(e)); }
                    // (the player's own folders, as the player, whatever happened in the game folder)
                    try { stayed.AddRange(Uninstall.FromDocuments(g, Scan.DocumentsFor(g), keepSettings)); }
                    catch (Exception e) { App.Log(e); stayed.Add(Scan.DocumentsFor(g) + ": " + ErrorText(e)); }
                }
                if (!needAdmin)
                {
                    // (frame generation the drivers did not turn off: the uninstall is not done, and its record stays)
                    var (nv, amd) = Uninstall.MachineData();
                    if (nv is not null) { App.Log(new InvalidOperationException("Smooth Motion off: " + nv)); stayed.Add(T("Turn off {0}", "NVIDIA Smooth Motion") + ": " + T(Engine.FriendlyFrameGenError(nv, false, false))); }
                    if (amd is not null) { App.Log(new InvalidOperationException("AFMF off: " + amd)); stayed.Add(T("Turn off {0}", "AMD Fluid Motion Frames") + ": " + T(Engine.FriendlyFrameGenError(amd, true, false))); }
                }
                return stayed;
            }));
        }
        catch (Exception e) { App.Log(e); left.Add(ErrorText(e)); }
        _busy = false;
        if (left.Count > 0)
        {
            // (the launcher stays: without it nothing could finish the job or put the rest back)
            await Message(T("The uninstall did not finish"), T("Some things stayed: {0}. Apex Launcher stays so you can try again.", string.Join("; ", left)));
            if (fromWindows) Application.Current.Exit();
            return;
        }
        // the launcher's own removal comes after the message: its delayed delete of the exe and folders waits a few seconds,
        // less than a player takes to read it (and it still runs when the window is closed instead of OK)
        var gone = false;
        void RemoveLauncher() { if (gone) return; gone = true; Uninstall.Launcher(keepData: keepSettings); }
        AppWindow.Closing += (_, _) => RemoveLauncher();
        await Message(T("Apex Launcher was uninstalled"), T("Everything it installed was removed. The files it replaced are back."));
        RemoveLauncher();
        Application.Current.Exit();
    }

    static void CloseOtherCopies()
    {
        try { if (EventWaitHandle.TryOpenExisting(@"Local\ApexLauncher.Replace", out var close)) using (close) close.Set(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); }
        foreach (var p in System.Diagnostics.Process.GetProcessesByName("ApexLauncher").Where(p => p.Id != Environment.ProcessId))
            try { using (p) p.WaitForExit(10000); } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
    }

    // What Play puts in first, each with its versions and the first line of its notes: update and play, play as it is, or
    // nothing (true, false, null)
    async Task<bool?> UpdatesFirst(List<string> ups)
    {
        var body = new StackPanel { Spacing = 12, Width = 480 };
        body.Children.Add(new TextBlock { Text = T("Then the game starts. Your settings stay."), Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        var list = new StackPanel();
        foreach (var c in ups)
        {
            var g = new Grid { ColumnSpacing = 14, Padding = new Thickness(16, 12, 16, 12), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, list.Children.Count == 0 ? 0 : 1, 0, 0) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            FrameworkElement icon = c == Components.Apex
                ? new Image { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = 32, Height = 32 }
                : new Border { Width = 32, Height = 32, CornerRadius = new CornerRadius(9), Background = Res("ApexPillBrush"), Child = Lucide.Icon(c == Components.Modernization ? "hammer" : c is Components.Bridge or Components.Dxvk ? "layers" : "package", 15, Res("ApexAccentBrush")) };
            icon.VerticalAlignment = VerticalAlignment.Top;
            g.Children.Add(icon);
            var words = new StackPanel { Spacing = 2 };
            var head = new TextBlock { FontSize = 14, TextWrapping = TextWrapping.Wrap };
            head.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = Name(c), FontWeight = FontWeights.SemiBold });
            var from = InstalledOf(c);
            if (ReleaseOf(c)?.Version is { } to)
                head.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "  " + (from is { Length: > 0 } && from != "?" ? from + " \u2192 " + to : to), Foreground = Res("ApexMutedBrush") });
            words.Children.Add(head);
            if (Paragraphs(ReleaseOf(c)?.Body).FirstOrDefault() is { } headline)
                words.Children.Add(new TextBlock { Text = headline, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis });
            Grid.SetColumn(words, 1);
            g.Children.Add(words);
            list.Children.Add(g);
        }
        body.Children.Add(new Border { Child = list, CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush") });
        var r = await ShowDialog(new ContentDialog
        {
            Title = ups.Count == 1 ? T("1 update installs first") : T("{0} updates install first", ups.Count), Content = body,
            PrimaryButtonText = T("Update and play"), SecondaryButtonText = T("Play without updating"), CloseButtonText = T("Cancel"),
            DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
        });
        return r == ContentDialogResult.Primary ? true : r == ContentDialogResult.Secondary ? false : null;
    }

    // Start with Windows: only the notification area icon until the player opens the window
    public void StartInTray() => TrayShow(T("Apex Launcher"));

    // a newer copy was installed while this one runs: this one closes so the new one opens in its place (never in the
    // middle of a change: the new one then shows this window and closes)
    // mod work (merge, split, take out, edit) off the UI thread, with the window and Quit held until it ends
    async Task<TR> ModWork<TR>(Func<TR> work)
    {
        _busy = true;
        try { return await Task.Run(work); }
        finally { _busy = false; }
    }

    public bool QuitForNewerCopy()
    {
        if (_busy) return false;
        _tray?.Dispose();
        Application.Current.Exit();
        return true;
    }

    // Installed apps' Uninstall: the window opens on the uninstall question
    public void UninstallWhenShown() => ((FrameworkElement)Content).Loaded += async (_, _) => await UninstallAsync(fromWindows: true);

    // the administrator copy's uninstall (no window): every game, then the machine-wide markers
    // (the Documents part runs afterwards in the player's copy)
    public static int UninstallElevated()
    {
        var failed = false;
        foreach (var g in Uninstall.Games())
        {
            try { if (Uninstall.FromGame(g, Scan.DocumentsFor(g)).Count > 0) failed = true; }
            catch (Exception e) { App.Log(e); failed = true; }
        }
        var (nv, amd) = Uninstall.MachineData();
        if (nv is not null) { App.Log(new InvalidOperationException("Smooth Motion off: " + nv)); failed = true; }
        if (amd is not null) { App.Log(new InvalidOperationException("AFMF off: " + amd)); failed = true; }
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
    static TextBlock Eyebrow(string t) => new() { Text = t, FontSize = 11, FontWeight = FontWeights.SemiBold, CharacterSpacing = 100, Foreground = Res("ApexMutedBrush") };
    static TextBlock Section(string t) => new() { Text = t, FontSize = 15, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 16, 0, 0) };

    // rows of one section in one card, divided by hairlines, instead of a card each (Settings): calmer, and the eye reads
    // the section as one group
    static void GroupCards(Panel host)
    {
        var kids = host.Children.ToList();
        host.Children.Clear();
        StackPanel? group = null;
        foreach (var k in kids)
        {
            if (k is Border { Child: { } inner } card && card.Style == Application.Current.Resources["ApexCard"])
            {
                if (group is null)
                {
                    group = new StackPanel();
                    host.Children.Add(new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(0), Child = group });
                }
                card.Child = null;
                group.Children.Add(new Border
                {
                    Child = inner, Padding = new Thickness(18, 14, 18, 14), BorderBrush = Res("ApexCardBorderBrush"),
                    BorderThickness = new Thickness(0, group.Children.Count == 0 ? 0 : 1, 0, 0),
                });
                continue;
            }
            group = null;
            host.Children.Add(k);
        }
    }
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
        try { Directory.CreateDirectory(dir); Process.Start(new ProcessStartInfo(dir) { UseShellExecute = true, Verb = "open" }); }
        catch (Exception e) { App.Log(e); }
    }
}
