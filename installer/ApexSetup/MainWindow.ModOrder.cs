using System.Collections.ObjectModel;
using System.Diagnostics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using static ApexSetup.Strings;

namespace ApexSetup;

// One of the Mods screen's tools: Load order, the player's packages in the order the game reads them (Mods\Resource.cfg: a higher
// Priority first, then each line in turn, then the names inside a folder), one section per line, with which mod wins a conflict.
// Rows are dragged (or moved with Alt and the arrows) into a new order that nothing writes until Apply (ModLoadOrder), with Undo
// before and after. Beside the list, how the game reads: each line's priority typed over in place, a new priority folder.
public sealed partial class MainWindow
{
    // a row of the list: a section's head (Mod null), a mod of a section, or one the game does not read (Section -1)
    sealed record OrderItem(ModFile? Mod, int Section, int At, bool First, bool Last, bool Moved);
    // what Resource.cfg and the mods say: its lines, the mods it reads in its order, the ones it does not
    sealed record OrderData(List<ModLoadOrder.Section> Secs, List<(ModFile Mod, int Section)> Read, List<ModFile> Unread);

    Grid? _orderPage;
    OrderData? _order; // (null: no Resource.cfg)
    List<(ModFile Mod, int Section)> _orderWant = []; // the order the player is making
    string _orderPlace = "";
    bool _orderClashOnly, _orderReady;
    (bool Light, string Lang) _orderLook;
    Dictionary<string, (List<string> Over, List<string> Under)> _orderClash = new(StringComparer.OrdinalIgnoreCase);
    int _orderRun, _orderTyped, _orderLit = -1;
    // (made with the page)
    TextBox _orderSearch = null!;
    ListView _orderList = null!;
    ObservableCollection<OrderItem> _orderItems = [];
    string? _orderFilter;
    Border _orderViews = null!, _orderBar = null!, _orderPanel = null!;
    Grid _orderBody = null!;
    bool? _orderUnder;
    StackPanel _orderRules = null!;
    TextBlock _orderBarTitle = null!, _orderBarLine = null!;
    ContentControl _orderEmpty = null!;
    readonly Dictionary<int, Border> _orderRuleRows = [];
    static ScrollViewer? FirstScroller(DependencyObject o)
    {
        for (int k = 0; k < Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChildrenCount(o); k++)
            if (Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChild(o, k) is var c && (c as ScrollViewer ?? FirstScroller(c)) is { } s) return s;
        return null;
    }

    async Task LoadOrderPageAsync()
    {
        if (_game is null) return;
        if (_step != ModsScreen) Show(ModsScreen);
        if (!_modsLoaded) await LoadModsAsync();
        if (!_modsLoaded || AskLeaveMerge(() => Safe(LoadOrderPageAsync()))) return;
        CloseFind();
        (_orderPlace, _order, _orderWant, _orderReady) = (_modPlace, null, [], false);
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
        var page = new Grid { Tag = "dyn", Padding = new Thickness(Design.Space.Page, Design.Space.X6, Design.Space.Page, 0), RowSpacing = Design.Space.X6, Background = Res("ApexClearBrush") };
        for (var i = 0; i < 2; i++) page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition());
        AutomationProperties.SetName(page, T("Load order"));
        void Put(FrameworkElement e, int row) { Grid.SetRow(e, row); page.Children.Add(e); }

        var top = new StackPanel { Spacing = 2 };
        var back = Design.Parts.Back(T("Mods"), T("Back to your mods"), () => CloseOrder(focus: true));
        back.Margin = new Thickness(0, 0, 0, Design.Space.X4);
        top.Children.Add(back);
        top.Children.Add(PageHead(T("Load order"), T("The game reads from the top down, and when two mods change the same thing the one above wins")));
        Put(top, 0);

        // the two views and a search
        var bar = new Grid { ColumnSpacing = Design.Space.X3 };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bar.ColumnDefinitions.Add(new ColumnDefinition());
        _orderViews = new Border();
        bar.Children.Add(_orderViews);
        _orderSearch = new TextBox { PlaceholderText = T("Search"), Text = query };
        AutomationProperties.SetName(_orderSearch, T("Search"));
        _orderSearch.TextChanged += (_, _) => { var mine = ++_orderTyped; After(Design.Motion.Base, () => { if (mine == _orderTyped) FillOrder(); }); };
        var search = SearchField(_orderSearch);
        (search.Width, search.MinWidth, search.HorizontalAlignment) = (300, 160, HorizontalAlignment.Right);
        Grid.SetColumn(search, 1);
        bar.Children.Add(search);
        Put(bar, 1);

