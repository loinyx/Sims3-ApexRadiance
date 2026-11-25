using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
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

    // the files again, unless a choice waits to be applied: the game, a restore point or another tool may have changed them
    void FreshGameSettings()
    {
        if (_gsFor is not null && GameSettingsChanges().Count > 0) return;
        _gsFor = null;
        _gsBuilt = null;
    }

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
        var resetAll = Id(Button(T("Reset all to default"), GameSettingsToDefault), "GameSettingsReset");
        host.Children.Add(PageHead(T("Game settings"), T("Graphics and speed options the game keeps in its files. Close the game before you apply."), _game is null ? null : resetAll));
        if (_game is not { } g) { host.Children.Add(Muted(T("Choose the game first."))); return; }
        if (_gsFor != g.Bin) LoadGameSettings();
        resetAll.IsEnabled = !_busy;
        var list = new SettingList(host, false);
        void Label(string text, FrameworkElement? end = null) => host.Children.Add(Section(text, end));

        Label(T("Graphics"));
        GraphicsRows(list);
        Label(T("In-game options"), Link(T("Turn all off"), () => { foreach (var i in InGame) _gsWant[i.Id] = "1"; SyncGameSettings(); }));
        InGameRows(list);
        if (!File.Exists(Engine.OptionsIni(Docs))) host.Children.Add(Muted(T("You haven't opened the game yet. These four apply after you play once.")));
        Label(T("Loading and walking"));
        LoadingRows(list);
        WalkingValues(host);
        Label("Sims3SettingsSetter");
        SetterRow(list);
        Label(T("Checks"));
        CardRow(list, g);
        var user = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var plain = user.All(c => c < 128);
        SettingRow(list, "users", T("Windows user folder"), plain ? T("No special characters") : T("Has characters the game may not read: {0}", user), null, State(plain));
        var cloud = PlaceChecks.InOneDrive(Docs);
        SettingRow(list, "folder", "OneDrive", cloud ? T("Syncs the game's folder in Documents, which can damage saves") : T("Does not sync the game's folder"), null, State(!cloud));
        Label(T("Your files"));
        var restore = Id(Button(T("Restore"), RestoreGameSettings), "GameSettingsRestore");
        restore.IsEnabled = !_busy && !_running && GameSettings.CanRestore(g.Bin, Docs);
        SettingRow(list, "undo-2", T("Restore my files"), T("The game's files as they were before your first change here"), null, restore);
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
        if (_step == GameSettingsScreen) UpdateFooter();
    }

    void GameSettingsToDefault()
    {
        foreach (var s in GameSettings.All) if (_gsNow.ContainsKey(s.Id)) _gsWant[s.Id] = s.Default;
        SyncGameSettings();
    }

    // the rows that differ from the files (the walking values count as the one choice they make together)
    int GameSettingsCount() => GameSettingsChanges().Keys.Select(k => k.StartsWith("walk.", StringComparison.Ordinal) ? "walk" : k).Distinct().Count();

    FrameworkElement State(bool ok) => Lucide.Icon(ok ? "check" : "triangle-alert", 16, Res(ok ? "ApexSuccessBrush" : "ApexWarningBrush"));

    // ---------- a row, as Components draws its own: icon, name and one line, a state column, the control on the right.
    // On the page a card per row; in the install one list per group. ----------

    sealed class SettingList(Panel host, bool wizard)
    {
        public Panel Host => host;
        public bool Wizard => wizard;
        public StackPanel? Group;
    }

    void SettingRow(SettingList list, string? icon, string name, string text, FrameworkElement? state, FrameworkElement? end, Func<bool>? changed = null)
    {
        var g = new Grid { ColumnSpacing = 16, VerticalAlignment = VerticalAlignment.Center };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(icon is null ? new Border { Width = 36 } : RowIcon(icon));
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = name, FontSize = 14, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        var line = new TextBlock { Text = text, FontSize = 13, LineHeight = 18, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 3 };
        ToolTipService.SetToolTip(line, text);
        words.Children.Add(line);
        Grid.SetColumn(words, 1);
        g.Children.Add(words);
        if (!list.Wizard)
        {
            var cell = new Grid { ColumnSpacing = 8, VerticalAlignment = VerticalAlignment.Center };
            cell.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(6) });
            cell.ColumnDefinitions.Add(new ColumnDefinition());
            if (changed is not null)
            {
                var dot = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 6, Height = 6, Fill = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center };
                _gsSync.Add(() => dot.Visibility = changed() ? Visibility.Visible : Visibility.Collapsed);
                cell.Children.Add(dot);
            }
            if (state is not null) { state.VerticalAlignment = VerticalAlignment.Center; Grid.SetColumn(state, 1); cell.Children.Add(state); }
            Grid.SetColumn(cell, 2);
            g.Children.Add(cell);
        }
        if (end is not null)
        {
            end.HorizontalAlignment = HorizontalAlignment.Right;
            end.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(end, 3);
            g.Children.Add(end);
        }
        if (!list.Wizard)
        {
            var card = new Border { Child = g, Padding = new Thickness(20, 12, 20, 12), MinHeight = 68 };
            Soft(card);
            list.Host.Children.Add(Join(card));
            return;
        }
        if (list.Group is null)
            list.Host.Children.Add(new Border { Child = list.Group = new StackPanel(), CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexRaisedBrush") });
        list.Group.Children.Add(new Border { Child = g, Padding = new Thickness(Design.Space.RowPad, 12, Design.Space.RowPad, 12), MinHeight = Design.Size.Row, BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, list.Group.Children.Count > 0 ? 1 : 0, 0, 0) });
    }

    // ---------- the options ----------

    static readonly (string Id, string Icon, string Title, string Detail)[] InGame =
    [
        (GameSettings.Store, "shopping-bag", "Turn off Shop mode", "Shop mode is the store inside the game"),
        (GameSettings.Interactive, "gamepad-2", "Turn off interactive loading screens", "The small game you play while a save loads"),
        (GameSettings.Memories, "book-open", "Turn off memories", "Sims stop recording memories"),
        (GameSettings.Online, "wifi", "Turn off online and social features", "Stops Connect notifications and the sign-in prompt"),
    ];

    void GraphicsRows(SettingList l)
    {
        Choice(l, "sun", GameSettings.Shadow, T("Shadow resolution"), T("Sharper shadow edges. Needs the game's lighting quality at its highest."), GameSettings.ShadowSizes, v => v);
        Choice(l, "person-standing", GameSettings.Sims, T("Sim texture size"), T("More detail up close. Needs the game's Sim detail at its highest."), GameSettings.SimSizes, v => v);
        Switch(l, "sparkles", GameSettings.Bloom, T("Turn off bloom"), T("Can also remove other screen effects"));
    }

    void InGameRows(SettingList l) { foreach (var i in InGame) Switch(l, i.Icon, i.Id, T(i.Title), T(i.Detail)); }

    void LoadingRows(SettingList l)
    {
        Choice(l, "hard-drive-download", GameSettings.Reads, T("Files read at once"), T("With 40 MB lots may load sooner, but this was not measured"), GameSettings.ReadSizes, v => v[..^6] + " MB");
        Choice(l, "footprints", "walk", T("Space between walking Sims"), T("With less space, Sims can pass closer to each other. Results vary."), Presets,
            v => v switch { "half" => T("Half"), "small" => T("Smallest"), _ => T("Default") });
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
    bool Changed(string id) => id != "walk" ? !GameSettings.Same(_gsWant.GetValueOrDefault(id), _gsNow.GetValueOrDefault(id)) : Avoidance.Any(w => Changed(w.Id));

    void Want(string id, string value)
    {
        if (_gsSyncing) return;
        if (id == "walk") foreach (var w in Avoidance) _gsWant[w.Id] = Preset(value, w.Default);
        else _gsWant[id] = value;
        SyncGameSettings();
    }

    void Choice(SettingList l, string icon, string id, string title, string detail, string[] values, Func<string, string> label)
    {
        var box = new ComboBox { Width = 200, IsEnabled = Available(id) && !_busy };
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
        SettingRow(l, icon, title, Detail(id, detail), ResetOf(id, title), box, () => Changed(id));
    }

    void Switch(SettingList l, string icon, string id, string title, string detail)
    {
        var t = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 40, IsEnabled = Available(id) && !_busy };
        _gsSync.Add(() => { if (t.IsOn != (Wanted(id) == "1")) t.IsOn = !t.IsOn; });
        t.Toggled += (_, _) => Want(id, t.IsOn ? "1" : "0");
        AutomationProperties.SetName(t, title);
        SettingRow(l, icon, title, Detail(id, detail), ResetOf(id, title), t, () => Changed(id));
    }

    string Detail(string id, string detail) => Available(id) ? detail : T("Not found in this game's files");

    FrameworkElement? ResetOf(string id, string title)
    {
        if (!Available(id)) return null;
        var reset = new Button { Content = Lucide.Icon("undo-2", 15, Res("ApexMutedBrush")), Width = 36, Height = 36, Padding = new Thickness(0), BorderThickness = new Thickness(0), Background = Res("ApexClearBrush") };
        reset.Click += (_, _) => Want(id, DefaultOf(id));
        ToolTipService.SetToolTip(reset, T("Reset"));
        _gsSync.Add(() => reset.Visibility = GameSettings.Same(Wanted(id), DefaultOf(id)) ? Visibility.Collapsed : Visibility.Visible);
        AutomationProperties.SetName(reset, T("Reset {0}", title));
        return reset;
    }

    // every value of Sims3.ini's [Config], by its own name there, under a row that opens them
    void WalkingValues(Panel host)
    {
        var rows = new StackPanel { Spacing = 4 };
        var open = new Button { Width = 100 };
        void Open(bool on) { _walkOpen = on; rows.Visibility = on ? Visibility.Visible : Visibility.Collapsed; open.Content = on ? T("Hide") : T("Show"); }
        open.Click += (_, _) => Open(!_walkOpen);
        SettingRow(new SettingList(host, false), "sliders-horizontal", T("All walking values in Sims3.ini"), T("The values behind this option, plus two for strollers"), null, open);
        host.Children.Add(rows);
        Open(_walkOpen);
        var list = new SettingList(rows, false);
        foreach (var (id, key, normal) in GameSettings.Walking)
        {
            // (a plain box: NumberBox reads and shows numbers in Windows' language, where 0.15 can mean 15; the files use a dot)
            var box = new TextBox { Width = 200, MaxLength = 8, IsEnabled = Available(id) && !_busy };
            var editing = false;
            void Take()
            {
                // (what is no number from 0 to 100 goes back to the value there)
                if (GameSettings.Typed(box.Text) is { } v && !GameSettings.Same(v, Wanted(id))) Want(id, v);
                box.Text = Wanted(id) ?? "";
            }
            _gsSync.Add(() => { if (!editing) box.Text = Wanted(id) ?? ""; });
            box.GotFocus += (_, _) => { editing = _gsTyping = true; SyncGameSettings(); };
            box.LostFocus += (_, _) => { editing = _gsTyping = false; Take(); DispatcherQueue.TryEnqueue(SyncGameSettings); };
            box.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) { Take(); box.SelectAll(); } };
            AutomationProperties.SetName(box, key);
            SettingRow(list, null, key, Detail(id, T("Default: {0}", normal)), ResetOf(id, key), box, () => Changed(id));
        }
    }
    bool _walkOpen;

    // ---------- Sims3SettingsSetter, the graphics card ----------

    void SetterRow(SettingList l)
    {
        var title = T("Recommended settings");
        if (_state?.S3ss != true) { SettingRow(l, "sliders-horizontal", title, T("Not installed yet. Add it in Components to apply them."), null, Button(T("Open Components"), () => Show(ComponentsStep))); return; }
        var text = SetterSettings.Read(Docs);
        var (on, off) = SetterSettings.Differences(text);
        if (on.Count + off.Count == 0) { SettingRow(l, "sliders-horizontal", title, T("Already in use"), null, State(true)); return; }
        var b = Button(T("Review"), SuggestSetter);
        b.IsEnabled = !_busy && !_running;
        SettingRow(l, "sliders-horizontal", title, T("For Apex Radiance: {0} to turn on, {1} to turn off", on.Count, off.Count), null, b);
    }

    async Task SuggestSetter()
    {
        var docs = Docs;
        var text = SetterSettings.Read(docs);
        var (on, off) = SetterSettings.Differences(text);
        var body = new StackPanel { Spacing = 16, Width = Design.Dialogs.Small };
        if (on.Count > 0) body.Children.Add(Body(T("Turns on: {0}", string.Join(", ", on))));
        if (off.Count > 0) body.Children.Add(Body(T("Turns off: {0}", string.Join(", ", off))));
        var hz = ScreenHz();
        var smooth = new CheckBox { Content = new TextBlock { Text = T("Also turn on Smooth Patch, at 480 ticks and {0} fps to match your screen. Leave it off on a slower processor.", hz), TextWrapping = TextWrapping.Wrap }, IsChecked = false };
        if (!SetterSettings.SmoothOn(text)) body.Children.Add(smooth);
        body.Children.Add(Muted(T("Its other settings stay as they are. To undo this, use Restore my files.")));
        if (await ShowDialog(new ContentDialog { Tag = "sliders-horizontal", Title = T("Recommended settings"), Content = body, PrimaryButtonText = T("Apply"), CloseButtonText = T("Cancel"),
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

    void CardRow(SettingList l, Game g)
    {
        var card = DeviceCard.Read(Docs);
        var state = DeviceCard.State(card, g.Bin);
        FrameworkElement? end = card is null ? null : State(state != CardState.Unknown);
        if (state == CardState.NotRecognized)
        {
            var b = Id(Button(T("Fix"), () => FixCard(g, card!)), "GameSettingsCard");
            b.IsEnabled = !_busy && !_running;
            end = b;
        }
        SettingRow(l, "cpu", T("Graphics card"), state switch
        {
            CardState.Recognized => T("Recognised"),
            CardState.Added => T("Fixed. The game reads it the next time you play."),
            CardState.NotRecognized => card!.Found ? T("The game can't read how much video memory {0} has, so it picks low settings", card.Name) : T("The game doesn't know {0}, so it can pick low settings", card.Name),
            _ => card is null ? T("Checked after you play once") : T("The game doesn't know {0}, and its maker isn't in the game's list", card.Name),
        }, null, end);
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

    // Options.ini now, or kept until the game has made it
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
        if (!await Confirm(T("Restore your files?"), T("The game's files go back to how they were before your first change here. Every change you made here since then is undone."), T("Restore"))) return;
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
        if (Environment.ProcessPath is not { } exe || !await Confirm(T("Administrator permission needed"), T("Windows will ask for permission. Then your changes are applied."), T("Continue"))) return false;
        var p = Elevation.StartMaintenance(exe, g.Bin, GameSettingsPlan, Elevation.SaveChoices(plan));
        if (p is null) { await Message(T("Nothing was changed"), T("Windows did not get your permission. Try again and choose Yes when it asks.")); return false; }
        using (p)
        {
            await p.WaitForExitAsync();
            if (p.ExitCode != 0) throw new SetupException("The game's files could not be changed. Close the game and try again. The log has the details.");
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

    // the choices made before the game ever ran go in once it has written its own Options.ini: before a start, and after one
    // (never a file of the launcher's making, which the game could take for its own and skip setting itself up)
    void PlayPendingOptions(Game g)
    {
        var docs = Docs;
        if (PendingOptions is not { } want || !File.Exists(Engine.OptionsIni(docs))) return;
        try { GameSettings.ApplyOptions(g, docs, want); _settings.PendingOptions.Remove(docs); _settings.Save(); }
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
        SettingList Label(string text)
        {
            host.Children.Add(new TextBlock { Text = text, FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexSoftTextBrush"), Margin = new Thickness(0, 20, 0, 8) });
            return new SettingList(host, true);
        }
        GraphicsRows(Label(T("Graphics")));
        InGameRows(Label(T("In-game options")));
        if (!File.Exists(Engine.OptionsIni(Docs))) host.Children.Add(Muted(T("You haven't opened the game yet. These four apply after you play once.")));
        LoadingRows(Label(T("Loading and walking")));
        // (only with the mod chosen in Components, and something to change)
        if (S3ssToggle.IsOn && SetterSettings.Differences(SetterSettings.Read(Docs)) is var d && d.TurnOn.Count + d.TurnOff.Count > 0)
        {
            var t = new ToggleSwitch { OnContent = "", OffContent = "", MinWidth = 0, Width = 40, IsOn = _wizSetter };
            t.Toggled += (_, _) => _wizSetter = t.IsOn;
            AutomationProperties.SetName(t, T("Recommended settings"));
            SettingRow(Label("Sims3SettingsSetter"), "sliders-horizontal", T("Recommended settings"), T("For Apex Radiance: {0} to turn on, {1} to turn off", d.TurnOn.Count, d.TurnOff.Count), null, t);
        }
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
