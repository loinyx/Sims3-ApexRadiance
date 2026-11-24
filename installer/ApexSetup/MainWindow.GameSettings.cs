using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using static ApexSetup.Strings;

namespace ApexSetup;

// Game settings: options the game keeps in its files (GameSettings), changed together with Apply, and the checks on the
// graphics card and folders. The install has the same options as a step of its own, all starting as the game has them.
public sealed partial class MainWindow
{
    const int GameSettingsScreen = 16, WizardSettings = 17;
    const string GameSettingsPlan = "gamesettings"; // (Elevation's --open for the administrator copy, which has no window)
    Dictionary<string, string> _gsNow = new(), _gsWant = new();
    string? _gsBuilt, _wizBuilt, _gsFor;
    bool _wizSetter;

    // the choices waiting for the game's first start, per Documents folder
    Dictionary<string, string>? PendingOptions => _settings.PendingOptions.GetValueOrDefault(Docs);

    void LoadGameSettings()
    {
        if (_game is not { } g) { _gsFor = null; return; }
        _gsFor = g.Bin;
        _gsNow = GameSettings.Now(g.Bin, Docs);
        foreach (var (k, v) in PendingOptions ?? new()) _gsNow[k] = v; // (chosen already, only not written yet)
        _gsWant = new(_gsNow);
    }

    Dictionary<string, string> GameSettingsChanges() => GameSettings.Changes(_gsWant, _gsNow);

