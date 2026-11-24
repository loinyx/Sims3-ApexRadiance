using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Two tabs of a mod's details, also for a package kept inside a merge. Content: the items it holds with their pictures, or
// every resource with its key. Conflicts: the mods that hold some of the same game data, and which one counts.
public sealed partial class MainWindow
{
    // ---------- Content ----------

    bool _contentKeys;              // the resource list instead of the items
    string _contentSearch = "";
    string? _contentFor;
    (string Path, long Size, DateTime Written, string Language, GameIndex? Game, PackageContent Content)? _contentRead; // the last one read
    readonly Dictionary<string, List<string>> _contentNames = new(StringComparer.OrdinalIgnoreCase); // the items' names of the ones read, for the search

    UIElement ContentTab(string path)
    {
        if (!path.Equals(_contentFor, StringComparison.OrdinalIgnoreCase)) (_contentFor, _contentSearch) = (path, "");
        var panel = new StackPanel { Spacing = 14 };
        // (another mod picked, or another tab: what is still being read for this one stops)
        var cts = new CancellationTokenSource();
        panel.Unloaded += (_, _) => cts.Cancel();
        var file = new FileInfo(path);
        var (size, written, language, game) = (file.Exists ? file.Length : 0, file.Exists ? file.LastWriteTimeUtc : default, Lang, _gameIndex);
        if (_contentRead is { } r && r.Path.Equals(path, StringComparison.OrdinalIgnoreCase) && r.Size == size && r.Written == written && r.Language == language && ReferenceEquals(r.Game, game))
            FillContent(panel, path, r.Content, cts.Token);
        else
        {
            panel.Children.Add(WaitLine(T("Reading what it holds…")));
            Safe(Read());
        }
        return panel;

        async Task Read()
        {
            PackageContent c;
            try { c = await Task.Run(() => ModInspect.Read(path, language, game, cts.Token), cts.Token); }
            catch (OperationCanceledException) { return; }
            catch (Exception e) { App.Log(e); c = PackageContent.Empty; } // (a damaged package says it can't be read, never an error box)
            if (cts.IsCancellationRequested) return;
            _contentRead = (path, size, written, language, game, c);
            _contentNames[path] = c.Items.Select(i => i.Name).OfType<string>().Distinct().ToList();
            panel.Children.Clear();
            FillContent(panel, path, c, cts.Token);
        }
    }