        // the list (only the rows in view are made: it holds thousands) and how the game reads, beside it or under it
        _orderBody = new Grid();
        _orderUnder = null;
        _orderBody.ColumnDefinitions.Add(new ColumnDefinition());
        _orderBody.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _orderItems = [];
        _orderList = new ListView
        {
            ItemTemplate = EmptyCell(), ItemsSource = _orderItems, SelectionMode = ListViewSelectionMode.None, IsItemClickEnabled = true,
            CanDragItems = true, CanReorderItems = true, AllowDrop = true, Padding = new Thickness(0, 0, 0, Design.Space.X16 + Design.Space.X8),
        };
        // (the row is the item, as the game list's: no list chrome around it, so rows join into one card and keyboard focus is
        // the system ring on the card's own corners)
        _orderList.ItemContainerStyle = (Style)Microsoft.UI.Xaml.Markup.XamlReader.Load(
            "<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' TargetType='ListViewItem'>"
            + "<Setter Property='Padding' Value='0'/><Setter Property='Margin' Value='0'/><Setter Property='MinHeight' Value='0'/>"
            + "<Setter Property='HorizontalContentAlignment' Value='Stretch'/><Setter Property='UseSystemFocusVisuals' Value='True'/>"
            + "<Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ListViewItem'>"
            + "<ContentPresenter Content='{TemplateBinding Content}' ContentTemplate='{TemplateBinding ContentTemplate}' HorizontalContentAlignment='Stretch' Background='Transparent'/>"
            + "</ControlTemplate></Setter.Value></Setter></Style>");
        AutomationProperties.SetName(_orderList, T("Load order"));
        AutomationProperties.SetHelpText(_orderList, T("Alt with the up or down arrow moves a mod."));
        _orderList.ContainerContentChanging += (_, a) =>
        {
            if (a.InRecycleQueue || a.Item is not OrderItem r || a.ItemContainer.ContentTemplateRoot is not Grid g) return;
            a.ItemContainer.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            (a.ItemContainer.Padding, a.ItemContainer.MinHeight, a.ItemContainer.Margin) = (new Thickness(0), 0, new Thickness(0));
            // (the template keeps 4 around each item and draws its own white ring: rows join here, and the ring follows the card)
            const double round = Design.Radius.Card;
            a.ItemContainer.CornerRadius = r.Mod is null ? new CornerRadius(0)
                : new CornerRadius(r.First ? round : 0, r.First ? round : 0, r.Last ? round : 0, r.Last ? round : 0);
            (a.ItemContainer.FocusVisualPrimaryBrush, a.ItemContainer.FocusVisualSecondaryBrush) = (Res("ApexAccentBrush"), Res("ApexClearBrush"));
            (a.ItemContainer.FocusVisualPrimaryThickness, a.ItemContainer.FocusVisualSecondaryThickness, a.ItemContainer.FocusVisualMargin) = (new Thickness(2), new Thickness(0), new Thickness(0));
            a.ItemContainer.IsTabStop = r.Mod is not null;
            HookOrderRow(a.ItemContainer);
            PaintOrderRow(a.ItemContainer, g, r);
            a.Handled = true;
        };
        _orderList.ItemClick += (_, e) => { if (e.ClickedItem is OrderItem { Mod: { } m } && _orderClash.ContainsKey(m.Path)) SeeConflicts(m); };
        OrderItem? dragged = null;
        _orderList.DragItemsStarting += (_, e) =>
        {
            (_dragRows, _dragFolder) = (null, null); // (the side pane's folders must not take these as mods)
            dragged = e.Items is [OrderItem { Mod: { Locked: false }, Section: >= 0 } one] ? one : null;
            if (dragged is null) e.Cancel = true;
        };
        _orderList.DragItemsCompleted += (_, _) =>
        {
            DragEnded?.Invoke();
            if (dragged is { } d) { dragged = null; OrderMoveTo(d, _orderItems.ToList(), _orderItems.IndexOf(d)); }
        };
        _orderList.PreviewKeyDown += (_, e) =>
        {
            if (e.Key is not (VirtualKey.Up or VirtualKey.Down)) return;
            if (!Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down)) return;
            if (FocusManager.GetFocusedElement(Content.XamlRoot) is not ListViewItem c || _orderList.ItemFromContainer(c) is not OrderItem { Mod: { Locked: false }, Section: >= 0 } r) return;
            e.Handled = true;
            var view = _orderItems.ToList();
            var at = view.IndexOf(r);
            var to = e.Key == VirtualKey.Up ? at - 1 : at + 1;
            if (to < 0 || to >= view.Count) return;
            view.RemoveAt(at);
            view.Insert(to, r);
            OrderMoveTo(r, view, to);
        };
        _orderList.PointerExited += (_, _) => LightRule(-1);
        _orderBody.Children.Add(_orderList);
        _orderEmpty = new ContentControl { HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Top };
        _orderBody.Children.Add(_orderEmpty);
        _orderPanel = OrderPanel();
        Put(_orderBody, 2);
        // (under ~1080 wide the panel goes under the list, as its footer, so the list keeps its own scrolling)
        page.SizeChanged += (_, e) => PlaceOrderPanel(e.NewSize.Width);

        // what the drag made, floating at the bottom as Mods' and Saves' results; the messages float there too
        var floats = new Grid();
        Grid.SetRow(floats, 2);
        page.Children.Add(floats);
        _orderBar = OrderBar();
        floats.Children.Add(_orderBar);
        TakeInfo(floats);
        (ModsInfo.HorizontalAlignment, ModsInfo.VerticalAlignment, ModsInfo.MinWidth, ModsInfo.MaxWidth) = (HorizontalAlignment.Center, VerticalAlignment.Bottom, 360, 720);
        (ModsInfo.Margin, ModsInfo.Translation) = (new Thickness(Design.Space.X3, 0, Design.Space.X3, Design.Space.X4), new System.Numerics.Vector3(0, 0, 32));

        page.KeyDown += (_, e) =>
        {
            if (e.Key != VirtualKey.Escape) return;
            e.Handled = true;
            if (_orderSearch.Text.Length > 0) _orderSearch.Text = ""; else CloseOrder(focus: true);
        };
        void Key(VirtualKey key, Action go)
        {
            var k = new KeyboardAccelerator { Modifiers = VirtualKeyModifiers.Control, Key = key };
            k.Invoked += (_, e) => { e.Handled = true; go(); };
            page.KeyboardAccelerators.Add(k);
        }
        Key(VirtualKey.F, () => _orderSearch.Focus(FocusState.Keyboard));
        Key(VirtualKey.Z, OrderUndo);
        page.KeyboardAcceleratorPlacementMode = KeyboardAcceleratorPlacementMode.Hidden;

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

    void PlaceOrderPanel(double width)
    {
        var under = width < 1080 - 248; // (the window's 1080, less the Mods side pane)
        if (under == _orderUnder) return;
        _orderUnder = under;
        _orderBody.Children.Remove(_orderPanel);
        _orderList.Footer = null;
        if (under)
        {
            _orderPanel.Margin = new Thickness(0, Design.Space.Section, 0, 0);
            _orderList.Footer = _orderPanel;
        }
        else
        {
            _orderPanel.Margin = new Thickness(Design.Space.X6, 0, 0, Design.Space.X6);
            Grid.SetColumn(_orderPanel, 1);
            _orderBody.Children.Add(_orderPanel);
        }
    }

    // Resource.cfg read and the mods sorted away from the window; who wins each conflict from Health's check (when it ends, again)
    async Task ReadOrderAsync()
    {
        var run = ++_orderRun;
        var (docs, mods) = (Docs, _mods.ToList());
        try
        {
            var data = await Task.Run(() => OrderOf(docs, mods));
            if (run != _orderRun || _orderPage is null) return;
            // (an order being made stays while the mods and the lines are the same ones)
            string Sig(OrderData? d) => d is null ? "" : string.Join("|", d.Read.Select(x => x.Mod.Path + ">" + d.Secs[x.Section].Text).Order(StringComparer.OrdinalIgnoreCase));
            if (Sig(data) != Sig(_order)) _orderWant = data?.Read.ToList() ?? [];
            else if (data is not null) _orderWant = _orderWant.Select(w => (data.Read.First(x => x.Mod.Path.Equals(w.Mod.Path, StringComparison.OrdinalIgnoreCase)).Mod, w.Section)).ToList();
            (_order, _orderClash, _orderReady) = (data, ClashOf(), true);
            FillRules();
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

    static OrderData? OrderOf(string docs, List<ModFile> mods)
    {
        var cfg = ModLoadOrder.CfgPath(docs);
        if (!File.Exists(cfg)) return null;
        var text = File.ReadAllText(cfg);
        var dirs = ModConflicts.ParseResourceCfg(text);
        var keyed = mods.Where(m => m.Enabled && m.FileType == "package" && m.Rel.StartsWith("Mods" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            .Select(m => (Mod: m, Key: ModConflicts.LoadKey(dirs, ModLoadOrder.Under(m)))).ToList();
        var read = keyed.Where(k => k.Key is not null).OrderBy(k => k.Key!.Value, Comparer<(int, int, string)>.Create(ModConflicts.CompareKey)).Select(k => (k.Mod, k.Key!.Value.Item2)).ToList();
        var unread = keyed.Where(k => k.Key is null).OrderBy(k => ModConflicts.NameKey(k.Mod.Rel), StringComparer.Ordinal).Select(k => k.Mod).ToList();
        return new(ModLoadOrder.Sections(text), read, unread);
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

    static string ModCount(int n) => n == 1 ? T("1 mod") : T("{0} mods", n);

    // a line in words: its folder, and how far under it it reads
    static string OrderWords(ModLoadOrder.Section s) =>
        string.Join(" › ", (s.Folder.Length == 0 ? [T("Mods")] : s.Folder.Split('/')).Concat(s.Depth switch { 0 => [], 1 => [T("subfolders")], var n => new[] { T("{0} levels down", n) } }));

    // the sections the list shows: the lines that read a mod when Resource.cfg was read, in the order the game reads them
    List<int> OrderHeads() => _order!.Read.Select(x => x.Section).Distinct().ToList();

    void FillOrder()
    {
        if (_orderPage is null) return;
        if (_order is null)
        {
            (_orderList.Visibility, _orderPanel.Visibility, _orderViews.Child) = (Visibility.Collapsed, Visibility.Collapsed, null);
            _orderEmpty.Content = !_orderReady ? null : EmptyPlace("list-ordered", T("The Mods folder has no Resource.cfg, so the game reads no mods."));
            FillOrderBar();
            return;
        }
        _orderPanel.Visibility = Visibility.Visible;
        var clashing = _order.Read.Count(x => _orderClash.ContainsKey(x.Mod.Path));
        if (_orderViews.Tag as string != $"{_order.Read.Count}|{clashing}|{_orderClashOnly}")
        {
            _orderViews.Tag = $"{_order.Read.Count}|{clashing}|{_orderClashOnly}";
            var views = new Design.Segmented([("list", $"{T("All")}  {_order.Read.Count}"), ("swords", $"{T("Only mods that clash")}  {clashing}")], _orderClashOnly ? 1 : 0,
                at => { _orderClashOnly = at == 1; _orderViews.Tag = null; FillOrder(); });
            AutomationProperties.SetName(views, T("Show"));
            _orderViews.Child = views;
        }
        var q = _orderSearch.Text.Trim();
        bool Shown(ModFile m) => (!_orderClashOnly || _orderClash.ContainsKey(m.Path))
            && (q.Length == 0 || m.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || ModRow.Place(m).Contains(q, StringComparison.OrdinalIgnoreCase));
        var all = q.Length == 0 && !_orderClashOnly;
        var was = _order.Read.Select((x, i) => (x, i)).ToDictionary(p => p.x.Mod.Path, p => (At: p.i, p.x.Section), StringComparer.OrdinalIgnoreCase);
        var shown = new List<OrderItem>();
        void Add(int section, List<(ModFile Mod, int At, bool Moved)> rows)
        {
            if (!all && rows.Count == 0) return;
            shown.Add(new(null, section, shown.Count == 0 ? 0 : 1, false, false, false));
            shown.AddRange(rows.Select((r, k) => new OrderItem(r.Mod, section, r.At, k == 0, k == rows.Count - 1, r.Moved)));
        }
        var at = _orderWant.Select((w, i) => (w, i)).ToList();
        var moved = OrderMoved();
        foreach (var s in OrderHeads())
            Add(s, at.Where(x => x.w.Section == s && Shown(x.w.Mod)).Select(x => (x.w.Mod, x.i + 1, moved.Contains(x.w.Mod))).ToList());
        if (_order.Unread.Where(Shown).ToList() is { Count: > 0 } unread) Add(-1, unread.Select(m => (m, 0, false)).ToList());
        // (filled again with the same search and filter, as when Health ends or after a move, the list keeps its place)
        var filter = q + "|" + _orderClashOnly;
        var keep = filter == _orderFilter && FirstScroller(_orderList) is { } sv ? sv.VerticalOffset : (double?)null;
        _orderFilter = filter;
        _orderItems = new ObservableCollection<OrderItem>(shown);
        _orderList.ItemsSource = _orderItems;
        if (keep is { } y) _orderList.DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () => FirstScroller(_orderList)?.ChangeView(null, y, null, true));
        _orderList.Visibility = shown.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        _orderEmpty.Content = shown.Count > 0 ? null
            : q.Length > 0 ? EmptyPlace("search", T("Nothing here matches your search."))
            : EmptyPlace("list-ordered", _orderClashOnly ? T("None of your mods change the same thing as another.") : T("No mods are turned on."));
        FillOrderBar();
        FillRules();
    }

    // ---------- moving ----------

    // a row put at `at` of the list as it is now (`view`): its section is the head above it, its place among the mods shown
    // there stands for its place among all of them (the ones a search hides stay where they are)
    void OrderMoveTo(OrderItem r, List<OrderItem> view, int at)
    {
        var head = view.Take(at).LastOrDefault(i => i.Mod is null);
        if (r.Mod is not { } m || head is null || head.Section < 0 || at < 0) { FillOrder(); return; }
        var s = head.Section;
        var want = _orderWant.Where(w => w.Mod != m).ToList();
        int Index(ModFile x) => want.FindIndex(w => w.Mod == x);
        var next = view.Skip(at + 1).TakeWhile(i => i.Mod is not null).FirstOrDefault(i => i.Section == s);
        var prev = view.Take(at).Reverse().TakeWhile(i => i.Mod is not null).FirstOrDefault(i => i.Section == s);
        var heads = OrderHeads();
        int to;
        if (next?.Mod is { } n) to = Index(n);
        else if (prev?.Mod is { } p) to = Index(p) + 1;
        else
        {
            // (no row of that section in view: after its last mod, else before the first mod of a section read later)
            var last = want.FindLastIndex(w => w.Section == s);
            var later = want.FindIndex(w => heads.IndexOf(w.Section) > heads.IndexOf(s));
            to = last >= 0 ? last + 1 : later >= 0 ? later : want.Count;
        }
        want.Insert(Math.Clamp(to, 0, want.Count), (m, s));
        _orderWant = want;
        FillOrder();
        // (the row was made again: the keyboard goes back to it)
        DispatcherQueue.TryEnqueue(() =>
        {
            if (_orderItems.FirstOrDefault(i => i.Mod == m) is not { } item) return;
            _orderList.ScrollIntoView(item);
            _orderList.UpdateLayout();
            (_orderList.ContainerFromItem(item) as ListViewItem)?.Focus(FocusState.Keyboard);
        });
    }

    bool OrderPending() => _order is not null && !_orderWant.SequenceEqual(_order.Read);

    // the floating bar of an order not applied yet: how many mods moved, where one went, Undo and Apply
    Border OrderBar()
    {
        var g = new Grid { ColumnSpacing = Design.Space.X4 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        g.Children.Add(Design.IconTile.Of("arrow-up-down"));
        _orderBarTitle = new TextBlock { FontSize = Design.TextSize.Body, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        _orderBarLine = new TextBlock { FontSize = Design.TextSize.Small, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis };
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center, Children = { _orderBarTitle, _orderBarLine } };
        Grid.SetColumn(words, 1);
        g.Children.Add(words);
        var undo = Button(T("Undo"), OrderUndo);
        var apply = Button(T("Apply"), () => Safe(OrderApplyAsync()), accent: true);
        foreach (var (b, col) in new[] { (undo, 2), (apply, 3) }) { (b.VerticalAlignment, b.Height) = (VerticalAlignment.Center, Design.Size.Control); Grid.SetColumn(b, col); g.Children.Add(b); }
        var bar = new Border
        {
            Child = g, Padding = new Thickness(Design.Space.X4, Design.Space.X3, Design.Space.X3, Design.Space.X3), CornerRadius = new CornerRadius(Design.Radius.Card),
            Background = Res("ApexRaisedBrush"), BorderBrush = Res("ApexInputBorderBrush"), BorderThickness = new Thickness(1), Shadow = new Microsoft.UI.Xaml.Media.ThemeShadow(),
            Translation = new System.Numerics.Vector3(0, 0, 24), VerticalAlignment = VerticalAlignment.Bottom, HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(Design.Space.X3, 0, Design.Space.X3, Design.Space.X4), MinWidth = 360, MaxWidth = 620, Visibility = Visibility.Collapsed,
        };
        AutomationProperties.SetLiveSetting(_orderBarTitle, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        return bar;
    }

    // the mods the player moved: those off the longest run that kept its order, and any that changed section (moving one
    // mod to the top shifts every number under it, but only that one moved)
    HashSet<ModFile> OrderMoved()
    {
        var read = _order!.Read;
        var from = read.Select((x, i) => (x.Mod, i)).ToDictionary(p => p.Mod, p => p.i);
        var seq = _orderWant.Select(w => from.TryGetValue(w.Mod, out var i) ? i : -1).ToList();
        // longest increasing run (patience), kept by its members
        var tails = new List<int>(); var prev = new int[seq.Count]; var ends = new List<int>();
        for (int k = 0; k < seq.Count; k++)
        {
            int lo = 0, hi = tails.Count;
            while (lo < hi) { var mid = (lo + hi) / 2; if (tails[mid] < seq[k]) lo = mid + 1; else hi = mid; }
            prev[k] = lo > 0 ? ends[lo - 1] : -1;
            if (lo == tails.Count) { tails.Add(seq[k]); ends.Add(k); } else { tails[lo] = seq[k]; ends[lo] = k; }
        }
        var kept = new HashSet<int>();
        for (var k = ends.Count > 0 ? ends[^1] : -1; k >= 0; k = prev[k]) kept.Add(k);
        var moved = new HashSet<ModFile>();
        for (int k = 0; k < _orderWant.Count; k++)
            if (!kept.Contains(k) || seq[k] < 0 || read[seq[k]].Section != _orderWant[k].Section) moved.Add(_orderWant[k].Mod);
        return moved;
    }

    void FillOrderBar()
    {
        if (!OrderPending())
        {
            _orderBar.Visibility = Visibility.Collapsed;
            return;
        }
        var read = _order!.Read;
        var moved = OrderMoved();
        _orderBarTitle.Text = moved.Count == 1 ? T("1 mod in a new order") : T("{0} mods in a new order", moved.Count);
        var crossed = _orderWant.LastOrDefault(w => read.First(x => x.Mod == w.Mod).Section != w.Section);
        _orderBarLine.Text = crossed.Mod is { } c
            ? T("{0} goes to {1} (priority {2})", Path.GetFileNameWithoutExtension(c.Name), OrderWords(_order.Secs[crossed.Section]), _order.Secs[crossed.Section].Priority)
            : T("Nothing changes in your files until you apply");
        ModsInfo.IsOpen = false;
        _orderBar.Visibility = Visibility.Visible;
    }

    // Ctrl+Z and the bar's Undo: the order being made goes back; with none, the last change applied (as its message's Undo)
    void OrderUndo()
    {
        if (OrderPending()) { _orderWant = _order!.Read.ToList(); FillOrder(); return; }
        if (_modUndo is { } u && ModsInfo.IsOpen && !_busy) { _modUndo = null; ModsInfo.IsOpen = false; u(); }
    }

    async Task OrderApplyAsync()
    {
        if (!OrderPending() || ModsBusy()) return;
        var (docs, want) = (Docs, _orderWant.ToList());
        try
        {
            var u = await ModWork(() => ModLoadOrder.Apply(docs, want));
            _orderWant = _order?.Read.ToList() ?? [];
            if (u is not null) OrderDone(T("The new load order is in place"), u);
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The load order was not changed"), ErrorText(e)); }
        await ModsAgain();
    }

    // a change made: its message offers Undo, which puts the files and Resource.cfg back as they were
    void OrderDone(string done, ModLoadOrder.Undo u)
    {
        var docs = Docs;
        _orderBar.Visibility = Visibility.Collapsed;
        ModsResult(done, () => Safe(Back()));
        async Task Back()
        {
            try { await ModWork(() => { ModLoadOrder.Back(docs, u); return 0; }); ModsResult(T("Every mod is as it was before"), null); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
            await ModsAgain();
        }
    }

    // a change to Resource.cfg's lines (a priority, a new folder, a file made read), off the window's thread; the order being
    // made is let go, as its lines change
    async Task OrderLinesAsync(Func<string, ModLoadOrder.Undo> work, string done)
    {
        if (ModsBusy()) return;
        var docs = Docs;
        try { OrderDone(done, await ModWork(() => work(docs))); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        await ModsAgain();
    }

    Task OrderPriorityAsync(int section, int priority) => OrderLinesAsync(d => ModLoadOrder.SetPriority(d, section, priority), T("Priority changed"));

    async Task AskPriorityAsync(ModLoadOrder.Section s)
    {
        var box = new TextBox { Text = s.Priority.ToString(), Width = Design.Dialogs.Small, InputScope = Digits() };
        AutomationProperties.SetName(box, T("Priority"));
        box.Loaded += (_, _) => box.SelectAll();
        var body = new StackPanel { Spacing = Design.Space.X2, Children = { Design.Parts.Label(T("Priority")), box } };
        var d = new ContentDialog { Tag = "list-ordered", Title = T("Change priority"), Content = body, PrimaryButtonText = T("Save"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        box.TextChanged += (_, _) => d.IsPrimaryButtonEnabled = int.TryParse(box.Text.Trim(), out _);
        if (await ShowDialog(d) == ContentDialogResult.Primary && int.TryParse(box.Text.Trim(), out var p) && p != s.Priority) await OrderPriorityAsync(s.Index, p);
    }

    static InputScope Digits() => new() { Names = { new InputScopeName(InputScopeNameValue.Number) } };

    async Task NewPriorityFolderAsync()
    {
        var name = new TextBox { Width = Design.Dialogs.Small };
        var prio = new TextBox { Text = "1000", Width = Design.Dialogs.Small, InputScope = Digits() };
        AutomationProperties.SetName(name, T("Name"));
        AutomationProperties.SetName(prio, T("Priority"));
        var body = new StackPanel { Spacing = Design.Space.X2, Children = { Design.Parts.Label(T("Name")), name, Design.Parts.Label(T("Priority")), prio } };
        ((FrameworkElement)body.Children[2]).Margin = new Thickness(0, Design.Space.X3, 0, 0);
        var d = new ContentDialog { Tag = "folder-plus", Title = T("New priority folder"), Content = body, PrimaryButtonText = T("Create"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        void Check() => d.IsPrimaryButtonEnabled = name.Text.Trim().Length > 0 && int.TryParse(prio.Text.Trim(), out _);
        name.TextChanged += (_, _) => Check();
        prio.TextChanged += (_, _) => Check();
        Check();
        if (await ShowDialog(d) != ContentDialogResult.Primary || !int.TryParse(prio.Text.Trim(), out var p)) return;
        var n = name.Text.Trim();
        await OrderLinesAsync(docs => ModLoadOrder.NewFolder(docs, n, p), T("Folder created"));
    }

    void OpenResourceCfg()
    {
        var cfg = ModLoadOrder.CfgPath(Docs);
        try { Process.Start(new ProcessStartInfo(cfg) { UseShellExecute = true }); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            // (no program is set for .cfg files)
            try { Process.Start(new ProcessStartInfo("notepad.exe", $"\"{cfg}\"") { UseShellExecute = true }); } catch (Exception e2) { App.Log(e2); }
        }
    }

    // a section's own options: its priority and its folder
    MenuFlyout OrderMenu(ModLoadOrder.Section s)
    {
        var menu = new MenuFlyout();
        var prio = new MenuFlyoutItem { Text = T("Change priority"), Icon = MenuIcon("list-ordered") };
        prio.Click += (_, _) => Safe(AskPriorityAsync(s));
        var open = new MenuFlyoutItem { Text = T("Open folder"), Icon = MenuIcon("folder-open") };
        open.Click += (_, _) => OpenFolder(Path.Combine(Docs, "Mods", s.Folder.Replace('/', Path.DirectorySeparatorChar)));
        menu.Items.Add(prio);
        menu.Items.Add(open);
        return menu;
    }

    Button OrderMore(ModLoadOrder.Section s)
    {
        var more = Design.Parts.Icon("ellipsis", T("More options for {0}", OrderWords(s)));
        more.Flyout = OrderMenu(s);
        return more;
    }

    // ---------- the rows ----------

    // (once per row made: the grip shows under the pointer and with the keyboard on the row)
    readonly System.Runtime.CompilerServices.ConditionalWeakTable<ListViewItem, object> _orderHooked = new();
    void HookOrderRow(Microsoft.UI.Xaml.Controls.Primitives.SelectorItem c)
    {
        if (c is not ListViewItem item || !_orderHooked.TryAdd(item, this)) return;
        void Grip(bool on) { if (item.ContentTemplateRoot is Grid g && g.Children.Count > 0 && g.Children[0] is Border { Tag: FrameworkElement grip } && grip.Tag is not "fixed") grip.Opacity = on ? 1 : 0; }
        item.GotFocus += (_, _) => Grip(true);
        item.LostFocus += (_, _) => Grip(false);
        item.PointerEntered += (_, _) => { Grip(true); if (_orderList.ItemFromContainer(item) is OrderItem r) LightRule(r.Section); };
        item.PointerExited += (_, _) => Grip(item.FocusState != FocusState.Unfocused);
    }

    void PaintOrderRow(Microsoft.UI.Xaml.Controls.Primitives.SelectorItem c, Grid g, OrderItem r)
    {
        g.Children.Clear();
        g.ColumnDefinitions.Clear();
        g.Padding = new Thickness(0);
        if (r.Mod is not { } m) { PaintOrderHead(c, g, r); return; }
        AutomationProperties.SetHeadingLevel(c, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.None);
        var unread = r.Section < 0;
        const double round = Design.Radius.Card;
        var card = new Border
        {
            MinHeight = Design.Size.Row, BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1, 1, 1, r.Last ? 1 : 0),
            CornerRadius = new CornerRadius(r.First ? round : 0, r.First ? round : 0, r.Last ? round : 0, r.Last ? round : 0),
            Background = Res(unread ? "ApexClearBrush" : r.Moved ? "ApexAccentSoftBrush" : "ApexCardBrush"),
        };
        if (!unread && !r.Moved)
        {
            card.PointerEntered += (_, _) => card.Background = Res("ApexHoverBrush");
            card.PointerExited += (_, _) => card.Background = Res("ApexCardBrush");
        }
        var outer = new Grid();
        if (r.Moved) outer.Children.Add(new Border { Width = 3, Margin = new Thickness(0, Design.Space.X3, 0, Design.Space.X3), HorizontalAlignment = HorizontalAlignment.Left, CornerRadius = new CornerRadius(0, 2, 2, 0), Background = Res("ApexAccentBrush") });
        var row = new Grid { ColumnSpacing = Design.Space.IconToText, Padding = new Thickness(Design.Space.RowPad, Design.Space.X3, Design.Space.RowPad, Design.Space.X3) };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) row.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        outer.Children.Add(row);
        card.Child = outer;
        g.Children.Add(card);
        void Put(FrameworkElement e, int col) { Grid.SetColumn(e, col); e.VerticalAlignment = VerticalAlignment.Center; row.Children.Add(e); }

        // the grip (a lock for what the launcher installed, an eye for what is not read), the place in the order, the mod
        var grip = Lucide.Icon(unread ? "eye-off" : m.Locked ? "lock" : "grip-vertical", Design.Size.IconSmall, Res("ApexMutedBrush"));
        if (unread || m.Locked) (grip.Tag, grip.Opacity) = ("fixed", 1); else grip.Opacity = c.FocusState != FocusState.Unfocused ? 1 : 0;
        if (m.Locked) ToolTipService.SetToolTip(grip, T("Installed by Apex Launcher, so it stays where it is"));
        card.Tag = grip;
        // (in the row's left margin: it shows under the pointer and moves nothing)
        (grip.HorizontalAlignment, grip.VerticalAlignment, grip.Margin, grip.IsHitTestVisible) = (HorizontalAlignment.Left, VerticalAlignment.Center, new Thickness(2, 0, 0, 0), false);
        outer.Children.Add(grip);
        var number = new TextBlock
        {
            Text = r.At > 0 ? r.At.ToString(LangCulture()) : "–", FontSize = Design.TextSize.Small, FontWeight = FontWeights.SemiBold,
            Foreground = Res(r.Moved ? "ApexOnAccentBrush" : unread ? "ApexMutedBrush" : "ApexSoftTextBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
        };
        Typography.SetNumeralAlignment(number, FontNumeralAlignment.Tabular);
        Put(new Border
        {
            Width = Design.Size.Tile, Height = Design.Size.Tile, CornerRadius = new CornerRadius(Design.Radius.Tile), Child = number,
            Background = Res(r.Moved ? "ApexAccentBrush" : unread ? "ApexClearBrush" : "ApexTileBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(unread ? 1 : 0),
        }, 0);
        var name = Path.GetFileNameWithoutExtension(m.Name);
        var words = new StackPanel { Spacing = 2 };
        words.Children.Add(new TextBlock { Text = name, FontSize = Design.TextSize.Body, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        words.Children.Add(new TextBlock { Text = ModRow.Place(m), FontSize = Design.TextSize.Small, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        ToolTipService.SetToolTip(words, m.Rel);
        Put(words, 1);

        var said = new List<string> { r.At > 0 ? r.At.ToString(LangCulture()) : T("Not read by the game"), name, ModRow.Place(m) };
        if (m.Locked) said.Add(T("Installed by Apex Launcher, so it stays where it is"));
        if (r.Moved) said.Add(T("Moved"));
        var end = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X2 };
        if (unread)
        {
            var read = Button(T("Make the game read it"), () => Safe(OrderLinesAsync(d => ModLoadOrder.MakeRead(d, m), T("The game reads {0} now", name))));
            (read.Height, read.MinHeight) = (Design.Size.ControlSmall, 0);
            AutomationProperties.SetName(read, T("Make the game read it") + ", " + name);
            end.Children.Add(read);
        }
        else if (_orderClash.TryGetValue(m.Path, out var clash))
        {
            // (the chip says how many, so the mod's own name keeps the room; the names are in its tooltip and the screen reader's)
            static string Count(List<string> names) => names.Distinct(StringComparer.OrdinalIgnoreCase).Count() is var n && n == 1 ? T("1 mod") : T("{0} mods", n);
            Border Named(Border chip, string full) { ToolTipService.SetToolTip(chip, full); return chip; }
            if (clash.Over.Count > 0) { var w = T("Wins over {0}", Others(clash.Over)); end.Children.Add(Named(Design.Parts.Chip("circle-check", T("Wins over {0}", Count(clash.Over)), tone: "Success"), w)); said.Add(w); }
            if (clash.Under.Count > 0) { var l = T("Loses to {0}", Others(clash.Under)); end.Children.Add(Named(Design.Parts.Chip("triangle-alert", T("Loses to {0}", Count(clash.Under)), warn: true), l)); said.Add(l); }
            var see = Design.Parts.Icon("chevron-right", T("See conflicts"));
            AutomationProperties.SetName(see, T("See conflicts") + ", " + name);
            see.Click += (_, _) => SeeConflicts(m);
            end.Children.Add(see);
        }
        Put(end, 2);
        AutomationProperties.SetName(c, string.Join(", ", said));
    }

    // a section's head: the folder in words, how many mods, and on the right its priority and its options
    void PaintOrderHead(Microsoft.UI.Xaml.Controls.Primitives.SelectorItem c, Grid g, OrderItem r)
    {
        AutomationProperties.SetHeadingLevel(c, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        g.Padding = new Thickness(0, r.At == 0 ? 0 : Design.Space.Section, 0, Design.Space.SectionToBody);
        foreach (var w in new[] { new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var words = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X3, VerticalAlignment = VerticalAlignment.Center };
        string title, note;
        if (r.Section < 0) (title, note) = (T("Not read by the game"), T("No line of Resource.cfg reads these folders"));
        else
        {
            var s = _order!.Secs[r.Section];
            var n = _orderWant.Count(w => w.Section == r.Section);
            (title, note) = (OrderWords(s), r.At == 0 ? ModCount(n) + " · " + T("read first") : ModCount(n));
            var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X2, VerticalAlignment = VerticalAlignment.Center };
            var prio = Button(T("Priority {0}", s.Priority), () => Safe(AskPriorityAsync(s)));
            (prio.Height, prio.MinHeight, prio.Padding, prio.FontSize) = (Design.Size.ControlSmall, 0, new Thickness(Design.Space.X3, 0, Design.Space.X3, 0), Design.TextSize.Small);
            AutomationProperties.SetName(prio, T("Change priority") + ", " + T("Priority {0}", s.Priority));
            acts.Children.Add(prio);
            acts.Children.Add(OrderMore(s));
            Grid.SetColumn(acts, 1);
            g.Children.Add(acts);
        }
        words.Children.Add(new TextBlock { Text = title, FontSize = Design.TextSize.Lead, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
        words.Children.Add(new TextBlock { Text = note, FontSize = Design.TextSize.Small, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
        g.Children.Add(words);
        AutomationProperties.SetName(c, $"{title}, {note}");
    }

    // the mod alone in the list, chosen, its details open on Conflicts (where Give priority is)
    void SeeConflicts(ModFile m)
    {
        CloseOrder();
        (_detailsFor, _detailsTab) = (m.Rel, "conflicts");
        ShowInMods([m]);
    }

    // ---------- how the game reads ----------

    Border OrderPanel()
    {
        var s = new StackPanel();
        var head = Design.Parts.Title(Design.Parts.Heading(T("How the game reads")), T("The lines of Resource.cfg, from the one that counts most"));
        s.Children.Add(new Border { Child = head, Padding = new Thickness(Design.Space.CardPad, Design.Space.CardPad, Design.Space.CardPad, Design.Space.X4), BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, 0, 0, 1) });
        _orderRules = new StackPanel();
        s.Children.Add(_orderRules);
        var how = new StackPanel { Spacing = Design.Space.X3, Padding = new Thickness(Design.Space.CardPad, Design.Space.X4, Design.Space.CardPad, Design.Space.X4), BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        foreach (var (line, i) in new[]
        {
            T("The folder with the higher number is read first, and what it changes wins"),
            T("At the same priority, the line earlier in the file wins"),
            T("Inside a folder the game reads in alphabetical order, and dragging changes that for you"),
        }.Select((l, i) => (l, i)))
        {
            var g = new Grid { ColumnSpacing = Design.Space.X3 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.Children.Add(new Border
            {
                Width = Design.Space.X6, Height = Design.Space.X6, CornerRadius = new CornerRadius(Design.Radius.Card), Background = Res("ApexTileBrush"), VerticalAlignment = VerticalAlignment.Top,
                Child = new TextBlock { Text = (i + 1).ToString(LangCulture()), FontSize = Design.TextSize.Caption, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexSoftTextBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center },
            });
            var t = new TextBlock { Text = line, FontSize = Design.TextSize.Small, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(t, 1);
            g.Children.Add(t);
            how.Children.Add(g);
        }
        s.Children.Add(how);
        var foot = new StackPanel { Spacing = Design.Space.X2, Padding = new Thickness(Design.Space.CardPad, Design.Space.X3, Design.Space.CardPad, Design.Space.CardPad), BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        foreach (var (icon, text, go) in new (string, string, Action)[] { ("folder-plus", T("New priority folder"), () => Safe(NewPriorityFolderAsync())), ("file-code", T("Open Resource.cfg"), OpenResourceCfg) })
        {
            var b = Button("", go);
            (b.Content, b.HorizontalAlignment, b.Height) = (Icon(icon, text), HorizontalAlignment.Stretch, Design.Size.Control);
            AutomationProperties.SetName(b, text);
            foot.Children.Add(b);
        }
        s.Children.Add(foot);
        var panel = Design.Parts.PanelOf(new ScrollViewer { Content = s, VerticalScrollBarVisibility = ScrollBarVisibility.Auto }, new Thickness(0));
        panel.VerticalAlignment = VerticalAlignment.Top;
        // (beside the list it is no taller than the page, less the room the floating bar takes, and scrolls inside)
        _orderBody.SizeChanged += (_, e) => { if (_orderUnder == false) panel.MaxHeight = Math.Max(240, e.NewSize.Height - Design.Space.X16 - Design.Space.X8); };
        return panel;
    }

    // each .package line as a row: its priority typed over in place, the folder in words, how many mods and how deep it reads
    void FillRules()
    {
        if (_orderPage is null || _order is null) return;
        var sig = string.Join("|", _order.Secs.Select(s => s.Text + s.Priority)) + "|" + string.Join(",", _orderWant.Select(w => w.Section));
        if (_orderRules.Tag as string == sig) return;
        _orderRules.Tag = sig;
        _orderRules.Children.Clear();
        _orderRuleRows.Clear();
        var reading = _orderWant.Select(w => w.Section).ToHashSet();
        foreach (var s in _order.Secs.Where(s => s.Text.TrimEnd().EndsWith(".package", StringComparison.OrdinalIgnoreCase) && reading.Contains(s.Index)).OrderBy(s => -s.Priority).ThenBy(s => s.Index))
        {
            var g = new Grid { ColumnSpacing = Design.Space.X3 };
            foreach (var w in new[] { new GridLength(Design.Space.X12 + Design.Space.X2), new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
            var field = Design.Parts.QuietField(new TextBox { Text = s.Priority.ToString(), InputScope = Digits(), TextAlignment = TextAlignment.Center, VerticalAlignment = VerticalAlignment.Center }, editable: true);
            field.Background = Res("ApexTileBrush");
            field.FontWeight = FontWeights.SemiBold;
            AutomationProperties.SetName(field, T("Priority of {0}", OrderWords(s)));
            ToolTipService.SetToolTip(field, T("Type a new priority"));
            var section = s;
            void Save()
            {
                if (int.TryParse(field.Text.Trim(), out var p) && p != section.Priority) Safe(OrderPriorityAsync(section.Index, p));
                else field.Text = section.Priority.ToString();
            }
            field.KeyDown += (_, e) => { if (e.Key == VirtualKey.Enter) { e.Handled = true; Save(); } else if (e.Key == VirtualKey.Escape) { e.Handled = true; field.Text = section.Priority.ToString(); } };
            field.LostFocus += (_, _) => Save();
            g.Children.Add(field);
            var n = _orderWant.Count(w => w.Section == s.Index);
            var depth = s.Depth switch { 0 => T("Only the folder"), 1 => T("One level down"), var d => T("{0} levels down", d) };
            var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            words.Children.Add(new TextBlock { Text = s.Folder.Length == 0 ? T("Mods") : s.Folder.Replace("/", " › "), FontSize = Design.TextSize.Small, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
            words.Children.Add(new TextBlock { Text = ModCount(n) + " · " + depth, FontSize = Design.TextSize.Caption, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            ToolTipService.SetToolTip(words, "PackedFile " + s.Text);
            Grid.SetColumn(words, 1);
            g.Children.Add(words);
            var more = OrderMore(s);
            Grid.SetColumn(more, 2);
            g.Children.Add(more);
            var row = new Border { Child = g, Padding = new Thickness(Design.Space.CardPad, Design.Space.X3, Design.Space.CardPad, Design.Space.X3), BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, _orderRules.Children.Count == 0 ? 0 : 1, 0, 0) };
            _orderRuleRows[s.Index] = row;
            _orderRules.Children.Add(row);
        }
        LightRule(_orderLit);
    }

    // the line that reads the section under the pointer
    void LightRule(int section)
    {
        _orderLit = section;
        foreach (var (i, row) in _orderRuleRows) row.Background = Res(i == section ? "ApexAccentSoftBrush" : "ApexClearBrush");
    }
}