    // built again only when what it shows changed: a refresh of the window must not close an open list
    void BuildGameSettings(bool force = false)
    {
        var key = $"{_game?.Bin}|{_running}|{_busy}|{Lang}|{App.Light}|{_state?.S3ss}";
        if (!force && key == _gsBuilt) return;
        _gsBuilt = key;
        _wizBuilt = null; // (the page and the step share the rows that follow the choices)
        _gsSync.Clear();
        var host = ScreenGameSettings;
        host.Children.Clear();
        host.MaxWidth = 920;
        var head = new Grid { ColumnSpacing = 16 };
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(Col(Heading(T("Game settings")), Muted(T("Graphics and speed options the game keeps in its files. Close the game before you apply."))));
        host.Children.Add(head);
        if (_game is not { } g) { host.Children.Add(Muted(T("Choose the game first."))); return; }
        if (_gsFor != g.Bin) LoadGameSettings();
        var resetAll = Id(Button(T("Reset all to default"), GameSettingsToDefault), "GameSettingsReset");
        resetAll.VerticalAlignment = VerticalAlignment.Center;
        resetAll.IsEnabled = !_busy;
        Grid.SetColumn(resetAll, 1);
        head.Children.Add(resetAll);

        host.Children.Add(Section(T("Graphics")));
        foreach (var r in GraphicsRows(true)) host.Children.Add(r);

        var options = new Grid();
        options.Children.Add(Section(T("In-game options")));
        var off = Link(T("Turn all off"), () => { foreach (var i in InGame) _gsWant[i.Id] = "1"; SyncGameSettings(); });
        off.HorizontalAlignment = HorizontalAlignment.Right;
        off.VerticalAlignment = VerticalAlignment.Bottom;
        options.Children.Add(off);
        host.Children.Add(options);
        foreach (var r in InGameRows(true)) host.Children.Add(r);
        if (!File.Exists(Engine.OptionsIni(Docs))) host.Children.Add(Muted(T("You haven't opened the game yet, so these four are set the first time you press Play.")));

        host.Children.Add(Section(T("Loading and walking")));
        foreach (var r in LoadingRows(true)) host.Children.Add(r);
        host.Children.Add(WalkingValues());

        host.Children.Add(Section("Sims3SettingsSetter"));
        host.Children.Add(SetterRow());

        host.Children.Add(Section(T("Checks")));
        host.Children.Add(CardRow(g));
        var user = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var plain = user.All(c => c < 128);
        host.Children.Add(Row(T("Windows user folder"), plain ? T("No special characters") : T("Has characters the game may not read: {0}", user), State(plain)));
        var cloud = PlaceChecks.InOneDrive(Docs);
        host.Children.Add(Row("OneDrive", cloud ? T("The game's Documents folder is in OneDrive") : T("Not syncing the game's folder"), State(!cloud)));
        GroupCards(host);

        var foot = new Grid { ColumnSpacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var note = Muted(_running ? T("Close The Sims 3 to continue") : T("Each file is copied before it changes."));
        note.VerticalAlignment = VerticalAlignment.Center;
        foot.Children.Add(note);
        var restore = Id(Button(T("Restore my files"), RestoreGameSettings), "GameSettingsRestore");
        restore.IsEnabled = !_busy && !_running && GameSettings.CanRestore(g.Bin, Docs);
        Grid.SetColumn(restore, 1);
        foot.Children.Add(restore);
        var apply = Id(Button(T("Apply"), ApplyGameSettings, accent: true), "GameSettingsApply");
        // (a value being typed counts once its box is left, which the click on Apply itself does)
        _gsSync.Add(() => apply.IsEnabled = !_busy && !_running && (_gsTyping || GameSettingsChanges().Count > 0));
        Grid.SetColumn(apply, 2);
        foot.Children.Add(apply);
        host.Children.Add(foot);
        SyncGameSettings();
    }

    // the controls follow the choices in place: a row built again under the pointer or the keyboard would lose it
    readonly List<Action> _gsSync = [];
    bool _gsSyncing, _gsTyping;
    void SyncGameSettings()
    {
        _gsSyncing = true;
        try { foreach (var s in _gsSync) s(); }
        finally { _gsSyncing = false; }
    }

    void GameSettingsToDefault()
    {
        foreach (var s in GameSettings.All) if (_gsNow.ContainsKey(s.Id)) _gsWant[s.Id] = s.Default;
        SyncGameSettings();
    }

    UIElement State(bool ok) => Lucide.Icon(ok ? "check" : "triangle-alert", 16, Res(ok ? "ApexSuccessBrush" : "ApexWarningBrush"));

    // ---------- the options, as rows of the page and of the install's step ----------

    static readonly (string Id, string Title, string Detail)[] InGame =
    [
        (GameSettings.Store, "Turn off Shop mode", "The in-game store."),
        (GameSettings.Interactive, "Turn off interactive loading screens", "The game you play while a save loads."),
        (GameSettings.Memories, "Turn off memories", "Sims stop recording memories. Leave them on for storytelling."),
        (GameSettings.Online, "Turn off online and social features", "Connect notifications, and the sign-in prompt before a save loads."),
    ];

    IEnumerable<Border> GraphicsRows(bool page)
    {
        yield return Choice(GameSettings.Shadow, T("Shadow resolution"), T("Sharper shadow edges. Uses more video memory, and needs the game's highest lighting quality."), GameSettings.ShadowSizes, v => v, page);
        yield return Choice(GameSettings.Sims, T("Sim texture size"), T("More detail on Sims up close. Uses more of the game's memory, and needs its highest Sim detail."), GameSettings.SimSizes, v => v, page);
        yield return Switch(GameSettings.Bloom, T("Turn off bloom"), T("Removes the glow on bright areas. It can also remove other screen effects."), page);
    }

    IEnumerable<Border> InGameRows(bool page) => InGame.Select(i => Switch(i.Id, T(i.Title), T(i.Detail), page));

    IEnumerable<Border> LoadingRows(bool page)
    {
        yield return Choice(GameSettings.Reads, T("Files read at once"), T("How much the game loads from disk at the same time. 40 MB can load lots sooner and uses a little more memory while loading."),
            GameSettings.ReadSizes, v => v[..^6] + " MB", page);
        yield return Choice("walk", T("Space between walking Sims"), T("Less space makes Sims stop less to let others pass, but they can walk through each other. Results vary."),
            ["default", "half", "small"], v => v switch { "half" => T("Half"), "small" => T("Smallest"), _ => T("Default") }, page);
    }

    // the five avoidance values as one choice; null when they are none of the presets
    static readonly string[] Presets = ["default", "half", "small"];
    static string Preset(string preset, string normal) => preset switch
    {
        "half" => ((GameSettings.Number(normal) ?? 0) / 2).ToString("0.###", CultureInfo.InvariantCulture),
        "small" => "0.1",
        _ => normal,
    };
    static IEnumerable<(string Id, string Key, string Default)> Avoidance => GameSettings.Walking.Where(w => w.Id.StartsWith("walk.", StringComparison.Ordinal));
    string? Wanted(string id) => id != "walk" ? _gsWant.GetValueOrDefault(id)
        : Presets.FirstOrDefault(p => Avoidance.All(w => GameSettings.Same(_gsWant.GetValueOrDefault(w.Id), Preset(p, w.Default))));
    bool Available(string id) => id != "walk" ? _gsNow.ContainsKey(id) : Avoidance.All(w => _gsNow.ContainsKey(w.Id));
    string DefaultOf(string id) => id == "walk" ? "default" : GameSettings.Of(id).Default;

    void Want(string id, string value)
    {
        if (_gsSyncing) return;
        if (id == "walk") foreach (var w in Avoidance) _gsWant[w.Id] = Preset(value, w.Default);
        else _gsWant[id] = value;
        SyncGameSettings();
    }

    Border Choice(string id, string title, string detail, string[] values, Func<string, string> label, bool page)
    {
        var box = new ComboBox { MinWidth = 180, IsEnabled = Available(id) && !_busy };
        foreach (var v in values) box.Items.Add(v == DefaultOf(id) && v != "default" ? T("{0} (default)", label(v)) : label(v));
        // (a value of the player's own is one more entry, and stays until another is picked)
        _gsSync.Add(() =>
        {
            var now = Wanted(id);
            var at = Array.IndexOf(values, now);
            if (at < 0 && Available(id))
            {
                var own = id == "walk" ? T("Custom") : T("{0} (yours)", now ?? "");
                if (box.Items.Count == values.Length) box.Items.Add(own);
                at = values.Length;
            }
            if (box.SelectedIndex != at) box.SelectedIndex = at;
        });
        box.SelectionChanged += (_, _) => { if (box.SelectedIndex >= 0 && box.SelectedIndex < values.Length) Want(id, values[box.SelectedIndex]); };
        AutomationProperties.SetName(box, title);
        return Row(title, Detail(id, detail), WithReset(id, title, box, page));
    }

    Border Switch(string id, string title, string detail, bool page)
    {
        var t = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 40, IsEnabled = Available(id) && !_busy };
        _gsSync.Add(() => { if (t.IsOn != (Wanted(id) == "1")) t.IsOn = !t.IsOn; });
        t.Toggled += (_, _) => Want(id, t.IsOn ? "1" : "0");
        AutomationProperties.SetName(t, title);
        return Row(title, Detail(id, detail), WithReset(id, title, t, page));
    }