    void FillContent(StackPanel panel, string path, PackageContent c, CancellationToken ct)
    {
        if (c.Protected) { panel.Children.Add(Note("lock", "ApexMutedBrush", T("This package is protected, so its content can't be shown."))); return; }
        if (c.Resources.Count == 0) { panel.Children.Add(Muted(T("The launcher couldn't read this package."))); return; }
        if (c.Parts.Count > 0)
        {
            var wrap = new WrapPanel { Spacing = 6 };
            foreach (var (kind, n) in c.Parts) wrap.Children.Add(Pill(PartsText(kind, n), "ApexSoftTextBrush"));
            panel.Children.Add(wrap);
        }
        else
            panel.Children.Add(Muted(c.Resources.Any(r => r.Entry.Type == ModKinds.S3sa) ? T("Game code and settings: no objects or Create a Sim parts.")
                : T("No objects or Create a Sim parts: it changes what the game already has.")));

        // (nothing but resources: the list alone, no switch)
        var keys = _contentKeys || c.Items.Count == 0;
        if (c.Items.Count > 0)
            panel.Children.Add(ContentSwitch((T("Items {0}", c.Items.Count), !keys), (T("Resources {0}", c.Resources.Count), keys), at => { _contentKeys = at == 1; AgainContent(panel, path, c, ct); }));

        var search = new TextBox { Text = _contentSearch, PlaceholderText = keys ? T("Search by name, type or key") : T("Search by name") };
        AutomationProperties.SetName(search, search.PlaceholderText);
        var tools = new Grid { ColumnSpacing = 4 };
        foreach (var w in new[] { new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) tools.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        tools.Children.Add(search);
        panel.Children.Add(tools);
        var count = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap };
        AutomationProperties.SetLiveSetting(count, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        panel.Children.Add(count);
        ListViewBase list = keys ? ResourceList() : ItemGrid(path, ct);
        panel.Children.Add(list);
        FitInPane(list);

        IReadOnlyList<ContentResource> listed = c.Resources;
        void Filter()
        {
            var q = _contentSearch.Trim();
            int shown, all;
            if (keys)
            {
                var found = q.Length == 0 ? c.Resources : c.Resources.Where(r => Has(r.Name, q) || Has(r.Tag, q) || Has(r.Key, q) || Has(DataLabel(r), q)).ToList();
                (list.ItemsSource, listed, shown, all) = (found, found, found.Count, c.Resources.Count);
            }
            else
            {
                var found = q.Length == 0 ? c.Items : c.Items.Where(i => Has(i.Name, q) || Has(ItemLabel(i), q)).ToList();
                (list.ItemsSource, shown, all) = (found, found.Count, c.Items.Count);
            }
            count.Text = shown == 0 ? T("Nothing found") : T("{0} of {1}", shown, all);
            count.Visibility = q.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
        }
        static bool Has(string? text, string q) => text is not null && text.Contains(q, StringComparison.OrdinalIgnoreCase);
        search.TextChanged += (_, _) => { _contentSearch = search.Text; Filter(); };
        Filter();

        if (list is GridView grid)
            // an item's own resources: the list on its instance
            grid.ItemClick += (_, e) =>
            {
                if (e.ClickedItem is not ContentItem i) return;
                (_contentKeys, _contentSearch) = (true, i.Entry.Instance.ToString("X16"));
                DispatcherQueue.TryEnqueue(() => AgainContent(panel, path, c, ct)); // (later: it removes this grid)
            };
        else
        {
            // (the chosen rows, else every one the list shows)
            IReadOnlyList<ContentResource> Chosen() => list.SelectedItems.Count > 0 ? list.SelectedItems.OfType<ContentResource>().ToList() : listed;
            void Copy() => CopyText(string.Join(Environment.NewLine, Chosen().Select(r => r.Key)));
            var copy = IconButton("copy", T("Copy keys"));
            copy.Click += (_, _) => Copy();
            var save = IconButton("file-down", T("Save the list as text"));
            save.Click += (_, _) => Safe(SaveKeysAsync(path, Chosen()));
            foreach (var (b, at) in new[] { (copy, 1), (save, 2) }) { Grid.SetColumn(b, at); tools.Children.Add(b); }
            list.KeyDown += (_, e) =>
            {
                if (e.Key != Windows.System.VirtualKey.C || !Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(Windows.System.VirtualKey.Control).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down)) return;
                e.Handled = true;
                Copy();
            };
            // (the row under the pointer, when it is not one of the chosen)
            list.RightTapped += (_, e) =>
            {
                if ((e.OriginalSource as FrameworkElement)?.DataContext is ContentResource r && !list.SelectedItems.Contains(r)) { list.SelectedItems.Clear(); list.SelectedItems.Add(r); }
            };
            var menu = new MenuFlyout();
            menu.Items.Add(MenuItem(T("Copy keys"), Copy, "copy"));
            menu.Items.Add(MenuItem(T("Save the list as text"), () => Safe(SaveKeysAsync(path, Chosen())), "file-down"));
            list.ContextFlyout = menu;
        }
    }

    // the same content again, in another view
    void AgainContent(StackPanel panel, string path, PackageContent c, CancellationToken ct)
    {
        panel.Children.Clear();
        FillContent(panel, path, c, ct);
        // (the keyboard stays on the switch, which is a new one)
        if (panel.Children.OfType<Border>().FirstOrDefault()?.Child is Grid g && g.Children.ElementAtOrDefault(_contentKeys ? 1 : 0) is Button on) on.Loaded += (_, _) => on.Focus(FocusState.Programmatic);
    }

