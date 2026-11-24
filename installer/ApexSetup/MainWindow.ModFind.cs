using System.Text;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;
using static ApexSetup.Strings;

namespace ApexSetup;

// Two of the Mods screen's tools: Find custom content (which mods a list or a game file uses, in place of the mods list)
// and choosing which of the game's caches to clear.
public sealed partial class MainWindow
{
    // ---------- find custom content ----------

    Grid? _findPage;
    FindResult? _findResult;
    CancellationTokenSource? _findCts;
    string _findTab = "found", _findPlace = "";
    (bool Light, string Lang) _findLook;
    // (made with the page)
    Border _findHead = null!, _findList = null!;
    Grid _findBar = null!;
    SelectorBar _findTabs = null!;
    TextBox _findSearch = null!;
    StackPanel _findRows = null!;
    ProgressBar? _findProgress;
    int _findTyped;
    const int MaxFindRows = 200;

    // what the picker offers: lists, then the game's own files
    static readonly string[] FindFiles = [".xml", ".txt", ".csv", ".html", ".json", ".log", ".package", ".sim", ".nhd", ".world", "*"];

    async Task FindContentAsync()
    {
        if (_game is null) return;
        if (_step != ModsScreen) Show(ModsScreen);
        if (!_modsLoaded) await LoadModsAsync();
        if (!_modsLoaded) return; // (the Mods screen says why)
        _findPlace = _modPlace;
        ModList.SelectedItems.Clear();
        ModGrid.SelectedItems.Clear();
        BuildFind();
        Animate(_findPage!);
    }

    // from FillMods: the page follows the mods (one turned on is found), the theme and the language; another place closes it
    void FindFollows()
    {
        if (_findPage is null) return;
        if (_modPlace != _findPlace) { CloseFind(); return; }
        ModDetailsPane.Visibility = Visibility.Collapsed;
        if (_findLook != (_light, Lang)) BuildFind(); else FillFind();
    }

    // focus: back to the list by keyboard (a place chosen in the side pane keeps it there)
    void CloseFind(bool focus = false)
    {
        if (_findPage is null) return;
        StopFind();
        TakeInfo((Panel)ModHuntBar.Parent);
        ScreenMods.Children.Remove(_findPage);
        _findPage = null;
        ((FrameworkElement)ModList.Parent).Visibility = Visibility.Visible;
        FillModDetails();
        if (focus) (_modsGrid ? (Control)ModGrid : ModList).Focus(FocusState.Programmatic);
    }

    void StopFind() { _findCts?.Cancel(); _findCts = null; }

    // (the Mods bar with it: what Turn on says shows here too)
    void TakeInfo(Panel to)
    {
        ((Panel)ModsInfo.Parent).Children.Remove(ModsInfo);
        to.Children.Add(ModsInfo);
    }

    // (a click on the place already chosen raises no selection change: it goes back to the list as well)
    bool _findWatchesPlaces;
    void WatchPlaces()
    {
        if (_findWatchesPlaces) return;
        _findWatchesPlaces = true;
        ModFolders.Tapped += (_, e) =>
        {
            for (var p = e.OriginalSource as DependencyObject; p is not null && _findPage is not null; p = VisualTreeHelper.GetParent(p))
                if (p is ListViewItem item) { if (item.Content is FrameworkElement { Tag: string key } && key == _modPlace) CloseFind(); return; }
        };
    }