    string Detail(string id, string detail) => Available(id) ? detail : T("Not found in this game's files.");

    FrameworkElement WithReset(string id, string title, FrameworkElement control, bool page)
    {
        if (!page || !Available(id)) return control;
        var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        var reset = Link(T("Reset"), () => Want(id, DefaultOf(id)));
        reset.VerticalAlignment = VerticalAlignment.Center;
        _gsSync.Add(() => reset.Visibility = GameSettings.Same(Wanted(id), DefaultOf(id)) ? Visibility.Collapsed : Visibility.Visible);
        AutomationProperties.SetName(reset, T("Reset {0}", title));
        p.Children.Add(reset);
        p.Children.Add(control);
        return p;
    }

    // every value of Sims3.ini's [Config], by its own name there
    Expander WalkingValues()
    {
        var list = new StackPanel { Spacing = 8 };
        foreach (var (id, key, normal) in GameSettings.Walking)
        {
            var g = new Grid { ColumnSpacing = 16 };
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(Col(new TextBlock { Text = key, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis }, Muted(T("Default: {0}", normal))));
            var box = new NumberBox
            {
                Minimum = 0, Maximum = 100, SmallChange = 0.05, LargeChange = 0.5, Width = 130, VerticalAlignment = VerticalAlignment.Center, IsEnabled = Available(id) && !_busy,
                SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact,
            };
            _gsSync.Add(() => { if (GameSettings.Number(Wanted(id)) is { } v && box.Value != v) box.Value = v; });
            box.GotFocus += (_, _) => { _gsTyping = true; SyncGameSettings(); };
            box.LostFocus += (_, _) => { _gsTyping = false; DispatcherQueue.TryEnqueue(SyncGameSettings); };
            AutomationProperties.SetName(box, key);
            // (an emptied box keeps the value there)
            box.ValueChanged += (_, e) => { if (!double.IsNaN(e.NewValue)) Want(id, e.NewValue.ToString("0.####", CultureInfo.InvariantCulture)); };
            Grid.SetColumn(box, 1);
            g.Children.Add(box);
            list.Children.Add(g);
        }
        var x = new Expander { Header = T("All values in Sims3.ini"), Content = list, CornerRadius = new CornerRadius(14), IsExpanded = _walkOpen, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
        x.Expanding += (_, _) => _walkOpen = true;
        x.Collapsed += (_, _) => _walkOpen = false;
        return x;
    }
    bool _walkOpen;

    // ---------- Sims3SettingsSetter, the graphics card ----------

    Border SetterRow()
    {
        var title = T("Settings recommended with Apex Radiance");
        if (_state?.S3ss != true)
            return Row(title, T("Sims3SettingsSetter isn't installed. You can apply them after you add it."), Button(T("Open Components"), () => Show(ComponentsStep)));
        var text = SetterSettings.Read(Docs);
        var (on, off) = SetterSettings.Differences(text);
        if (on.Count + off.Count == 0) return Row(title, T("Already in use."), State(true));
        var b = Button(T("Review…"), SuggestSetter);
        b.IsEnabled = !_busy && !_running;
        return Row(title, T("{0} to turn on, {1} to turn off.", on.Count, off.Count), b);
    }

    async Task SuggestSetter()
    {
        var docs = Docs;
        var text = SetterSettings.Read(docs);
        var (on, off) = SetterSettings.Differences(text);
        var body = new StackPanel { Spacing = 12, Width = 440 };
        if (on.Count > 0) body.Children.Add(Body(T("Turns on: {0}", string.Join(", ", on))));
        if (off.Count > 0) body.Children.Add(Body(T("Turns off: {0}", string.Join(", ", off))));
        var hz = ScreenHz();
        var smooth = new CheckBox { Content = new TextBlock { Text = T("Also turn on Smooth Patch: 480 ticks, {0} FPS to match your screen. A slower processor may prefer it off.", hz), TextWrapping = TextWrapping.Wrap }, IsChecked = false };
        if (!SetterSettings.SmoothOn(text)) body.Children.Add(smooth);
        body.Children.Add(Muted(T("Everything else in its settings stays. Restore my files puts them back.")));
        if (await ShowDialog(new ContentDialog { Title = T("Settings recommended with Apex Radiance"), Content = body, PrimaryButtonText = T("Apply"), CloseButtonText = T("Cancel"),
            DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot }) != ContentDialogResult.Primary) return;
        await GameSettingsWork(() => { SetterSettings.Apply(docs, smooth.IsChecked == true ? hz : null); return Task.FromResult(true); });
    }

    // the main screen's refresh rate (59, 119 and the like are 59.94, 119.88: the whole rate); 60 when Windows does not say
    static int ScreenHz()
    {
        var m = new byte[220]; // DEVMODEW
        BitConverter.GetBytes((short)m.Length).CopyTo(m, 68); // dmSize
        return EnumDisplaySettingsW(null, -1, m) && BitConverter.ToInt32(m, 184) is > 1 and var hz ? hz is 59 or 119 or 143 or 164 or 239 ? hz + 1 : hz : 60; // dmDisplayFrequency
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string? device, int mode, byte[] devmode);

    Border CardRow(Game g)
    {
        var card = DeviceCard.Read(Docs);
        var state = DeviceCard.State(card, g.Bin);
        var title = T("Graphics card");
        if (state == CardState.NotRecognized)
        {
            var b = Id(Button(T("Fix"), () => FixCard(g, card!)), "GameSettingsCard");
            b.IsEnabled = !_busy && !_running;
            return Row(title, card!.Found ? T("The game can't read the video memory of {0}, so it picks low settings.", card.Name) : T("The game doesn't know {0}, so it can pick low settings.", card.Name), b);
        }
        return state switch
        {
            CardState.Recognized => Row(title, T("Recognized"), State(true)),
            CardState.Added => Row(title, T("Fixed. The game reads it the next time you play."), State(true)),
            _ => Row(title, card is null ? T("Checked after you play once.") : T("The game doesn't know {0}, and its maker isn't in the game's list.", card.Name), card is null ? null : State(false)),
        };
    }

    Task FixCard(Game g, DeviceCard card) => GameSettingsWork(() => InGameFolder(g, () => card.Fix(g), new()
    {
        ["card"] = card.Name, ["card.vendor"] = card.Vendor.ToString(), ["card.device"] = card.Device.ToString(), ["card.found"] = card.Found ? "1" : "0",
        ["card.mem"] = card.MemoryMb.ToString(), ["card.guessed"] = card.MemoryGuessed ? "1" : "0",
    }));

    // ---------- Apply and Restore ----------

    Task ApplyGameSettings()
    {
        if (_game is not { } g) return Task.CompletedTask;
        var (docs, want) = (Docs, GameSettingsChanges());
        return GameSettingsWork(async () =>
        {
            if (want.Keys.Any(GameSettings.InBin) && !await InGameFolder(g, () => GameSettings.ApplyBin(g, docs, want), want)) return false;
            await Task.Run(() => SaveOptions(g, docs, want));
            return true;
        });
    }

    // Options.ini now, or kept for the first Play
    void SaveOptions(Game g, string docs, IReadOnlyDictionary<string, string> want)
    {
        var mine = want.Where(w => !GameSettings.InBin(w.Key)).ToDictionary(w => w.Key, w => w.Value);
        if (mine.Count == 0) return;
        var all = new Dictionary<string, string>(_settings.PendingOptions.GetValueOrDefault(docs) ?? new());
        foreach (var (k, v) in mine) all[k] = v;
        var written = GameSettings.ApplyOptions(g, docs, all);
        foreach (var k in all.Where(a => a.Value == GameSettings.Of(a.Key).Default).Select(a => a.Key).ToList()) all.Remove(k);
        if (written || all.Count == 0) _settings.PendingOptions.Remove(docs);
        else _settings.PendingOptions[docs] = all;
        _settings.Save();
    }

    async Task RestoreGameSettings()
    {
        if (_game is not { } g) return;
        if (!await Confirm(T("Restore my files"), T("The game's files go back to how they were before your first change here."), T("Restore"))) return;
        var docs = Docs;
        await GameSettingsWork(async () =>
        {
            if (GameSettings.BinHasCopies(g.Bin) && !await InGameFolder(g, () => GameSettings.RestoreBin(g), new() { ["restore"] = "1" })) return false;
            await Task.Run(() => GameSettings.RestoreDocs(docs));
            if (_settings.PendingOptions.Remove(docs)) _settings.Save();
            return true;
        });
    }

    async Task GameSettingsWork(Func<Task<bool>> work)
    {
        if (_busy || _running) return;
        _busy = true;
        BuildGameSettings(true);
        string? error = null;
        var changed = false;
        try { changed = await work(); }
        catch (Exception e) { App.Log(e); error = ErrorText(e); }
        finally { _busy = false; }
        if (changed || error is not null) LoadGameSettings(); // (a refused prompt keeps the choices)
        RefreshAll();
        BuildGameSettings(true);
        if (error is not null) await Message(T("The launcher could not finish"), error);
    }

    // Game\Bin's part: here when this copy may write there, else in a copy run as administrator, without a window
    async Task<bool> InGameFolder(Game g, Action here, Dictionary<string, string> plan)
    {
        if (Elevation.IsElevated || DryRun || Elevation.CanWrite(g.Bin)) { await Task.Run(here); return true; }
        if (Environment.ProcessPath is not { } exe || !await Confirm(T("Administrator permission needed"), T("Windows asks for permission, then your changes apply."), T("Continue"))) return false;
        var p = Elevation.StartMaintenance(exe, g.Bin, GameSettingsPlan, Elevation.SaveChoices(plan));
        if (p is null) { await Message(T("Nothing was changed"), T("Windows did not allow the change.")); return false; }
        using (p)
        {
            await p.WaitForExitAsync();
            if (p.ExitCode != 0) throw new SetupException("The game's files could not be changed. See the log for details.");
        }
        return true;
    }

    // the administrator copy: the plan's values pass the same checks as the page's
    public static int? GameSettingsElevated(string? bin, string? screen, IReadOnlyList<string> args)
    {
        if (screen != GameSettingsPlan || bin is null || !Elevation.IsElevated) return null;
        try
        {
            if (Games.FromFolder(bin) is not { } g) return 1;
            var plan = Elevation.Choices(args);
            int N(string k) => int.TryParse(plan.GetValueOrDefault(k), out var n) ? n : 0;
            if (plan.ContainsKey("restore")) GameSettings.RestoreBin(g);
            else if (plan.TryGetValue("card", out var name))
                new DeviceCard(name, N("card.vendor"), N("card.device"), N("card.found") == 1, N("card.mem"), N("card.guessed") == 1).Fix(g);
            else GameSettings.ApplyBin(g, Scan.DocumentsFor(g), plan);
            return 0;
        }
        catch (Exception e) { App.Log(e); return 1; }
    }

    // ---------- Play ----------

    // before the game starts: the choices go in, making Options.ini when the game never ran. After that first session the game
    // has written its own file: they go in again if it dropped them, and wait no more.
    void PlayPendingOptions(Game g, bool ended)
    {
        var docs = Docs;
        if (PendingOptions is not { } want) return;
        try
        {
            if (File.Exists(Engine.OptionsIni(docs))) { GameSettings.ApplyOptions(g, docs, want); _settings.PendingOptions.Remove(docs); _settings.Save(); }
            else if (!ended) GameSettings.CreateOptions(docs, want);
        }
        catch (Exception e) { App.Log(e); } // (never in the way of the game: tried again at the next Play)
        _gsFor = null;
    }

    // ---------- the install's step ----------

    void BuildWizardSettings()
    {
        var key = $"{_game?.Bin}|{_busy}|{Lang}|{App.Light}|{S3ssToggle.IsOn}";
        if (key == _wizBuilt && _gsFor == _game?.Bin) return;
        _wizBuilt = key;
        var host = ScreenWizardSettings;
        host.Children.Clear();
        _gsSync.Clear();
        if (_game is not { } g) return;
        if (_gsFor != g.Bin) { LoadGameSettings(); _wizSetter = !File.Exists(SetterSettings.FileOf(Docs)); }
        var head = new Grid { ColumnSpacing = 16 };
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(Col(Heading(T("Game settings")), Muted(T("Optional. You can change these later in the launcher."))));
        var reset = Button(T("Reset to default"), GameSettingsToDefault);
        reset.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(reset, 1);
        head.Children.Add(reset);
        host.Children.Add(head);
        host.Children.Add(Section(T("Graphics")));
        foreach (var r in GraphicsRows(false)) host.Children.Add(r);
        host.Children.Add(Section(T("In-game options")));
        foreach (var r in InGameRows(false)) host.Children.Add(r);
        if (!File.Exists(Engine.OptionsIni(Docs))) host.Children.Add(Muted(T("You haven't opened the game yet, so these four are set the first time you press Play.")));
        host.Children.Add(Section(T("Loading and walking")));
        foreach (var r in LoadingRows(false)) host.Children.Add(r);
        // (only with the mod chosen in Components, and something to change)
        if (S3ssToggle.IsOn && SetterSettings.Differences(SetterSettings.Read(Docs)) is var d && d.TurnOn.Count + d.TurnOff.Count > 0)
        {
            host.Children.Add(Section("Sims3SettingsSetter"));
            host.Children.Add(Toggle(T("Settings recommended with Apex Radiance"), T("{0} to turn on, {1} to turn off.", d.TurnOn.Count, d.TurnOff.Count), _wizSetter, on => _wizSetter = on));
        }
        GroupCards(host);
        _gsBuilt = null;
        SyncGameSettings();
    }

    // after the install's files: the step's choices. bin: the copy that may write the game folder; docs: the player's own copy.
    async Task ApplyWizardSettings(Game g, bool bin, bool docs)
    {
        if (_gsFor != g.Bin) return;
        var (folder, want, setter) = (Docs, GameSettingsChanges(), _wizSetter && S3ssToggle.IsOn);
        try
        {
            await Task.Run(() =>
            {
                if (bin && want.Keys.Any(GameSettings.InBin)) GameSettings.ApplyBin(g, folder, want);
                if (!docs) return;
                SaveOptions(g, folder, want);
                if (setter) SetterSettings.Apply(folder, null);
            });
        }
        catch (Exception e) { App.Log(e); await Message(T("Apex Radiance is installed, but the game settings were not changed"), ErrorText(e)); }
        _gsFor = null;
    }
}