    async Task SaveKeysAsync(string path, IReadOnlyList<ContentResource> resources)
    {
        try
        {
            var picker = new Microsoft.Windows.Storage.Pickers.FileSavePicker(AppWindow.Id) { SuggestedFileName = Path.GetFileNameWithoutExtension(path) };
            picker.FileTypeChoices.Add("TXT", [".txt"]);
            if (await picker.PickSaveFileAsync() is not { } file) return;
            var text = ModInspect.Export(resources);
            await Task.Run(() => File.WriteAllText(file.Path, text));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The list was not saved"), ErrorText(e)); }
    }

    // two choices side by side, one on
    static Border ContentSwitch((string Text, bool On) a, (string Text, bool On) b, Action<int> pick)
    {
        var g = new Grid { ColumnSpacing = 4 };
        foreach (var ((text, on), i) in new[] { (a, 0), (b, 1) })
        {
            g.ColumnDefinitions.Add(new ColumnDefinition());
            var x = new Button { Content = new TextBlock { Text = text, FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis, Foreground = Res(on ? "TextFillColorPrimaryBrush" : "ApexMutedBrush") },
                Height = 34, Padding = new Thickness(8, 0, 8, 0), CornerRadius = new CornerRadius(6), BorderThickness = new Thickness(0), HorizontalAlignment = HorizontalAlignment.Stretch,
                Background = new SolidColorBrush(on ? Pick(0x1FFFFFFF, 0x14000000) : Microsoft.UI.Colors.Transparent) };
            AutomationProperties.SetName(x, text);
            AutomationProperties.SetItemStatus(x, on ? T("Selected") : "");
            if (!on) x.Click += (_, _) => pick(i);
            Grid.SetColumn(x, i);
            g.Children.Add(x);
        }
        return new Border { Child = g, Padding = new Thickness(4), CornerRadius = new CornerRadius(9), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) };
    }

