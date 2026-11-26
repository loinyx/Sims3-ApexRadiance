using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Input;
using static ApexSetup.Strings;

namespace ApexSetup;

// One of the Mods screen's tools: Load order, the player's packages in the order the game reads them (Mods\Resource.cfg: a higher
// Priority first, then each directive in turn, then Windows' name order), with which mod wins a conflict. In place of the mods list,
// as Find custom content; Give priority itself stays on a mod's Conflicts tab.
public sealed partial class MainWindow
{
    // a row: a directive's head (Mod null), or a mod with its place in the order (0: the game does not read it)
    sealed record OrderRow(ModFile? Mod, string Head, string Sub, int At);

    Grid? _orderPage;
    List<OrderRow>? _orderAll; // (null: no Resource.cfg)
    string _orderPlace = "";
    bool _orderClashOnly, _orderReady;
    (bool Light, string Lang) _orderLook;
    Dictionary<string, (List<string> Over, List<string> Under)> _orderClash = new(StringComparer.OrdinalIgnoreCase);
    int _orderRun, _orderTyped;
    // (made with the page)
    TextBox _orderSearch = null!;
    ListView _orderList = null!;
    Border _orderBox = null!;
    ContentControl _orderEmpty = null!;

    async Task LoadOrderPageAsync()
    {
        if (_game is null) return;
        if (_step != ModsScreen) Show(ModsScreen);
        if (!_modsLoaded) await LoadModsAsync();
        if (!_modsLoaded || AskLeaveMerge(() => Safe(LoadOrderPageAsync()))) return;
        CloseFind();
        (_orderPlace, _orderAll, _orderReady) = (_modPlace, null, false);
        ModList.SelectedItems.Clear();
        ModGrid.SelectedItems.Clear();
        BuildOrder();
        Animate(_orderPage!);
    }

    // from FillMods: the order follows the mods, the theme and the language; another place closes the page
    void OrderFollows()
    {
        if (_orderPage is null) return;
        if (_modPlace != _orderPlace) { CloseOrder(); return; }
        ModDetailsPane.Visibility = Visibility.Collapsed;
        if (_orderLook != (_light, Lang)) BuildOrder(); else Safe(ReadOrderAsync());
    }

    void CloseOrder(bool focus = false)
    {
        if (_orderPage is null) return;
        _orderRun++;
        TakeInfo((Panel)ModSelectionBar.Parent);
        PlaceSelectionBar();
        ScreenMods.Children.Remove(_orderPage);
        _orderPage = null;
        ((FrameworkElement)ModList.Parent).Visibility = Visibility.Visible;
        FillModDetails();
        if (focus) (_modsGrid ? (Control)ModGrid : ModList).Focus(FocusState.Programmatic);
    }

    void BuildOrder()
    {
        WatchPlaces();
        var query = _orderPage is null ? "" : _orderSearch.Text;
        var page = new Grid { Tag = "dyn", Padding = new Thickness(Design.Space.Page, Design.Space.X6, Design.Space.Page, 0), RowSpacing = Design.Space.X4, Background = Res("ApexClearBrush") };
        for (var i = 0; i < 3; i++) page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition());
        AutomationProperties.SetName(page, T("Load order"));
        void Put(FrameworkElement e, int row) { Grid.SetRow(e, row); page.Children.Add(e); }

        var top = new StackPanel { Spacing = 2 };
        var back = Design.Parts.Back(T("Mods"), T("Back to your mods"), () => CloseOrder(focus: true));
        back.Margin = new Thickness(0, 0, 0, Design.Space.X4);
        top.Children.Add(back);
        top.Children.Add(PageHead(T("Load order"), T("The order the game reads your mods in. When two change the same thing, the one read first wins.")));
        Put(top, 0);

        var info = new StackPanel();
        Put(info, 1);