    void BuildFind()
    {
        WatchPlaces();
        var query = _findPage is null ? "" : _findSearch.Text;
        // (Tag="dyn": its texts are set here, not translated in place)
        var page = new Grid { Tag = "dyn", Padding = new Thickness(24, 20, 24, 0), RowSpacing = 14, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), AllowDrop = true };
        for (var i = 0; i < 4; i++) page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition());
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(page, T("Find custom content"));
        void Put(FrameworkElement e, int row) { Grid.SetRow(e, row); page.Children.Add(e); }

        var top = new StackPanel { Spacing = 2 };
        var back = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        back.Children.Add(Lucide.Icon("chevron-left", 15));
        back.Children.Add(new TextBlock { Text = T("Mods") });
        var backButton = new Button { Content = back, Height = 32, Padding = new Thickness(8, 0, 12, 0), Margin = new Thickness(-8, 0, 0, 2), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(backButton, T("Back to your mods"));
        ToolTipService.SetToolTip(backButton, T("Back to your mods"));
        backButton.Click += (_, _) => CloseFind(focus: true);
        top.Children.Add(backButton);
        top.Children.Add(Heading(T("Find custom content")));
        top.Children.Add(Muted(T("See which mods a file uses and what is missing.")));
        Put(top, 0);

        var info = new StackPanel();
        Put(info, 1);

        _findHead = new Border();
        Put(_findHead, 2);

        // the tabs, a search and the report
        _findBar = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
        _findBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _findBar.ColumnDefinitions.Add(new ColumnDefinition());
        _findBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _findTabs = new SelectorBar { VerticalAlignment = VerticalAlignment.Center };
        foreach (var key in new[] { "found", "off", "missing" })
        {
            var tab = Tab("", key == "found");
            tab.Tag = key;
            tab.IsSelected = key == _findTab;
            _findTabs.Items.Add(tab);
        }
        _findTabs.SelectionChanged += (s, _) => { if (s.SelectedItem?.Tag is string k && k != _findTab) { _findTab = k; FillFindRows(); } };
        _findBar.Children.Add(_findTabs);
        _findSearch = new TextBox { PlaceholderText = T("Search"), Text = query, MaxWidth = 280, MinWidth = 100, HorizontalAlignment = HorizontalAlignment.Right, Width = 280, VerticalAlignment = VerticalAlignment.Center };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(_findSearch, T("Search"));
        _findSearch.TextChanged += (_, _) => { var mine = ++_findTyped; After(200, () => { if (mine == _findTyped) FillFindRows(); }); };
        Grid.SetColumn(_findSearch, 1);
        _findBar.Children.Add(_findSearch);
        var save = Button(T("Save report"), () => Safe(SaveFindReportAsync()));
        Grid.SetColumn(save, 2);
        _findBar.Children.Add(save);
        // (a narrow page: the search takes a line of its own under the tabs)
        _findBar.SizeChanged += (_, e) =>
        {
            var narrow = e.NewSize.Width < 620;
            if (narrow == (_findBar.RowDefinitions.Count > 0)) return;
            _findBar.RowDefinitions.Clear();
            if (narrow) for (var i = 0; i < 2; i++) _findBar.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Grid.SetRow(_findSearch, narrow ? 1 : 0);
            Grid.SetColumn(_findSearch, narrow ? 0 : 1);
            Grid.SetColumnSpan(_findSearch, narrow ? 3 : 1);
            _findSearch.Width = narrow ? double.NaN : 280;
            _findSearch.MaxWidth = narrow ? double.PositiveInfinity : 280;
            _findSearch.HorizontalAlignment = narrow ? HorizontalAlignment.Stretch : HorizontalAlignment.Right;
        };
        Put(_findBar, 3);

        _findRows = new StackPanel();
        _findList = new Border { Child = _findRows, CornerRadius = new CornerRadius(12), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), Margin = new Thickness(0, 0, 0, 24) };
        Put(new ScrollViewer { Content = _findList }, 4);

        page.DragOver += (_, e) =>
        {
            if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
            e.AcceptedOperation = DataPackageOperation.Copy;
            e.DragUIOverride.Caption = T("Find custom content");
            e.Handled = true;
        };
        page.Drop += (_, e) => { if (e.DataView.Contains(StandardDataFormats.StorageItems)) { e.Handled = true; Safe(Dropped(e.DataView)); } };
        async Task Dropped(DataPackageView data)
        {
            if ((await data.GetStorageItemsAsync()).Select(i => i.Path).FirstOrDefault(p => p is { Length: > 0 }) is { } path) await RunFindAsync(path);
        }
        page.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Escape) return;
            e.Handled = true;
            if (_findSearch.Text.Length > 0) _findSearch.Text = ""; else CloseFind(focus: true);
        };
        var find = new KeyboardAccelerator { Modifiers = Windows.System.VirtualKeyModifiers.Control, Key = Windows.System.VirtualKey.F };
        find.Invoked += (_, e) => { e.Handled = true; _findSearch.Focus(FocusState.Keyboard); };
        page.KeyboardAccelerators.Add(find);
        page.KeyboardAcceleratorPlacementMode = KeyboardAcceleratorPlacementMode.Hidden;

        TakeInfo(info);
        if (_findPage is not null) ScreenMods.Children.Remove(_findPage);
        _findPage = page;
        _findLook = (_light, Lang);
        Grid.SetColumn(page, 1);
        Grid.SetColumnSpan(page, 2);
        ((FrameworkElement)ModList.Parent).Visibility = Visibility.Collapsed;
        ModDetailsPane.Visibility = Visibility.Collapsed;
        ScreenMods.Children.Add(page);
        FillFindHead();
        FillFind();
    }

    // the file: what was read, or the way to choose one; while it is read, a bar and Cancel
    void FillFindHead()
    {
        var g = new Grid { ColumnSpacing = 16, Padding = new Thickness(18, 16, 18, 16) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var icon = Lucide.Icon("scan-search", 20, Res("ApexAccentBrush"), 1.8);
        icon.HorizontalAlignment = HorizontalAlignment.Center;
        icon.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(new Border { Width = 40, Height = 40, CornerRadius = new CornerRadius(9), Background = Res("ApexAccentSoftBrush"), Child = icon, VerticalAlignment = VerticalAlignment.Center });
        var busy = _findCts is not null;
        var r = _findResult;
        var text = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        var title = new TextBlock { Text = busy ? T("Looking through your mods…") : r?.Source.Name ?? T("Choose a list or a game file"), FontSize = 14, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        if (!busy && r is not null) ToolTipService.SetToolTip(title, r.Source.Name);
        text.Children.Add(title);
        _findProgress = null;
        if (busy) text.Children.Add(_findProgress = new ProgressBar { Maximum = 1, Margin = new Thickness(0, 6, 0, 0) });
        else
        {
            var n = r?.Source.Items.Count ?? 0;
            var line = Muted(r is null ? T("A list of custom content, a package, a Sim, a lot or a save. You can also drop it here.")
                : n == 0 ? T("This file names no custom content. Drop another file here to change it.")
                : (n == 1 ? T("1 item read. Drop another file here to change it.") : T("{0} items read. Drop another file here to change it.", n))
                  + (r.Source.Truncated ? " " + T("The file names more than the launcher reads.") : ""));
            // (a screen reader says what was read without moving to it)
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetLiveSetting(line, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
            text.Children.Add(line);
        }
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        var act = busy ? Button(T("Cancel"), () => { StopFind(); FillFindHead(); }) : Button(T("Choose file"), () => Safe(PickFindFileAsync()), accent: r is null);
        act.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(act, 2);
        g.Children.Add(act);
        var box = new Grid();
        box.Children.Add(new Microsoft.UI.Xaml.Shapes.Rectangle { RadiusX = 12, RadiusY = 12, Fill = Res("ApexCardBrush"), Stroke = Res("ApexInputBorderBrush"), StrokeThickness = 1, StrokeDashArray = [4, 3] });
        box.Children.Add(g);
        _findHead.Child = box;
    }

    async Task PickFindFileAsync()
    {
        var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
        foreach (var x in FindFiles) picker.FileTypeFilter.Add(x);
        if (await picker.PickSingleFileAsync() is { } file) await RunFindAsync(file.Path);
    }

    async Task RunFindAsync(string path)
    {
        if (_game is not { } game || _findPage is null) return;
        StopFind();
        var cts = _findCts = new CancellationTokenSource();
        var (docs, mods, gi) = (Docs, _mods.ToList(), _gameIndex);
        FillFindHead();
        var progress = new Progress<double>(v => { if (_findCts == cts && _findProgress is { } bar) bar.Value = v; });
        try
        {
            var result = await Task.Run(() =>
            {
                var source = ModFind.Read(path, cts.Token);
                // (without the game's own list the search still works: what is missing may then come with the game)
                try { gi ??= DefaultReplacements.Index(game, null, cts.Token); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); }
                return ModFind.Search(source, mods, docs, gi, progress, cts.Token);
            }, cts.Token);
            if (_findCts != cts) return;
            _findResult = result;
            _findSearch.Text = "";
            ModsInfo.IsOpen = false;
        }
        catch (OperationCanceledException) { }
        catch (Exception e)
        {
            App.Log(e);
            if (_findCts == cts) SetInfo(ModsInfo, InfoBarSeverity.Error, T("The file could not be read"), ErrorText(e));
        }
        if (_findCts != cts) return; // (another file, Cancel, or the page is closed)
        _findCts = null;
        if (_findPage is null) return;
        FillFindHead();
        FillFind();
    }

    // each item with where it stands now, and the mod it is found in
    sealed record FoundItem(FindMatch Match, FindState State, FindHit? Hit, ModFile? Mod);

    List<FoundItem> FindNow()
    {
        var live = new Dictionary<string, ModFile>(StringComparer.OrdinalIgnoreCase);
        foreach (var m in _mods.Where(m => m.FileType == "package"))
            if (m.Enabled || !live.ContainsKey(m.Rel)) live[m.Rel] = m;
        bool? On(string rel) => live.TryGetValue(rel, out var m) ? m.Enabled : null;
        return (_findResult?.Matches ?? []).Select(m =>
        {
            var state = ModFind.StateOf(m, On);
            var hit = state is FindState.Found or FindState.Off ? m.Hits.FirstOrDefault(h => On(h.Rel) == (state == FindState.Found)) : null;
            return new FoundItem(m, state, hit, hit is null ? null : live[hit.Rel]);
        }).ToList();
    }

    void FillFind()
    {
        if (_findPage is null) return;
        var any = _findResult is { Source.Items.Count: > 0 };
        _findBar.Visibility = _findList.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
        FillFindRows();
    }

    static string Tabbed(FindState s) => s switch { FindState.Off => "off", FindState.Missing => "missing", _ => "found" };

    void FillFindRows()
    {
        if (_findPage is null || _findResult is not { } result) return;
        var now = FindNow();
        foreach (var tab in _findTabs.Items)
        {
            var n = now.Count(i => Tabbed(i.State) == (string)tab.Tag);
            tab.Text = tab.Tag switch { "off" => T("Off ({0})", n), "missing" => T("Missing ({0})", n), _ => T("Found ({0})", n) };
        }
        var q = _findSearch.Text.Trim();
        bool Has(params string?[] texts) => q.Length == 0 || texts.Any(t => t?.Contains(q, StringComparison.OrdinalIgnoreCase) == true);

        var rows = new List<(string Title, string Info, string Brush, string Tip, string Label, Action<Button>? Act)>();
        var mine = now.Where(i => Tabbed(i.State) == _findTab).ToList();
        foreach (var group in mine.Where(i => i.Mod is not null).GroupBy(i => (i.Hit!.Rel.ToLowerInvariant(), i.Hit.Inside?.ToLowerInvariant())).OrderBy(x => x.First().Hit!.Inside ?? x.First().Mod!.Name, StringComparer.OrdinalIgnoreCase))
        {
            var (mod, hit) = (group.First().Mod!, group.First().Hit!);
            var title = hit.Inside ?? Path.GetFileNameWithoutExtension(mod.Name);
            if (!Has([title, mod.Rel, .. group.SelectMany(i => new[] { i.Match.Item.Text, i.Match.Item.Name })])) continue;
            var idle = mod.Enabled && !Loaded(mod);
            var info = string.Join(" · ", new[]
            {
                mod.Enabled ? T("On") : T("Off"),
                hit.Inside is not null ? T("inside the merge {0}", Path.GetFileNameWithoutExtension(mod.Name)) : ModRow.Place(mod),
                idle ? T("the game does not load this folder") : null,
                group.Count() == 1 ? T("1 item") : T("{0} items", group.Count()),
            }.Where(x => x is not null));
            var turnOn = !mod.Enabled && !mod.Locked;
            rows.Add((title, info, mod.Enabled && !idle ? "ApexSuccessBrush" : "ApexWarningBrush", Listed(group.Select(i => i.Match.Item)), turnOn ? T("Turn on") : T("Show"),
                _ => { if (turnOn) TurnOnFound(mod); else { CloseFind(); ShowInMods([mod]); } }));
        }
        var game = mine.Where(i => i.State == FindState.Game && Has(i.Match.Item.Text, i.Match.Item.Name)).ToList();
        if (game.Count > 0)
            rows.Add((T("Comes with the game"), game.Count == 1 ? T("1 item") : T("{0} items", game.Count), "ApexMutedBrush", Listed(game.Select(i => i.Match.Item)), "", null));
        foreach (var i in mine.Where(i => i.State == FindState.Missing && Has(i.Match.Item.Text, i.Match.Item.Name)))
        {
            var item = i.Match.Item;
            var maybe = ModFind.MayBeGame(result, item);
            var info = string.Join(" · ", new[] { item.Name is null && item.Key is not null ? FindKind(item.Type) : null,
                maybe ? result.GameKnown ? T("May come from a game pack you don't have") : T("May come with the game") : T("None of your mods has this item") }.Where(x => x is not null));
            var title = item.Name ?? item.Text;
            rows.Add((title, info, maybe ? "ApexWarningBrush" : "ApexErrorBrush", item.Text, T("Copy"), b =>
            {
                CopyText(item.Text);
                b.Content = T("Copied");
                After(1600, () => b.Content = T("Copy"));
            }));
        }

        // (a button just used keeps the keyboard where it was when its row goes to another tab)
        var focused = _findPage.XamlRoot is { } xr ? FocusManager.GetFocusedElement(xr) as DependencyObject : null;
        var at = -1;
        for (var k = 0; k < _findRows.Children.Count && at < 0; k++)
            for (var p = focused; p is not null; p = VisualTreeHelper.GetParent(p)) if (ReferenceEquals(p, _findRows.Children[k])) { at = k; break; }
        _findRows.Children.Clear();
        foreach (var row in rows.Take(MaxFindRows)) _findRows.Children.Add(FindRow(row.Title, row.Info, row.Brush, row.Tip, row.Label, row.Act));
        if (rows.Count > MaxFindRows) _findRows.Children.Add(FindLine(T("and {0} more", rows.Count - MaxFindRows)));
        if (rows.Count == 0)
            _findRows.Children.Add(FindLine(q.Length > 0 && mine.Count > 0 ? T("Nothing here matches your search.") : _findTab switch
            {
                "off" => T("Nothing this file uses is turned off."), "missing" => T("Nothing is missing."), _ => T("None of your mods is used by this file."),
            }));
        if (at >= 0 && _findRows.Children.Count > 0 && _findRows.Children[Math.Min(at, _findRows.Children.Count - 1)] is Grid next)
            next.Children.OfType<Button>().FirstOrDefault()?.Focus(FocusState.Programmatic);
    }

    static string Listed(IEnumerable<FindItem> items)
    {
        var all = items.Select(i => i.Name ?? i.Text).ToList();
        return string.Join("\n", all.Take(12)) + (all.Count > 12 ? "\n" + T("and {0} more", all.Count - 12) : "");
    }

    static string FindKind(uint type) => type switch
    {
        0 => T("Item with no name in the file"),
        ModKinds.Objd or 0x02DC343F => T("Object with no name in the file"),
        ModKinds.Casp or 0x0354796A or 0x0358B08A => T("Create a Sim item with no name in the file"),
        0x0333406C => T("Pattern with no name in the file"),
        0x736884F1 => T("Model with no name in the file"),
        _ => T("Build item with no name in the file"),
    };

    static TextBlock FindLine(string text) => new() { Text = text, FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, Padding = new Thickness(18, 16, 18, 16) };

    Grid FindRow(string title, string info, string brush, string tip, string label, Action<Button>? act)
    {
        var g = new Grid { ColumnSpacing = 14, MinHeight = 60, Padding = new Thickness(18, 8, 12, 8), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, _findRows.Children.Count == 0 ? 0 : 1, 0, 0) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Res(brush), VerticalAlignment = VerticalAlignment.Center });
        var text = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = title, FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        text.Children.Add(new TextBlock { Text = info, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        ToolTipService.SetToolTip(text, tip);
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        if (act is not null)
        {
            var b = Button(label, () => { });
            b.MinWidth = 92;
            b.VerticalAlignment = VerticalAlignment.Center;
            b.Click += (_, _) => act(b);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, $"{label}: {title}");
            Grid.SetColumn(b, 2);
            g.Children.Add(b);
        }
        return g;
    }

    // (the list is filled again by SetMods, and this page with it)
    void TurnOnFound(ModFile mod)
    {
        if (_modRows.FirstOrDefault(r => r.File.Path.Equals(mod.Path, StringComparison.OrdinalIgnoreCase)) is { } row) SetMods([row], true);
    }

    async Task SaveFindReportAsync()
    {
        if (_findResult is not { } r) return;
        var picker = new Microsoft.Windows.Storage.Pickers.FileSavePicker(AppWindow.Id) { SuggestedFileName = Path.GetFileNameWithoutExtension(r.Source.Name) + " - " + T("custom content"), DefaultFileExtension = ".txt" };
        picker.FileTypeChoices.Add(T("Text"), new List<string> { ".txt" });
        var file = await picker.PickSaveFileAsync();
        if (file is null) return;
        var now = FindNow();
        var lines = new List<string> { r.Source.Name, r.Source.Items.Count == 1 ? T("1 item") : T("{0} items", r.Source.Items.Count), "" };
        void Part(string title, Func<FoundItem, bool> which)
        {
            var part = now.Where(which).ToList();
            if (part.Count == 0) return;
            lines.Add($"== {title} ({part.Count}) ==");
            foreach (var i in part.OrderBy(i => i.Mod?.Rel, StringComparer.OrdinalIgnoreCase).ThenBy(i => i.Match.Item.Text, StringComparer.OrdinalIgnoreCase))
                lines.Add(string.Join("\t", new[] { i.Match.Item.Text, i.Match.Item.Text == i.Match.Item.Name ? null : i.Match.Item.Name, i.Mod?.Rel, i.Hit?.Inside }.Where(x => x is not null)));
            lines.Add("");
        }
        Part(T("Found"), i => i.State == FindState.Found);
        Part(T("Off"), i => i.State == FindState.Off);
        Part(T("Missing"), i => i.State == FindState.Missing);
        Part(T("Comes with the game"), i => i.State == FindState.Game);
        try
        {
            await Task.Run(() => File.WriteAllLines(file.Path, lines, new UTF8Encoding(true)));
            SetInfo(ModsInfo, InfoBarSeverity.Success, T("Report saved"), file.Path, T("Open"), () => OpenLog(file.Path));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The report was not saved"), ErrorText(e)); }
    }

    // ---------- the game's caches ----------

    async Task ChooseCachesAsync()
    {
        if (_game is null) return;
        var docs = Docs;
        List<CacheFile> files;
        bool kept;
        try
        {
            Engine.CheckWritable();
            (files, kept) = await Task.Run(() => (GameCaches.List(docs, Enum.GetValues<CacheGroup>()), GameCaches.Backup is not null));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); return; }
        long Size(CacheGroup g) => files.Where(f => f.Group == g).Sum(f => f.Size);

        var groups = new List<(CacheGroup Group, string Name, string Text, bool On)>
        {
            (CacheGroup.CreateASim, T("Create a Sim"), T("Hair and clothes that are gone or look wrong"), true),
            (CacheGroup.Scripts, T("Scripts"), T("After you change script mods"), true),
            (CacheGroup.Worlds, T("Worlds"), T("When a world takes long to load"), true),
            (CacheGroup.Thumbnails, T("Thumbnails"), T("Wrong pictures in Buy and Build mode"), false),
            (CacheGroup.Online, T("Online"), T("Store pictures and online features that don't load"), false),
            (CacheGroup.ErrorLogs, T("Old error logs"), T("Script errors Health is done with"), true),
        };
        var body = new StackPanel { Spacing = 8, Width = 480 };
        body.Children.Add(Muted(T("The game rebuilds its caches the next time it starts.")));
        var d = new ContentDialog { Title = T("Clear caches"), PrimaryButtonText = T("Clear"), CloseButtonText = T("Cancel"), SecondaryButtonText = kept ? T("Put the caches back") : "", DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        var total = new TextBlock { FontSize = 13, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 6, 0, 0) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetLiveSetting(total, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        var boxes = new List<(CacheGroup Group, CheckBox Box)>();
        void Total()
        {
            var on = boxes.Where(b => b.Box.IsChecked == true).Sum(b => Size(b.Group));
            total.Text = on == 0 ? T("Nothing to clear") : T("{0} to clear", ModRow.SizeText(on));
            d.IsPrimaryButtonEnabled = boxes.Any(b => b.Box.IsChecked == true);
        }
        foreach (var (group, name, text, on) in groups)
        {
            var any = files.Any(f => f.Group == group);
            if (!any && group == CacheGroup.Online) continue; // (most players have none)
            var g = new Grid { ColumnSpacing = 12 };
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var words = new StackPanel { Spacing = 1 };
            words.Children.Add(new TextBlock { Text = name, FontSize = 13, FontWeight = FontWeights.SemiBold });
            words.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            g.Children.Add(words);
            var sizeText = any ? ModRow.SizeText(Size(group)) : T("Empty");
            var size = new TextBlock { Text = sizeText, FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(size, 1);
            g.Children.Add(size);
            var box = new CheckBox { Content = g, IsChecked = on && any, IsEnabled = any, MinHeight = 56, Padding = new Thickness(12, 0, 0, 0), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Center };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, $"{name}, {text}, {sizeText}");
            var card = new Border { Child = box, Padding = new Thickness(14, 0, 14, 0), CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), Background = new SolidColorBrush(Pick(0x0AFFFFFF, 0x06000000)), Opacity = any ? 1 : 0.5 };
            void Changed() { card.BorderBrush = Res(box.IsChecked == true ? "ApexAccentLineBrush" : "ApexCardBorderBrush"); Total(); }
            boxes.Add((group, box));
            Changed();
            box.Checked += (_, _) => Changed();
            box.Unchecked += (_, _) => Changed();
            body.Children.Add(card);
        }
        body.Children.Add(total);
        body.Children.Add(Muted(T("A copy is kept until the game closes normally.")));
        d.Content = new ScrollViewer { Content = body, Padding = new Thickness(0, 0, 14, 0) };
        var answer = await ShowDialog(d);
        if (answer == ContentDialogResult.Secondary) { await PutCachesBackAsync(last: false); return; }
        if (answer != ContentDialogResult.Primary) return;
        var chosen = boxes.Where(b => b.Box.IsChecked == true).Select(b => b.Group).ToList();
        try
        {
            var bytes = await ModWork(() => GameCaches.Clean(docs, chosen));
            ModsResult(chosen is [CacheGroup.ErrorLogs] ? T("Old error logs cleared.") : T("Caches cleared. The game rebuilds them when it starts."), () => Safe(PutCachesBackAsync(last: true)), T("{0} freed", ModRow.SizeText(bytes)));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        RefreshGame();
    }

    // last: what the last clear moved, else every copy still kept
    async Task PutCachesBackAsync(bool last)
    {
        try
        {
            var back = await ModWork(() => GameCaches.Restore(last));
            ModsResult(back ? T("Caches put back.") : T("There is no copy to put back."), null, "", back ? InfoBarSeverity.Success : InfoBarSeverity.Informational);
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some caches were not put back"), ErrorText(e)); }
        RefreshGame();
    }
}