    static StackPanel WaitLine(string text)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        s.Children.Add(new ProgressRing { IsActive = true, Width = 16, Height = 16 });
        s.Children.Add(Muted(text));
        return s;
    }

    // a list in the pane's scroller takes the room left in it and scrolls on its own: only the rows in view are made
    static void FitInPane(ListViewBase list)
    {
        list.MaxHeight = 320;
        ScrollViewer? pane = null;
        // (24: the pane's own foot; the rest for the count a search shows)
        void Fit()
        {
            if (pane?.Content is not UIElement content || pane.ViewportHeight <= 0) return;
            var top = list.TransformToVisual(content).TransformPoint(new Windows.Foundation.Point(0, 0)).Y;
            list.MaxHeight = Math.Max(240, pane.ViewportHeight - top - 24 - 32);
        }
        void Sized(object s, SizeChangedEventArgs e) => Fit();
        list.Loaded += (_, _) =>
        {
            if (pane is not null) return;
            for (DependencyObject? p = VisualTreeHelper.GetParent(list); p is not null && pane is null; p = VisualTreeHelper.GetParent(p)) pane = p as ScrollViewer;
            if (pane is null) return;
            pane.SizeChanged += Sized;
            Fit();
        };
        list.Unloaded += (_, _) => { if (pane is not null) pane.SizeChanged -= Sized; pane = null; };
    }

    static DataTemplate EmptyCell() => (DataTemplate)XamlReader.Load("<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"><Grid/></DataTemplate>");

    // ---------- the items ----------

    sealed record ItemTile(Image Picture, LucideIcon Icon, TextBlock Name) { public ContentItem? Item; }

    GridView ItemGrid(string path, CancellationToken ct)
    {
        var grid = new GridView { SelectionMode = ListViewSelectionMode.None, IsItemClickEnabled = true, ItemTemplate = EmptyCell(), Padding = new Thickness(0),
            ItemsPanel = (ItemsPanelTemplate)XamlReader.Load("<ItemsPanelTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"><ItemsWrapGrid Orientation=\"Horizontal\" ItemWidth=\"98\" ItemHeight=\"132\"/></ItemsPanelTemplate>") };
        AutomationProperties.SetName(grid, T("Items"));
        // (a few pictures at a time, the ones in view when their turn comes)
        var turn = new SemaphoreSlim(3);
        // (as many columns as fit, sharing the width)
        void Fit()
        {
            if (grid.ItemsPanelRoot is ItemsWrapGrid panel && grid.ActualWidth > 60) panel.ItemWidth = Math.Floor((grid.ActualWidth - 1) / Math.Max(2, (int)(grid.ActualWidth / 96)));
        }
        grid.SizeChanged += (_, _) => Fit();
        var fitted = false;
        grid.ContainerContentChanging += (s, a) =>
        {
            if (a.ItemContainer.ContentTemplateRoot is not Grid g) return;
            if (a.InRecycleQueue) { if (g.Tag is ItemTile old) (old.Item, old.Picture.Source) = (null, null); return; }
            if (a.Item is not ContentItem item) return;
            if (g.Tag is not ItemTile t) g.Tag = t = MakeTile(g);
            if (!fitted) fitted = grid.DispatcherQueue.TryEnqueue(Fit); // (the panel exists from here on)
            (a.ItemContainer.HorizontalContentAlignment, a.ItemContainer.VerticalContentAlignment) = (HorizontalAlignment.Stretch, VerticalAlignment.Stretch);
            (a.ItemContainer.Margin, a.ItemContainer.Padding) = (new Thickness(0, 0, 8, 8), new Thickness(0));
            var kind = ItemLabel(item);
            t.Item = item;
            t.Name.Text = item.Name ?? kind;
            t.Icon.Kind = item.Kind is { } k ? KindIcon(k.ToString()) : "image";
            t.Icon.Visibility = Visibility.Visible;
            t.Picture.Source = null;
            AutomationProperties.SetName(a.ItemContainer, item.Name is { } n ? $"{n}, {kind}" : kind);
            ToolTipService.SetToolTip(g, (item.Name is { } m ? m + "\n" : "") + kind + "\n" + T("Show its resources"));
            if (item.Picture is { } e) _ = Load(t, item, e);
            a.Handled = true;
        };
        return grid;

        async Task Load(ItemTile t, ContentItem item, Dbpf.Entry e)
        {
            try
            {
                byte[]? bytes;
                await turn.WaitAsync(ct);
                try
                {
                    if (!ReferenceEquals(t.Item, item)) return; // (scrolled away)
                    bytes = await Task.Run(() => Dbpf.Read(path, e, 8 << 20), ct);
                }
                finally { turn.Release(); }
                if (!ReferenceEquals(t.Item, item) || await SaveCard.ImageOf(bytes, 160) is not { } image || !ReferenceEquals(t.Item, item)) return;
                t.Picture.Source = image;
                t.Icon.Visibility = Visibility.Collapsed;
            }
            catch (OperationCanceledException) { }
            catch (Exception x) { App.Log(x); } // (the icon stays)
        }
    }

    static ItemTile MakeTile(Grid g)
    {
        var icon = new LucideIcon { Size = 26, Stroke = Res("ApexMutedBrush"), StrokeWidth = 1.5, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        var picture = new Image { Stretch = Stretch.Uniform, Margin = new Thickness(6) };
        var top = new Grid { Background = Res("ApexPillBrush") };
        top.Children.Add(icon);
        top.Children.Add(picture);
        var name = new TextBlock { FontSize = 12, Margin = new Thickness(8, 0, 8, 2), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        var rows = new Grid();
        rows.RowDefinitions.Add(new RowDefinition());
        rows.RowDefinitions.Add(new RowDefinition { Height = new GridLength(32) });
        rows.Children.Add(top);
        Grid.SetRow(name, 1);
        rows.Children.Add(name);
        var frame = new Border { Child = rows, CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) };
        g.Children.Add(frame);
        return new(picture, icon, name);
    }

    static string ItemLabel(ContentItem i) => i.Kind is { } k ? KindLabel(k.ToString()) : T("Picture");

    // ---------- the resources ----------

    sealed record KeyRow(TextBlock Tag, TextBlock Name, TextBlock Key, TextBlock Size);

    static ListView ResourceList()
    {
        var list = new ListView { SelectionMode = ListViewSelectionMode.Extended, ItemTemplate = EmptyCell(), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), Padding = new Thickness(0) };
        AutomationProperties.SetName(list, T("Resources"));
        list.ContainerContentChanging += (_, a) =>
        {
            if (a.InRecycleQueue || a.Item is not ContentResource r || a.ItemContainer.ContentTemplateRoot is not Grid g) return;
            if (g.Tag is not KeyRow t) g.Tag = t = MakeRow(g);
            a.ItemContainer.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            a.ItemContainer.Padding = new Thickness(12, 0, 12, 0);
            var name = r.Name ?? DataLabel(r);
            (t.Tag.Text, t.Name.Text, t.Key.Text, t.Size.Text) = (r.Tag, name, r.Key, ModRow.SizeText(r.Entry.Size));
            AutomationProperties.SetName(a.ItemContainer, $"{r.Tag}, {name}, {r.Key}, {t.Size.Text}");
            ToolTipService.SetToolTip(g, (r.Name is null ? "" : r.Name + "\n") + DataLabel(r) + "\n" + r.Key);
            a.Handled = true;
        };
        return list;
    }

    static KeyRow MakeRow(Grid g)
    {
        (g.ColumnSpacing, g.MinHeight) = (12, 50);
        foreach (var w in new[] { new GridLength(52), new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var mono = new FontFamily("Consolas");
        var tag = new TextBlock { FontFamily = mono, FontSize = 12, Foreground = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        var name = new TextBlock { FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis };
        var key = new TextBlock { FontFamily = mono, FontSize = 11, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis };
        var size = new TextBlock { FontSize = 11, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
        var words = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(name);
        words.Children.Add(key);
        Grid.SetColumn(words, 1);
        Grid.SetColumn(size, 2);
        foreach (var x in new FrameworkElement[] { tag, words, size }) g.Children.Add(x);
        return new(tag, name, key, size);
    }

    // one resource's type in words
    static string DataLabel(ContentResource r) => ModInspect.KindOf(r.Entry.Type) switch
    {
        DataKind.Text when ModInspect.TableLanguage(r.Entry.Instance) is { } code && Languages.FirstOrDefault(l => l.Code == code).Name is { } language => T("Text · {0}", language),
        DataKind.Tuning => T("Tuning"), DataKind.Script => T("Script"), DataKind.Object => T("Object"), DataKind.CasPart => T("Create a Sim"), DataKind.Texture => T("Texture"),
        DataKind.Picture => T("Picture"), DataKind.Text => T("Text"), DataKind.Model => T("Model"), DataKind.Animation => T("Animation"), DataKind.Sound => T("Sound"),
        DataKind.Effect => T("Effect"), DataKind.Light => T("Light"), DataKind.Interface => T("Interface"),
        _ => T("Resource"),
    };

    // ---------- Conflicts ----------

    Task<SharedData>? _shared;       // of the mods as they are now; read once, in the background
    (List<ModFile> Mods, int Now) _sharedFor;
    double _sharedDone;
    ProgressBar? _sharedBar;         // the open tab's
    readonly Dictionary<(string Mod, string? Inside), List<SharedMod>> _sharedOf = new(); // the answers, until the mods change

    // which mods hold the same data, read again when a mod was added, moved, switched or removed
    Task<SharedData> SharedNow()
    {
        var now = new HashCode();
        foreach (var m in _mods) { now.Add(m.Path, StringComparer.OrdinalIgnoreCase); now.Add(m.Size); now.Add(m.Enabled); }
        var mark = (_mods, now.ToHashCode());
        if (_shared is null || _shared.IsFaulted || !ReferenceEquals(_sharedFor.Mods, mark._mods) || _sharedFor.Now != mark.Item2)
        {
            _sharedFor = mark;
            _sharedOf.Clear();
            _sharedDone = 0;
            var (docs, mods) = (Docs, _mods.ToList());
            var progress = new Progress<double>(x => { _sharedDone = x; if (_sharedBar is { } b) b.Value = x; });
            _shared = Task.Run(() => new SharedData(docs, mods, progress));
        }
        return _shared;
    }

    async Task<List<SharedMod>> SharedOf(ModFile m, string? inside, CancellationToken ct)
    {
        var shared = await SharedNow();
        ct.ThrowIfCancellationRequested();
        if (_sharedOf.TryGetValue((m.Path, inside), out var known)) return known;
        var rows = await Task.Run(() => shared.Of(m, inside, ct), ct);
        if (ReferenceEquals(_shared?.IsCompletedSuccessfully == true ? _shared.Result : null, shared)) _sharedOf[(m.Path, inside)] = rows;
        return rows;
    }

    // how many mods it conflicts with, as Health counts: not the ignored ones, nor the ones a priority settled
    int OpenConflicts(ModFile m, List<SharedMod> rows)
    {
        bool Open(ModConflict? c) => c is not null && !_settings.IgnoredConflicts.Contains(c.Id);
        var found = _conflicts?.Found ?? [];
        return rows.Count(r => Open(ModInspect.InHealth(found, m, r))) + ModInspect.OnlyInHealth(found, m, _mods).Count(Open);
    }

    // the count on the tab's label, when it is known (a moment after the mod is picked, so going through the list reads nothing)
    async Task CountConflictsAsync(ModFile m, SelectorBar bar, SelectorBarItem tab)
    {
        var cts = new CancellationTokenSource();
        bar.Unloaded += (_, _) => cts.Cancel();
        try
        {
            if (!_sharedOf.ContainsKey((m.Path, null))) await Task.Delay(300, cts.Token);
            var rows = await SharedOf(m, null, cts.Token);
            await HealthReady();
            if (!cts.IsCancellationRequested && OpenConflicts(m, rows) is > 0 and var n) tab.Text = T("Conflicts ({0})", n);
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { App.Log(e); } // (the tab keeps its plain name)
    }

    // m: a mod the game loads; inside: the package kept in it, when m is a merge
    UIElement ConflictsTab(ModFile m, string? inside = null)
    {
        var panel = new StackPanel { Spacing = 22 };
        var cts = new CancellationTokenSource();
        panel.Unloaded += (_, _) => cts.Cancel();
        if (!m.Enabled) { panel.Children.Add(Muted(T("This mod is off, so it changes nothing in the game."))); return panel; }
        Safe(Fill());
        return panel;

        async Task Fill()
        {
            var wait = new StackPanel { Spacing = 10 };
            wait.Children.Add(Muted(T("Comparing with your other mods…")));
            var bar = new ProgressBar { Maximum = 1, Value = _sharedDone };
            AutomationProperties.SetName(bar, T("Comparing with your other mods…"));
            wait.Children.Add(bar);
            panel.Children.Add(wait);
            try
            {
                _sharedBar = bar;
                var rows = await SharedOf(m, inside, cts.Token);
                // (what decides the priority of each: the folders of Resource.cfg)
                var docs = Docs;
                var moves = await Task.Run(() => rows.Where(r => r is { Conflict: true, Wins: false }).ToDictionary(r => r, r => ModPriority.Moves(docs, m, [r.Mod])), cts.Token);
                // a script's conflicts are found by the launcher's own check of every mod; what is inside a merge and holds no script has none
                // shortcut: two script mods in one merge both show the merge's script conflicts, until the check names the assembly
                var scripts = inside is null || await Task.Run(() => Dbpf.Index(inside).Any(e => e.Type == ModKinds.S3sa), cts.Token);
                void Show()
                {
                    if (cts.IsCancellationRequested) return;
                    panel.Children.Clear();
                    FillConflicts(panel, m, inside, rows, moves, scripts);
                }
                Show();
                // (Health's check still runs: what it says of each comes when it ends)
                if (_healthChecking) { await HealthReady(); Show(); }
            }
            catch (OperationCanceledException) { }
            catch (Exception e)
            {
                App.Log(e);
                panel.Children.Clear();
                panel.Children.Add(Warn(T("The mods couldn't be compared"), ErrorText(e)));
            }
            finally { if (ReferenceEquals(_sharedBar, bar)) _sharedBar = null; }
        }
    }

    void FillConflicts(StackPanel panel, ModFile m, string? inside, List<SharedMod> rows, Dictionary<SharedMod, List<(ModFile Mod, string Folder)>?> moves, bool scripts)
    {
        var found = _conflicts?.Found ?? [];
        var name = Path.GetFileNameWithoutExtension(inside ?? m.Path);
        if (_healthChecking) panel.Children.Add(WaitLine(T("Checking your mods…")));
        bool Ignored(ModConflict? c) => c is not null && _settings.IgnoredConflicts.Contains(c.Id);
        void Part(string title, string brush, IEnumerable<UIElement> list)
        {
            var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            head.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Res(brush), VerticalAlignment = VerticalAlignment.Center });
            var text = new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
            AutomationProperties.SetHeadingLevel(text, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level3);
            head.Children.Add(text);
            var s = new StackPanel { Spacing = 8 };
            s.Children.Add(head);
            var box = new StackPanel();
            foreach (var r in list) box.Children.Add(new Border { Child = r, BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, box.Children.Count == 0 ? 0 : 1, 0, 0) });
            s.Children.Add(new Border { Child = box, CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) });
            panel.Children.Add(s);
        }
        UIElement Row(string title, string what, ModConflict? health, params UIElement[] acts)
        {
            var s = new StackPanel { Spacing = 4, Padding = new Thickness(14, 12, 14, 12) };
            var t = new TextBlock { Text = title, FontSize = 13, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
            s.Children.Add(t);
            s.Children.Add(new TextBlock { Text = Ignored(health) ? what + " · " + T("Ignored in Health") : what, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            var links = new WrapPanel { Spacing = 14 };
            foreach (var a in acts) links.Children.Add(a);
            if (health is not null)
            {
                var see = Link(T("See in Health"), () => SeeInHealth(health));
                AutomationProperties.SetName(see, T("See in Health") + ", " + title);
                links.Children.Add(see);
            }
            if (links.Children.Count > 0) s.Children.Add(links);
            return s;
        }
        UIElement ModLine(SharedMod r, params UIElement[] acts)
        {
            var what = string.Join(", ", r.Different.Select(d => DataText(d.Kind, d.Count)));
            if (r.Identical > 0 && r.Conflict) what += " · " + (r.Identical == 1 ? T("1 more is the same in both") : T("{0} more are the same in both", r.Identical));
            else if (!r.Conflict) what = r.Identical == 1 ? T("1 resource") : T("{0} resources", r.Identical);
            return Row(Path.GetFileNameWithoutExtension(r.Mod.Name), what, ModInspect.InHealth(found, m, r), acts);
        }

        var over = rows.Where(r => r is { Conflict: true, Wins: true }).ToList();
        var under = rows.Where(r => r is { Conflict: true, Wins: false }).ToList();
        var same = rows.Where(r => !r.Conflict).ToList();
        var other = scripts ? ModInspect.OnlyInHealth(found, m, _mods) : [];
        if (over.Count > 0) Part(over.Count == 1 ? T("Replaces 1 mod") : T("Replaces {0} mods", over.Count), "ApexSuccessBrush", over.Select(r => ModLine(r)));
        var up = false;
        if (under.Count > 0)
            Part(under.Count == 1 ? T("Replaced by 1 mod") : T("Replaced by {0} mods", under.Count), "ApexWarningBrush", under.Select(r =>
            {
                if (moves.GetValueOrDefault(r) is not { Count: > 0 } go) return ModLine(r);
                up = true;
                var give = Link(T("Give priority"), () => Safe(PriorityHereAsync(m, name, go)));
                give.IsEnabled = !_running && !_busy;
                AutomationProperties.SetName(give, T("Give priority") + ", " + Path.GetFileNameWithoutExtension(r.Mod.Name));
                ToolTipService.SetToolTip(give, string.Join("\n", go.Select(x => T("Moves {0} to {1}", Path.GetFileNameWithoutExtension(x.Mod.Name), "Mods\\" + x.Folder))));
                return ModLine(r, give);
            }).ToList());
        if (other.Count > 0)
            Part(T("Other conflicts"), "ApexWarningBrush", other.Select(c => Row(string.Join(", ", c.Parties.Where(p => !p.Equals(Path.GetFileNameWithoutExtension(m.Path), StringComparison.OrdinalIgnoreCase))), T(c.Title) + ". " + T(c.Say, c.SayArgs), c)));
        if (same.Count > 0) Part(same.Count == 1 ? T("The same data in 1 mod") : T("The same data in {0} mods", same.Count), "ApexMutedBrush", same.Select(r => ModLine(r)));
        if (over.Count + under.Count + other.Count + same.Count == 0)
        {
            if (!_healthChecking) panel.Children.Add(Empty(T("No conflicts"), T("It shares no game data with your other mods.")));
            return;
        }
        if (up) panel.Children.Add(new TextBlock { Text = T("With priority, the file goes to the folder the game reads first. You can undo it."), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
    }

    // "3 tuning files", "1 texture"
    static string DataText(DataKind kind, int n) => (kind, n == 1) switch
    {
        (DataKind.Tuning, true) => T("1 tuning file"), (DataKind.Tuning, _) => T("{0} tuning files", n),
        (DataKind.Script, true) => T("1 script"), (DataKind.Script, _) => T("{0} scripts", n),
        (DataKind.Object, true) => T("1 object"), (DataKind.Object, _) => T("{0} objects", n),
        (DataKind.CasPart, true) => T("1 Create a Sim part"), (DataKind.CasPart, _) => T("{0} Create a Sim parts", n),
        (DataKind.Texture, true) => T("1 texture"), (DataKind.Texture, _) => T("{0} textures", n),
        (DataKind.Picture, true) => T("1 image"), (DataKind.Picture, _) => T("{0} images", n),
        (DataKind.Text, true) => T("1 text"), (DataKind.Text, _) => T("{0} texts", n),
        (DataKind.Model, true) => T("1 model"), (DataKind.Model, _) => T("{0} models", n),
        (DataKind.Animation, true) => T("1 animation"), (DataKind.Animation, _) => T("{0} animations", n),
        (DataKind.Sound, true) => T("1 sound"), (DataKind.Sound, _) => T("{0} sounds", n),
        (DataKind.Effect, true) => T("1 effect"), (DataKind.Effect, _) => T("{0} effects", n),
        (DataKind.Light, true) => T("1 light"), (DataKind.Light, _) => T("{0} lights", n),
        (DataKind.Interface, true) => T("1 interface file"), (DataKind.Interface, _) => T("{0} interface files", n),
        (_, true) => T("1 resource"), _ => T("{0} resources", n),
    };

    // Health's page of mod conflicts, open on this one
    void SeeInHealth(ModConflict c)
    {
        Show(HealthScreen);
        HealthGo(nameof(HealthArea.Mods));
        (_conflictTab, _conflictOpen) = (_settings.IgnoredConflicts.Contains(c.Id) ? 2 : c.Level == ConflictLevel.Small ? 1 : 0, c.Id);
        BuildHealth();
    }

    // this mod (or the merge it is in) first: the move Health's Give priority makes, with its Undo on the Mods bar
    async Task PriorityHereAsync(ModFile m, string name, List<(ModFile Mod, string Folder)> moves)
    {
        if (_running || _busy) return; // (a change started since the button showed)
        var docs = Docs;
        List<(ModFile Mod, string From)>? done = null;
        try { done = await ModWork(() => ModPriority.Apply(docs, moves)); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The mod was not moved"), ErrorText(e)); }
        if (done is not null) ModsResult(T("{0} has priority now.", name), () => Safe(Back(done)));
        var at = moves.FindIndex(x => x.Mod.Path.Equals(m.Path, StringComparison.OrdinalIgnoreCase));
        await ShowAgain(done is not null && at >= 0 ? done[at].Mod.Rel : m.Rel);

        async Task Back(List<(ModFile Mod, string From)> moved)
        {
            try { await ModWork(() => { ModPriority.Back(docs, moved); return 0; }); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The mod was not moved"), ErrorText(e)); }
            await ShowAgain(m.Rel);
        }

        // the list is read again (the file may be in another folder now): the same mod stays open on this tab
        async Task ShowAgain(string rel)
        {
            var tab = _detailsTab;
            await ModsAgain();
            if (_modShown.FirstOrDefault(r => r.File.Rel.Equals(rel, StringComparison.OrdinalIgnoreCase)) is not { } row) return;
            (_detailsFor, _detailsTab) = (row.File.Rel, tab);
            var list = _modsGrid ? (ListViewBase)ModGrid : ModList;
            list.SelectedItems.Clear();
            list.SelectedItems.Add(row);
        }
    }
}
