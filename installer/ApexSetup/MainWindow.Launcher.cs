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
    int _healthTab, _newsTab;
    Session? _session;
    SessionInfo? _lastSession;
    Process? _maintenance;
    Tray? _tray;
    Crystal? _homeCrystal, _healthCrystal;
    readonly HashSet<int> _openArea = [];
    bool _showNotes;

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
        }
    }

    // Compat, Verify and the mod checks, run again (Check now, after a change, at start)
    // (off the window's thread: the saves, caches, script error logs and the conflict scan take seconds; the screens are
    // filled again when it is done, and a newer check replaces an older one still running)
    ConflictCheck? _conflicts;
    int _healthRun;
    bool _healthChecking;
    Task _healthDone = Task.CompletedTask;
    // (the task ends when this check's result is in _health; Play awaits it, the rest let it run)
    Task CheckHealth()
    {
        if (_game is not { } g || _state is not { } s) { _health = []; _healthState = HealthState.Ready; return Task.CompletedTask; }
        var run = ++_healthRun;
        var done = new TaskCompletionSource();
        _healthDone = done.Task;
        var (docs, apex, mods) = (Docs, _latest.GetValueOrDefault(Releases.Apex), _modsLoaded ? _mods.ToList() : null);
        if (_conflicts is null || _conflicts.Offline != _offline) _conflicts = new ConflictCheck { Offline = _offline };
        var conflicts = _conflicts;
        _healthChecking = true;
        // (the mods are read here when the Mods screen has not read them yet: Health checks them either way)
        List<ModFile>? read = null;
        _ = Task.Run(() =>
        {
            try
            {
                if (mods is null) try { mods = read = Mods.List(g, docs, kinds: false); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); }
                return Health.Gather(g, s, docs, apex, mods, mods is null ? null : conflicts);
            }
            catch (Exception e) { App.Log(e); return []; }
        }).ContinueWith(t =>
        {
            if (!DispatcherQueue.TryEnqueue(() =>
            {
                try
                {
                    if (run != _healthRun) return;
                    _healthChecking = false;
                    if (read is not null && !_modsLoaded) _mods = read; // (for Show in Mods; the Mods screen still reads its own)
                    _health = t.Result;
                    _healthState = Health.StateOf(_health);
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

    // waits for the newest health check, also one started while waiting
    async Task HealthReady()
    {
        while (true) { var t = _healthDone; await t; if (t == _healthDone) return; }
    }

    // ---------- Home ----------

    // Components: Apex Radiance large on the left with its four parts in two rows beside it; one column on a narrow
    // window; with no frame generation on this PC the restart host takes the whole second row
    bool _coreHooked;
    void OnCoreSized(object sender, SizeChangedEventArgs e)
    {
        if (!_coreHooked)
        {
            _coreHooked = true;
            var crystal = new Crystal(92);
            crystal.SetColor(new System.Numerics.Vector3(0.62f, 0.42f, 0.95f));
            ApexCrystalHost.Child = crystal;
            SmoothSection.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, (_, _) => LayCore());
            GraphicsInfo.RegisterPropertyChangedCallback(InfoBar.IsOpenProperty, (_, _) => ShowCoreNotes());
        }
        LayCore();
    }

    void LayCore()
    {
        var narrow = CoreGrid.ActualWidth is > 0 and < 760;
        var cols = CoreGrid.ColumnDefinitions;
        cols[1].Width = narrow ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
        cols[2].Width = narrow ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
        CoreGrid.ColumnSpacing = narrow ? 0 : 14;
        static void At(FrameworkElement el, int row, int col, int rows = 1, int span = 1) { Grid.SetRow(el, row); Grid.SetColumn(el, col); Grid.SetRowSpan(el, rows); Grid.SetColumnSpan(el, span); }
        if (narrow)
        {
            At(CoreApex, 0, 0); At(CoreModernization, 1, 0); At(CoreRenderer, 2, 0); At(SmoothSection, 3, 0); At(CoreRestart, 4, 0);
            return;
        }
        At(CoreApex, 0, 0, 2); At(CoreModernization, 0, 1); At(CoreRenderer, 0, 2); At(SmoothSection, 1, 1);
        if (SmoothSection.Visibility == Visibility.Visible) At(CoreRestart, 1, 2); else At(CoreRestart, 1, 1, 1, 2);
    }

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
        if (_running || Games.IsRunning()) { await Message(T("The Sims 3 is already running"), T("Switch to the game, or close it and press Play again.")); return; }
        if (update && WelcomeUpdates() is { Count: > 0 } ups)
        {
            await InstallAsync(ups);
            if (WelcomeUpdates().Count > 0) return; // (it failed or was cancelled: its message said why)
        }
        _ = CheckHealth();
        await HealthReady();
        // the fixes that are safe to make without asking (each keeps a copy of the file it changes)
        if (_settings.FixSafeAutomatically && _health.Any(i => i.Finding.CanFix))
        {
            if (!await Guarded(() => { Engine.CheckWritable(); foreach (var x in Compat.Check(g, Scan.Run(g)).Where(x => x.CanFix)) x.Fix!(); },
                T("Fixed."), null, "Fix incompatible settings")) return;
            RefreshGame();
            _ = CheckHealth();
            await HealthReady();
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
        // (kept for Health's last sessions, and for Mods' "new since last play")
        try { SessionHistory.Add(info); ModViewPrefs.LastPlay = info.Started + info.Played; } catch (Exception e) { App.Log(e); }
        _session = null;
        _running = Games.IsRunning();
        AppWindow.Show();
        Activate();
        _tray?.Hide();
        RefreshGame();
        _ = CheckHealth();
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
                ? T("Windows will ask for permission, then your changes are applied in their own window.")
                : T("Windows will ask for permission, then this screen opens in its own window to make the change."), T("Continue"))) return false;
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
        RefreshGame();
        _ = CheckHealth();
        if (_step is Welcome or GameStep && !FirstInstall) Show(Home); // (a first install finished in the administrator window)
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
        if (_checked == default) _ = CheckHealth();
        var top = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center, Spacing = 12 };
        _healthCrystal = new Crystal(170);
        _healthCrystal.SetLevel(HealthLevel, animate: false);
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
        check.Click += (_, _) => { _modsLoaded = false; RefreshGame(); _ = CheckHealth(); BuildHealth(); };
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
            var list = new StackPanel { Spacing = 10 };
            foreach (var it in needs) list.Children.Add(IssueCard(it));
            // the notes that ask nothing (one crash, a save without backup): one quiet line, opened on demand. The possible
            // overlaps (two mods in one behaviour area, unproven, often a dozen) stay in Checks > Mods only.
            var notes = _health.Where(i => i.Finding.Severity == Severity.Info && i.Finding.Text != "{0} may overlap in {1}: {2}").ToList();
            if (notes.Count > 0)
            {
                var strip = new Grid { ColumnSpacing = 12, Padding = new Thickness(18, 12, 14, 12), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush") };
                strip.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                strip.ColumnDefinitions.Add(new ColumnDefinition());
                strip.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var info = Lucide.Icon("info", 16, Res("ApexMutedBrush"));
                info.VerticalAlignment = VerticalAlignment.Center;
                strip.Children.Add(info);
                var what = new TextBlock { Text = notes.Count == 1 ? T("1 note that needs nothing from you") : T("{0} notes that need nothing from you", notes.Count), FontSize = 13, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
                Grid.SetColumn(what, 1);
                strip.Children.Add(what);
                var lines = new StackPanel { Spacing = 6, Padding = new Thickness(46, 0, 18, 4), Visibility = _showNotes ? Visibility.Visible : Visibility.Collapsed };
                foreach (var n in notes) lines.Children.Add(new TextBlock { Text = T(n.Finding.Text, n.Finding.Args), FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap });
                var see = new HyperlinkButton { Content = _showNotes ? T("Hide notes") : T("See notes"), FontSize = 13, Padding = new Thickness(0), VerticalAlignment = VerticalAlignment.Center };
                see.Click += (_, _) => { _showNotes = !_showNotes; lines.Visibility = _showNotes ? Visibility.Visible : Visibility.Collapsed; see.Content = _showNotes ? T("Hide notes") : T("See notes"); };
                Grid.SetColumn(see, 2);
                strip.Children.Add(see);
                list.Children.Add(Col(strip, lines));
            }
            body.Children.Add(list);
        }
        body.Children.Add(Eyebrow(T("CHECKS")));
        // one card, a row per area: its icon, what it covers, how it is; a click opens what it found
        var areas = new StackPanel();
        (HealthArea Area, string Name, string Detail, string Icon)[] all =
        [
            (HealthArea.ApexRadiance, "Apex Radiance", T("files, settings, one copy"), "sparkles"),
            (HealthArea.Loaders, T("Loaders"), T("ASI loader, Sims 3 Modernization"), "plug"),
            (HealthArea.Graphics, T("Graphics"), _state?.Layer.ToString() ?? "", "cpu"),
            (HealthArea.GameSettings, T("Game settings"), "Options.ini, Resource.cfg", "sliders-horizontal"),
            (HealthArea.Mods, T("Mods"), T("conflicts and what they need"), "layout-grid"),
            (HealthArea.Stability, T("Stability"), T("crashes, memory, script errors"), "heart-pulse"),
            (HealthArea.Saves, T("Saves"), T("size, backups, complete"), "folder"),
            (HealthArea.Disk, T("Space and caches"), T("free space, OneDrive, game caches"), "hard-drive"),
        ];
        foreach (var (area, name, detail, icon) in all)
        {
            var items = _health.Where(i => i.Area == area).ToList();
            var bad = items.Count(i => i.Finding.Severity != Severity.Info);
            var error = items.Any(i => i.Finding.Severity == Severity.Error);
            var open = _openArea.Contains((int)area);
            var head = new Grid { ColumnSpacing = 14, Padding = new Thickness(18, 12, 14, 12) };
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            head.ColumnDefinitions.Add(new ColumnDefinition());
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var glyph = Lucide.Icon(icon, 18, Res("ApexSoftTextBrush"));
            glyph.VerticalAlignment = VerticalAlignment.Center;
            head.Children.Add(glyph);
            var t = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
            t.Children.Add(new TextBlock { Text = name, FontSize = 14 });
            if (detail.Length > 0) t.Children.Add(new TextBlock { Text = detail, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            Grid.SetColumn(t, 1);
            head.Children.Add(t);
            var brush = bad == 0 ? "ApexSuccessBrush" : error ? "ApexErrorBrush" : "ApexWarningBrush";
            var status = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
            status.Children.Add(bad == 0 ? Lucide.Icon("check", 14, Res(brush)) : new Border { Width = 8, Height = 8, CornerRadius = new CornerRadius(4), Background = Res(brush), VerticalAlignment = VerticalAlignment.Center });
            status.Children.Add(new TextBlock { Text = bad == 0 ? T("All good") : bad == 1 ? T("1 to look at") : T("{0} to look at", bad), FontSize = 12, Foreground = Res(brush) });
            Grid.SetColumn(status, 2);
            head.Children.Add(status);
            var chevron = (LucideIcon)Lucide.Icon(open ? "chevron-down" : "chevron-right", 14, Res("ApexMutedBrush"));
            chevron.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(chevron, 3);
            head.Children.Add(chevron);
            var content = new StackPanel { Spacing = 8, Padding = new Thickness(50, 0, 18, 14), Visibility = open ? Visibility.Visible : Visibility.Collapsed };
            if (items.Count == 0) content.Children.Add(Muted(T("Nothing to report.")));
            foreach (var it in items) content.Children.Add(new TextBlock { Text = T(it.Finding.Text, it.Finding.Args), FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap });
            var toggle = new Button { Content = head, Padding = new Thickness(0), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
                Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(toggle, name);
            toggle.Click += (_, _) =>
            {
                var now = content.Visibility != Visibility.Visible;
                content.Visibility = now ? Visibility.Visible : Visibility.Collapsed;
                if (now) _openArea.Add((int)area); else _openArea.Remove((int)area);
                chevron.Kind = now ? "chevron-down" : "chevron-right";
            };
            var row = new StackPanel { BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, areas.Children.Count == 0 ? 0 : 1, 0, 0) };
            row.Children.Add(toggle);
            row.Children.Add(content);
            areas.Children.Add(row);
        }
        body.Children.Add(Card(areas, pad: false));
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

    // Needs you: one card per finding, its area's icon on the severity's tint, the first sentence as the title and the rest
    // as the detail line, the action on the right
    UIElement IssueCard(Health.Item it)
    {
        var f = it.Finding;
        var tint = ((SolidColorBrush)Res(f.Severity == Severity.Error ? "ApexErrorBrush" : "ApexWarningBrush")).Color;
        var g = new Grid { ColumnSpacing = 16, Padding = new Thickness(18, 16, 18, 16) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new Border { Width = 40, Height = 40, CornerRadius = new CornerRadius(10), VerticalAlignment = VerticalAlignment.Center,
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x2E, tint.R, tint.G, tint.B)), Child = Lucide.Icon(AreaIcon(it.Area), 20, new SolidColorBrush(tint)) });
        var (title, detail) = Sentences(T(f.Text, f.Args));
        var text = new StackPanel { Spacing = 3, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = title, FontSize = 15, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        if (detail.Length > 0) text.Children.Add(new TextBlock { Text = detail, FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        if (FindingAction(it) is { } act) { Grid.SetColumn(act, 2); act.VerticalAlignment = VerticalAlignment.Center; g.Children.Add(act); }
        return new Border { Style = (Style)Application.Current.Resources["ApexCard"], Child = g, Padding = new Thickness(0) };
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
        if (f.CanFix) return Button(T("Fix"), async () => { if (await Guarded(() => { Engine.CheckWritable(); f.Fix!(); }, T("Fixed."), null, "Fix incompatible settings")) { RefreshGame(); _ = CheckHealth(); BuildHealth(); } }, accent: true);
        if (ModsFor(it.Files) is { Count: > 0 } hits) return Button(T("Show in Mods"), () => ShowInMods(hits));
        if (f.Link is { } link) return Button(T("Read more"), () => Process.Start(new ProcessStartInfo(link) { UseShellExecute = true }));
        return null;
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
        if (FindingAction(it) is { } act) { Grid.SetColumn(act, 2); act.VerticalAlignment = VerticalAlignment.Center; g.Children.Add(act); }
        return g;
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
        var b = Button(action, async () => { await RestoreAsync(p); _ = CheckHealth(); BuildHealth(); });
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
        foreach (var raw in (r.Body ?? T("The notes could not be read right now. They show on GitHub.")).Replace("\r", "").Split('\n'))
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
        host.Children.Add(Row(T("Apex Launcher {0}", App.Version), T("Updates itself from each Apex Radiance release."), null));
        host.Children.Add(InstallModeRow());
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
                if (p.ExitCode != 0) left.Add(T("Some files could not be put back: see the log."));
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
                if (!needAdmin) Uninstall.MachineData();
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
        Uninstall.Launcher(keepData: keepSettings);
        await Message(T("Apex Launcher was uninstalled"), T("Everything it installed is gone, and the files it replaced are back."));
        Application.Current.Exit();
    }

    // Start with Windows: only the notification area icon until the player opens the window
    public void StartInTray() => TrayShow(T("Apex Launcher"));

    // a newer copy was installed while this one runs: this one closes so the new one opens in its place (never in the
    // middle of a change: the new one then shows this window and closes)
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