        // the two views and a search
        var bar = new Grid { ColumnSpacing = Design.Space.X2 };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.ColumnDefinitions.Add(new ColumnDefinition());
        var views = new SelectorBar { VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetName(views, T("Show"));
        foreach (var (key, text) in new[] { (false, T("All mods")), (true, T("Only mods that clash")) })
        {
            var tab = Tab(text, !key);
            (tab.Tag, tab.IsSelected) = (key, key == _orderClashOnly);
            views.Items.Add(tab);
        }
        views.SelectionChanged += (s, _) => { if (s.SelectedItem?.Tag is bool k && k != _orderClashOnly) { _orderClashOnly = k; FillOrder(); } };
        bar.Children.Add(views);
        _orderSearch = new TextBox { PlaceholderText = T("Search"), Text = query, Width = 280, MinWidth = 100, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetName(_orderSearch, T("Search"));
        _orderSearch.TextChanged += (_, _) => { var mine = ++_orderTyped; After(Design.Motion.Base, () => { if (mine == _orderTyped) FillOrder(); }); };
        Grid.SetColumn(_orderSearch, 1);
        bar.Children.Add(_orderSearch);
        Put(bar, 2);

        // (a list that can hold thousands: only the rows in view are made)
        _orderList = new ListView { ItemTemplate = EmptyCell(), SelectionMode = ListViewSelectionMode.None, Padding = new Thickness(0, 0, 0, Design.Space.X2) };
        AutomationProperties.SetName(_orderList, T("Load order"));
        _orderList.ContainerContentChanging += (_, a) =>
        {
            if (a.InRecycleQueue || a.Item is not OrderRow r || a.ItemContainer.ContentTemplateRoot is not Grid g) return;
            a.ItemContainer.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            (a.ItemContainer.Padding, a.ItemContainer.MinHeight) = (new Thickness(0), 0);
            a.ItemContainer.IsTabStop = r.Mod is not null;
            PaintOrderRow(a.ItemContainer, g, r);
            a.Handled = true;
        };
        _orderBox = new Border { Child = _orderList, CornerRadius = new CornerRadius(Design.Radius.Card), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), Margin = new Thickness(0, 0, 0, Design.Space.X6) };
        Put(_orderBox, 3);
        _orderEmpty = new ContentControl { HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Top };
        Put(_orderEmpty, 3);

        page.KeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Escape) return;
            e.Handled = true;
            if (_orderSearch.Text.Length > 0) _orderSearch.Text = ""; else CloseOrder(focus: true);
        };
        var find = new KeyboardAccelerator { Modifiers = Windows.System.VirtualKeyModifiers.Control, Key = Windows.System.VirtualKey.F };
        find.Invoked += (_, e) => { e.Handled = true; _orderSearch.Focus(FocusState.Keyboard); };
        page.KeyboardAccelerators.Add(find);
        page.KeyboardAcceleratorPlacementMode = KeyboardAcceleratorPlacementMode.Hidden;

        TakeInfo(info);
        if (_orderPage is not null) ScreenMods.Children.Remove(_orderPage);
        _orderPage = page;
        _orderLook = (_light, Lang);
        Grid.SetColumn(page, 1);
        Grid.SetColumnSpan(page, 2);
        ((FrameworkElement)ModList.Parent).Visibility = Visibility.Collapsed;
        ModDetailsPane.Visibility = Visibility.Collapsed;
        ScreenMods.Children.Add(page);
        FillOrder();
        Safe(ReadOrderAsync());
    }

    // Resource.cfg read and the mods sorted away from the window; who wins each conflict from Health's check (when it ends, again)
    async Task ReadOrderAsync()
    {
        var run = ++_orderRun;
        var (docs, mods) = (Docs, _mods.ToList());
        try
        {
            var all = await Task.Run(() => OrderOf(docs, mods));
            if (run != _orderRun || _orderPage is null) return;
            (_orderAll, _orderClash, _orderReady) = (all, ClashOf(), true);
            FillOrder();
            if (!_healthChecking) return;
            await HealthReady();
            if (run != _orderRun || _orderPage is null) return;
            _orderClash = ClashOf();
            FillOrder();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            App.Log(e);
            if (run == _orderRun) SetInfo(ModsInfo, InfoBarSeverity.Error, T("Resource.cfg could not be read"), ErrorText(e));
        }
    }

    static List<OrderRow>? OrderOf(string docs, List<ModFile> mods)
    {
        var cfg = Path.Combine(docs, "Mods", "Resource.cfg");
        if (!File.Exists(cfg)) return null;
        var dirs = ModConflicts.ParseResourceCfg(File.ReadAllText(cfg));
        var keyed = mods.Where(m => m.Enabled && m.FileType == "package" && m.Rel.StartsWith("Mods" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            .Select(m => (Mod: m, Key: ModConflicts.LoadKey(dirs, Path.GetRelativePath("Mods", m.Rel).Replace('\\', '/')))).ToList();
        var rows = new List<OrderRow>();
        var at = 0;
        foreach (var group in keyed.Where(k => k.Key is not null).OrderBy(k => k.Key!.Value, Comparer<(int, int, string)>.Create(ModConflicts.CompareKey)).GroupBy(k => k.Key!.Value.Item2))
        {
            var d = dirs[group.Key];
            rows.Add(new(null, d.Text, T("Priority {0}", d.Priority), 0));
            rows.AddRange(group.Select(k => new OrderRow(k.Mod, "", "", ++at)));
        }
        var unread = keyed.Where(k => k.Key is null).OrderBy(k => ModConflicts.NameKey(k.Mod.Rel), StringComparer.Ordinal).ToList();
        if (unread.Count > 0)
        {
            rows.Add(new(null, T("Not read by the game"), "", 0));
            rows.AddRange(unread.Select(k => new OrderRow(k.Mod, "", "", 0)));
        }
        return rows;
    }

    // by a mod's path: the mods it wins over and the ones it loses to, in the conflicts the order decides
    Dictionary<string, (List<string> Over, List<string> Under)> ClashOf()
    {
        var o = new Dictionary<string, (List<string> Over, List<string> Under)>(StringComparer.OrdinalIgnoreCase);
        (List<string> Over, List<string> Under) Of(string path) => o.TryGetValue(path, out var x) ? x : o[path] = ([], []);
        foreach (var c in ConflictsNow().Where(c => c.CanOrder))
        {
            var n = Math.Min(c.Files.Length, c.Names.Length);
            var w = c.InUse is { } used ? Array.IndexOf(c.Labels, used) : c.Winner is { } win ? Array.IndexOf(c.Parties, win) : -1;
            if (w < 0 || w >= n) continue;
            for (var i = 0; i < n; i++)
            {
                if (i == w) Of(c.Files[i]).Over.AddRange(c.Names.Take(n).Where((_, k) => k != w));
                else if (i >= c.Labels.Length || !c.Alike.Contains(c.Labels[i])) Of(c.Files[i]).Under.Add(c.Names[w]); // (a same copy loses nothing)
            }
        }
        return o;
    }

    // one mod by its name (a long one cut, the row's own name says it whole), more by their number
    static string Others(List<string> names) => names.Distinct(StringComparer.OrdinalIgnoreCase).ToList() is var all && all is [var one]
        ? one.Length > 28 ? one[..27] + "…" : one : T("{0} mods", all.Count);

    void FillOrder()
    {
        if (_orderPage is null) return;
        if (_orderAll is null)
        {
            _orderBox.Visibility = Visibility.Collapsed;
            _orderEmpty.Content = !_orderReady ? null : EmptyPlace("list-ordered", T("The Mods folder has no Resource.cfg, so the game reads no mods."));
            return;
        }
        var q = _orderSearch.Text.Trim();
        bool Shown(ModFile m) => (!_orderClashOnly || _orderClash.ContainsKey(m.Path))
            && (q.Length == 0 || m.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || ModRow.Place(m).Contains(q, StringComparison.OrdinalIgnoreCase));
        var shown = new List<OrderRow>();
        OrderRow? head = null;
        foreach (var r in _orderAll)
        {
            if (r.Mod is null) { head = r; continue; }
            if (!Shown(r.Mod)) continue;
            if (head is not null) { shown.Add(head); head = null; }
            shown.Add(r);
        }
        _orderList.ItemsSource = shown;
        _orderBox.Visibility = shown.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        _orderEmpty.Content = shown.Count > 0 ? null
            : q.Length > 0 ? EmptyPlace("search", T("Nothing here matches your search."))
            : EmptyPlace("list-ordered", _orderClashOnly ? T("None of your mods change the same thing as another.") : T("No mods are turned on."));
    }

    void PaintOrderRow(Microsoft.UI.Xaml.Controls.Primitives.SelectorItem c, Grid g, OrderRow r)
    {
        g.Children.Clear();
        g.ColumnDefinitions.Clear();
        if (r.Mod is not { } m)
        {
            // a directive: what it reads and its priority
            var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X3, Padding = new Thickness(Design.Space.RowPad, Design.Space.X4, Design.Space.RowPad, Design.Space.X2) };
            var t = new TextBlock { Text = r.Head, FontSize = Design.TextSize.Small, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
            s.Children.Add(t);
            if (r.Sub.Length > 0) s.Children.Add(new TextBlock { Text = r.Sub, FontSize = Design.TextSize.Caption, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
            g.Children.Add(s);
            AutomationProperties.SetName(c, r.Sub.Length > 0 ? $"{r.Head}, {r.Sub}" : r.Head);
            AutomationProperties.SetHeadingLevel(c, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
            return;
        }
        AutomationProperties.SetHeadingLevel(c, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.None);
        (g.ColumnSpacing, g.MinHeight, g.Padding) = (Design.Space.X3, Design.Size.ControlLarge + Design.Space.X3, new Thickness(Design.Space.RowPad, Design.Space.X1, Design.Space.X3, Design.Space.X1));
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(Design.Space.X10) });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var number = new TextBlock { Text = r.At > 0 ? r.At.ToString(LangCulture()) : "", FontSize = Design.TextSize.Small, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right };
        Typography.SetNumeralAlignment(number, FontNumeralAlignment.Tabular);
        g.Children.Add(number);
        var name = Path.GetFileNameWithoutExtension(m.Name);
        var words = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = name, FontSize = Design.TextSize.Small, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        words.Children.Add(new TextBlock { Text = ModRow.Place(m), FontSize = Design.TextSize.Caption, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        ToolTipService.SetToolTip(words, m.Rel);
        Grid.SetColumn(words, 1);
        g.Children.Add(words);
        var said = new List<string> { r.At > 0 ? r.At.ToString(LangCulture()) : T("Not read by the game"), name, ModRow.Place(m) };
        if (_orderClash.TryGetValue(m.Path, out var clash))
        {
            var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X2 };
            if (clash.Over.Count > 0) { var w = T("Wins over {0}", Others(clash.Over)); chips.Children.Add(Design.Parts.Chip("circle-check", w, tone: "Success")); said.Add(w); }
            if (clash.Under.Count > 0) { var l = T("Loses to {0}", Others(clash.Under)); chips.Children.Add(Design.Parts.Chip("triangle-alert", l, warn: true)); said.Add(l); }
            Grid.SetColumn(chips, 2);
            g.Children.Add(chips);
            var see = Button(T("See conflicts"), () => SeeConflicts(m));
            see.VerticalAlignment = VerticalAlignment.Center;
            AutomationProperties.SetName(see, T("See conflicts") + ", " + name);
            Grid.SetColumn(see, 3);
            g.Children.Add(see);
        }
        AutomationProperties.SetName(c, string.Join(", ", said));
    }

    // the mod alone in the list, chosen, its details open on Conflicts (where Give priority is)
    void SeeConflicts(ModFile m)
    {
        CloseOrder();
        (_detailsFor, _detailsTab) = (m.Rel, "conflicts");
        ShowInMods([m]);
    }
}
