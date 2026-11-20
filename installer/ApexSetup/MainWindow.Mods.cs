using System.Collections.ObjectModel;
using System.Diagnostics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.ApplicationModel.DataTransfer;
using static ApexSetup.Strings;

namespace ApexSetup;

// The Mods screen. The side pane lists places in four groups: the library (all mods, new since the last game, what needs a
// look, duplicates, what is off), the player's folders, their tags and the game folder's ASI plugins. The list shows the chosen place through the search, the filters (file type, kind, state, tags, age, size) and
// the sort; the details pane shows the chosen mod or what several chosen mods share. Moves, renames, removals and merges go
// through ModLibrary and ModMerge, notes and tags through ModNotes.
public sealed partial class MainWindow
{
    const string DefaultMods = "*default", AllMods = "*all", NewMods = "*new", LookMods = "*look", DupMods = "*dup", OffMods = "*off",
        AsiMods = "*asi", NewTag = "*newtag";

    string _modPlace = AllMods;
    List<ModRow> _modRows = [];
    readonly RowCollection _modShown = [];

    // the shown rows: Reset swaps them all with one change event (a list of thousands raised one per row before)
    sealed class RowCollection : ObservableCollection<ModRow>
    {
        public void Reset(IEnumerable<ModRow> rows)
        {
            Items.Clear();
            foreach (var row in rows) Items.Add(row);
            OnPropertyChanged(new System.ComponentModel.PropertyChangedEventArgs(nameof(Count)));
            OnPropertyChanged(new System.ComponentModel.PropertyChangedEventArgs("Item[]"));
            OnCollectionChanged(new System.Collections.Specialized.NotifyCollectionChangedEventArgs(System.Collections.Specialized.NotifyCollectionChangedAction.Reset));
        }
    }
    Dictionary<string, List<string>> _modFindings = new(StringComparer.OrdinalIgnoreCase); // by path: the findings' texts
    Func<string, bool>? _modLoadedBy;
    List<string> _modDirs = [];
    string _folderSig = "";
    bool _fillingMods, _modsAgain;
    List<ModRow>? _dragRows;
    string? _dragFolder;
    readonly Dictionary<string, ModContents> _modContents = new(StringComparer.OrdinalIgnoreCase);
    Action? _modUndo;
    GameIndex? _gameIndex;

    // filters: each a set of chosen values; a mod matches any value of a group and every group in use
    readonly HashSet<string> _fFiles = [], _fKinds = [], _fStates = [], _fTags = [], _fCreators = [];
    Dictionary<string, string> _modCreatorGuess = new(StringComparer.OrdinalIgnoreCase);
    string? CreatorOf(ModFile m) => ModCreators.Of(m, _modCreatorGuess).Name;
    int _fAddedDays, _fSizeMb, _modSort, _filterGroup = 1;
    bool _modsGrid;
    readonly HashSet<string> _collapsed = [];

    // ---------- loading ----------

    bool _mergesRecovered;

    async Task LoadModsAsync()
    {
        if (_game is not { } g) return;
        // (a change made while the list is read: the list is read again once this read ends)
        if (_modsLoading) { _modsAgain = true; return; }
        _modsLoading = true;
        _modsAgain = false;
        ModsSummary.Text = T("Reading your mods…");
        var docs = Docs;
        // (only at the first read: no merge of this launcher runs yet, and a merge that fails later rolls itself back)
        var recover = !_mergesRecovered;
        _mergesRecovered = true;
        try
        {
            var since = ModViewPrefs.LastPlay;
            var (mods, found, dirs, loaded, dups, kinds) = await Task.Run(() =>
            {
                if (recover) ModMerge.Recover(docs); // (a merge stopped during the move: its originals back first)
                var m = Mods.List(g, docs);
                // every kind each package holds (read once per file; ModKinds never throws)
                var k = m.Where(x => x.FileType == "package").ToDictionary(x => x.Path, x => ModKinds.Read(x.Path), StringComparer.OrdinalIgnoreCase);
                return (m, ModChecks.Run(m, g, docs), ModLibrary.Folders(docs), ModLibrary.LoadedBy(docs), ModLibrary.Duplicates(m), k);
            });
            _mods = mods;
            _modFindings = new(StringComparer.OrdinalIgnoreCase);
            foreach (var f in found)
                foreach (var file in f.Files)
                    (_modFindings.TryGetValue(file, out var l) ? l : _modFindings[file] = []).Add(T(f.Finding.Text, f.Finding.Args));
            _modLooks = _modFindings.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase);
            _modDirs = dirs;
            _modLoadedBy = loaded;
            _modCreatorGuess = ModCreators.Guess(mods);
            _modRows = mods.Select(m => new ModRow(m, _modFindings.TryGetValue(m.Path, out var l) ? l : [])
            {
                IsNew = since is { } s && Added(m) > s,
                DuplicateOf = dups.TryGetValue(m.Path, out var d) ? Path.GetFileNameWithoutExtension(d) : null,
                DetectedKinds = kinds.TryGetValue(m.Path, out var k) && k.Kinds.Any() ? k.Kinds.Select(x => x.ToString()).ToList()
                    : [m.FileType == "asi" ? "Asi" : m.Kind.ToString()],
            }).ToList();
            foreach (var r in _modRows) ApplyKinds(r);
            _modsLoaded = true;
            Safe(LoadModPicturesAsync(_modRows));
            Safe(LoadDefaultsAsync(g, _modRows));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The launcher could not read your mods"), ErrorText(e)); }
        finally { _modsLoading = false; }
        _folderSig = "";
        FillMods();
        if (_modsAgain) await LoadModsAsync();
    }

    // the game's own keys (cached; read once, in a second or two, when the game's files change) and which mods replace them
    async Task LoadDefaultsAsync(Game g, List<ModRow> rows)
    {
        try
        {
            var found = await Task.Run(() =>
            {
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(60));
                var gi = DefaultReplacements.Cached(g) ?? DefaultReplacements.Index(g, null, cts.Token);
                return (gi, rows.Where(r => r.File.FileType == "package").Select(r => (r, DefaultReplacements.Analyze(gi, r.File))).ToList());
            });
            if (!ReferenceEquals(rows, _modRows)) return; // (the list was read again meanwhile)
            _gameIndex = found.gi;
            foreach (var (r, d) in found.Item2) r.Defaults = d;
            _folderSig = "";
            FillMods();
        }
        catch (Exception e) when (e is not OutOfMemoryException) { App.Log(e); }
    }

    // when a mod arrived: the later of its creation (a copy) and last change (an update)
    static DateTime Added(ModFile m)
    {
        try { var fi = new FileInfo(m.Path); return fi.CreationTime > fi.LastWriteTime ? fi.CreationTime : fi.LastWriteTime; }
        catch (IOException) { return DateTime.MinValue; }
    }

    // the player's own picture first, else the one inside the package; one at a time, behind the list
    // (a newer list read stops an older loader: every install, merge or removal reads the list again)
    async Task LoadModPicturesAsync(List<ModRow> rows)
    {
        var whole = ReferenceEquals(rows, _modRows);
        foreach (var r in rows)
        {
            if (whole && !ReferenceEquals(rows, _modRows)) return;
            try
            {
                var bytes = await Task.Run(() => ModNotes.PicturePath(r.File.Rel) is { } own ? File.ReadAllBytes(own) : Mods.Thumbnail(r.File));
                r.Image = await SaveCard.ImageOf(bytes);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { }
        }
    }

    // a package's folder under Mods ("Packages\NRaas"; "" for Mods itself)
    static string DirOf(ModFile m) => m.FileType != "package" ? "" : Path.GetRelativePath("Mods", m.Folder) is var r && r == "." ? "" : r;
    static bool InDir(ModFile m, string dir) => m.FileType == "package"
        && (DirOf(m).Equals(dir, StringComparison.OrdinalIgnoreCase) || DirOf(m).StartsWith(dir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase));
    bool Loaded(ModFile m) => m.FileType != "package" || _modLoadedBy is not { } by || by(Path.GetRelativePath("Mods", m.Rel));
    // (a folder the game loads, or one Resource.cfg names a file of, like PackedFile TinyUIFix/tiny-ui-fix.package)
    bool FolderLoaded(string dir) => _modLoadedBy is not { } by || by(Path.Combine(dir, "x.package")) || _mods.Any(m => InDir(m, dir) && m.Enabled && Loaded(m));
    static bool IsFolder(string place) => !place.StartsWith('*') && !place.StartsWith('#');

    // ---------- filling the screen ----------

    void FillMods()
    {
        if (_game is null) { ModsSummary.Text = T("Choose the game first."); return; }
        if (!_modsLoaded) { if (!_modsLoading) Safe(LoadModsAsync()); return; }
        _fillingMods = true;
        try
        {
            ModRow.Toggle = ToggleMod;
            ModRow.Check = OnModCheck;
            if (ModList.ItemsSource is null) { ModList.ItemsSource = _modShown; ModGrid.ItemsSource = _modShown; LoadViewPrefs(); }
            FillSide();
            var inPlace = _modRows.Where(r => InPlace(r, _modPlace)).ToList();
            var shown = Sorted(inPlace.Where(Matches)).ToList();
            ModRow.ShowPlace = !IsFolder(_modPlace);
            Sync(shown);
            ModsSelectAll.Visibility = ModsSelectNone.Visibility = Visibility.Visible;
            ModList.Visibility = ModListHead.Visibility = _modsGrid ? Visibility.Collapsed : Visibility.Visible;
            ModGrid.Visibility = _modsGrid ? Visibility.Visible : Visibility.Collapsed;
            ModsViewList.IsChecked = !_modsGrid;
            ModsViewGallery.IsChecked = _modsGrid;
            // (the chosen one's icon in the colour that reads on the accent)
            ModsViewListIcon.Stroke = Res(_modsGrid ? "ApexSoftTextBrush" : "TextOnAccentFillColorPrimaryBrush");
            ModsViewGalleryIcon.Stroke = Res(_modsGrid ? "TextOnAccentFillColorPrimaryBrush" : "ApexSoftTextBrush");
            HuntBanner();

            // empty: say what to do here
            ModsEmpty.Visibility = shown.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            (ModsEmptyIcon.Kind, ModsEmptyText.Text) = inPlace.Count > 0 ? ("search", T("No mod matches the search and filters. Clear them to see all {0}.", inPlace.Count))
                : _modPlace switch
                {
                    NewMods => ("sparkles", T("Nothing new since you last played.")),
                    LookMods => ("circle-check", T("Nothing needs a look.")),
                    DupMods => ("circle-check", T("No mod is here twice.")),
                    OffMods => ("power", T("Every mod is on.")),
                    _ when _modPlace.StartsWith('#') => ("tag", T("No mod has this tag yet. Drag mods onto it, or add it in a mod's details.")),
                    _ => ("folder-open", T("Nothing here yet. Drag mods here, drop files from Explorer, or use Add mods.")),
                };

            // the header: where this is, what it holds
            var (crumb, title) = PlaceTitle(_modPlace);
            ModsCrumb.Text = crumb;
            ModsCrumb.Visibility = crumb.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            ModsTitle.Text = title;
            ModsPlacesText.Text = title;
            var off = inPlace.Count(r => !r.File.Enabled);
            var looks = inPlace.Count(NeedsLook);
            ModsSummary.Text = string.Join(" · ", new[]
            {
                inPlace.Count == 1 ? T("1 file") : T("{0} files", inPlace.Count), ModRow.SizeText(inPlace.Sum(r => r.File.Size)),
                off > 0 ? T("{0} off", off) : null, looks > 0 ? T("{0} need a look", looks) : null,
                IsFolder(_modPlace) && !FolderLoaded(_modPlace) ? T("the game does not load this folder") : null,
            }.Where(x => x is not null));
            ModsAdd.IsEnabled = true; // (from a place that is no folder, files go into Mods › Packages; the preview says where)
            FillChips();
            FillSortText();
            FillFooter();
        }
        finally { _fillingMods = false; }
        FillModDetails();
    }

    // the shown list changed: remove and insert rows in place, so the list animates instead of being rebuilt
    void Sync(List<ModRow> shown)
    {
        if (_modShown.SequenceEqual(shown)) return;
        var keep = shown.ToHashSet();
        var had = _modShown.ToHashSet();
        // a big change (a search over thousands of mods, a new sort order) is one reset, the selection put back; animating
        // it would raise and animate an event per row
        var changes = _modShown.Count(r => !keep.Contains(r)) + shown.Count(r => !had.Contains(r));
        if (changes > 200 || shown.Count > 200 && !_modShown.Where(keep.Contains).SequenceEqual(shown.Where(had.Contains)))
        {
            var picked = Picked().Where(keep.Contains).ToList();
            _modShown.Reset(shown);
            var list = _modsGrid ? (ListViewBase)ModGrid : ModList;
            foreach (var row in picked) list.SelectedItems.Add(row);
            return;
        }
        for (int i = _modShown.Count - 1; i >= 0; i--) if (!keep.Contains(_modShown[i])) _modShown.RemoveAt(i);
        for (int i = 0; i < shown.Count; i++)
        {
            if (i < _modShown.Count && ReferenceEquals(_modShown[i], shown[i])) continue;
            var at = _modShown.IndexOf(shown[i]);
            if (at >= 0) _modShown.Move(at, i); else _modShown.Insert(i, shown[i]);
        }
    }

    (string Crumb, string Title) PlaceTitle(string place) => place switch
    {
        AllMods => ("", T("All mods")), NewMods => ("", T("New since last play")), DefaultMods => ("", T("Default replacements")), LookMods => ("", T("Needs a look")), DupMods => ("", T("Duplicates")),
        OffMods => ("", T("Turned off")), AsiMods => ("", T("Game plugins")),
        _ when place.StartsWith('#') => (T("Tag"), place[1..]),
        "" => ("", "Mods"),
        _ => (string.Join(" › ", new[] { "Mods" }.Concat(place.Split(Path.DirectorySeparatorChar).SkipLast(1))), Path.GetFileName(place)),
    };

    bool InPlace(ModRow r, string place)
    {
        var m = r.File;
        return place switch
        {
            AllMods => m.FileType == "package",
            NewMods => r.IsNew,
            DefaultMods => r.Defaults.IsDefault,
            LookMods => NeedsLook(r),
            DupMods => r.DuplicateOf is not null,
            OffMods => !m.Enabled,
            AsiMods => m.FileType == "asi",
            _ when place.StartsWith('#') => ModNotes.TagsOf(m.Rel).Contains(place[1..], StringComparer.OrdinalIgnoreCase),
            _ => InDir(m, place),
        };
    }

    // a finding, or on in a place the game does not load: the side pane's Needs a look, the state filter and the summary alike
    bool NeedsLook(ModRow r) => r.Findings.Count > 0 || !Loaded(r.File) && r.File.Enabled;

    bool Matches(ModRow r)
    {
        var m = r.File;
        var q = ModSearch.Text?.Trim() ?? "";
        if (q.Length > 0 && !(m.Rel.Contains(q, StringComparison.OrdinalIgnoreCase)
            || (ModNotes.Get(m.Rel)?.Notes?.Contains(q, StringComparison.OrdinalIgnoreCase) ?? false)
            || ModNotes.TagsOf(m.Rel).Any(t => t.Contains(q, StringComparison.OrdinalIgnoreCase))
            || (CreatorOf(m)?.Contains(q, StringComparison.OrdinalIgnoreCase) ?? false)
            || r.Kinds.Any(k => KindLabel(k).Contains(q, StringComparison.OrdinalIgnoreCase))
            || _modContents.TryGetValue(m.Path, out var c) && c.Names.Concat(c.PartNames).Any(n => n.Contains(q, StringComparison.OrdinalIgnoreCase))))
            return false;
        if (_fFiles.Count > 0 && !_fFiles.Contains(m.FileType)) return false;
        if (_fKinds.Count > 0 && !r.Kinds.Any(_fKinds.Contains)) return false;
        if (_fStates.Count > 0 && !_fStates.Any(s => s switch
            {
                "on" => m.Enabled, "off" => !m.Enabled, "look" => NeedsLook(r), "new" => r.IsNew, "dup" => r.DuplicateOf is not null, "default" => r.Defaults.IsDefault, _ => false,
            })) return false;
        if (_fTags.Count > 0 && !_fTags.Any(t => t == "" ? ModNotes.TagsOf(m.Rel).Count == 0 : ModNotes.TagsOf(m.Rel).Contains(t, StringComparer.OrdinalIgnoreCase))) return false;
        if (_fCreators.Count > 0 && !_fCreators.Contains(CreatorOf(m) ?? "")) return false;
        if (_fAddedDays != 0 && !AddedWithin(m, _fAddedDays)) return false;
        if (_fSizeMb > 0 && m.Size < (long)_fSizeMb << 20) return false;
        return true;
    }

    IEnumerable<ModRow> Sorted(IEnumerable<ModRow> rows) => _modSort switch
    {
        1 => rows.OrderByDescending(r => r.File.Size),
        2 => rows.OrderBy(r => KindLabel(r.Kinds[0])).ThenBy(r => r.Name, StringComparer.OrdinalIgnoreCase),
        3 => rows.OrderBy(r => r.File.Folder, StringComparer.OrdinalIgnoreCase).ThenBy(r => r.Name, StringComparer.OrdinalIgnoreCase),
        4 => rows.OrderByDescending(r => Added(r.File)),
        5 => rows.OrderBy(r => r.Findings.Count > 0 || r.DuplicateOf is not null ? 0 : 1).ThenBy(r => r.Name, StringComparer.OrdinalIgnoreCase),
        _ => rows.OrderBy(r => r.Name, StringComparer.OrdinalIgnoreCase),
    };

    List<ModRow> Picked() => (_modsGrid ? ModGrid.SelectedItems : ModList.SelectedItems).OfType<ModRow>().ToList();
    static bool Mine(ModRow r) => !r.File.Locked && r.File.FileType == "package";
    // (objects, build items or Create a Sim parts: a save that uses them loses them when it is saved without the mod)
    static bool HasItems(ModRow r) => r.Kinds.Any(k => k.StartsWith("Buy") || k.StartsWith("Build") || KindGroup(k) is 1 or 2 or 3);

    // ---------- the side pane ----------

    sealed record Place(string Key, string Label, string Icon, int Count, int Depth = 0, string? Color = null, bool Warn = false);

    void FillSide()
    {
        var dirs = _modDirs.Concat(_mods.Where(m => m.FileType == "package").Select(DirOf).Where(d => d.Length > 0))
            .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase).ToList();
        int N(string place) => _modRows.Count(r => InPlace(r, place));
        // each package counted in its folder and every folder above, in one pass (a count per folder walked every mod)
        var perDir = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        foreach (var m in _mods.Where(m => m.FileType == "package"))
            for (var d = DirOf(m); d.Length > 0; d = Path.GetDirectoryName(d) ?? "") perDir[d] = perDir.GetValueOrDefault(d) + 1;
        var groups = new List<(string Key, string Label, string? Add, List<Place> Items)>
        {
            ("library", T("LIBRARY"), null, new List<Place> { new(AllMods, T("All mods"), "layers", N(AllMods)) }
                .Concat(new[] { new Place(AsiMods, T("Game plugins (ASI)"), "plug", N(AsiMods)), new(NewMods, T("New since last play"), "sparkles", N(NewMods)), new(DefaultMods, T("Default replacements"), "replace", N(DefaultMods)), new(LookMods, T("Needs a look"), "triangle-alert", N(LookMods), Warn: true),
                    new(DupMods, T("Duplicates"), "copy", N(DupMods)), new(OffMods, T("Turned off"), "power-off", N(OffMods)) }.Where(p => p.Count > 0 || p.Key == _modPlace)).ToList()),
            ("folders", T("FOLDERS"), "folder", dirs.Select(d => new Place(d, Path.GetFileName(d), "folder", perDir.GetValueOrDefault(d), d.Count(c => c == Path.DirectorySeparatorChar))).ToList()),
            ("tags", T("TAGS"), "tag", ModNotes.Tags.Select(t => new Place("#" + t.Name, t.Name, "", N("#" + t.Name), Color: t.Color)).ToList()),
        };
        if (!groups.SelectMany(g => g.Items).Any(p => p.Key.Equals(_modPlace, StringComparison.OrdinalIgnoreCase))) _modPlace = AllMods;

        var sig = string.Join("|", groups.Select(g => g.Key + (_collapsed.Contains(g.Key) ? "-" : "+") + string.Join(",", g.Items.Select(p => $"{p.Key}:{p.Count}:{p.Color}")))) + Lang;
        if (sig == _folderSig) { SelectPlace(); return; }
        _folderSig = sig;
        var items = new List<FrameworkElement>();
        foreach (var (key, label, add, places) in groups)
        {
            items.Add(GroupHeader(key, label, add));
            if (_collapsed.Contains(key)) continue;
            foreach (var p in places) items.Add(PlaceItem(p));
            if (key == "tags") items.Add(PlaceItem(new Place(NewTag, T("New tag"), "plus", -1)));
        }
        ModFolders.ItemsSource = items;
        SelectPlace();
    }

    void SelectPlace() => ModFolders.SelectedItem = (ModFolders.ItemsSource as List<FrameworkElement>)?.FirstOrDefault(i => i.Tag as string == _modPlace);

    FrameworkElement GroupHeader(string key, string label, string? add)
    {
        // the whole row is the heading's button (hover and click across it); the + sits on its right end
        var g = new Grid { Tag = "-", Margin = new Thickness(0, 12, 0, 2), Height = 30 };
        var head = new Button
        {
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0), Padding = new Thickness(10, 0, 8, 0),
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Stretch,
            CornerRadius = new CornerRadius(4),
        };
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        s.Children.Add(Lucide.Icon(_collapsed.Contains(key) ? "chevron-right" : "chevron-down", 12, Res("ApexMutedBrush")));
        s.Children.Add(new TextBlock { Text = label, FontSize = 11, FontWeight = FontWeights.SemiBold, CharacterSpacing = 70, Foreground = Res("ApexMutedBrush") });
        head.Content = s;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(head, label);
        head.Click += (_, _) => { if (!_collapsed.Remove(key)) _collapsed.Add(key); SaveViewPrefs(); _folderSig = ""; FillSide(); };
        g.Children.Add(head);
        if (add is not null)
        {
            var tip = add == "tag" ? T("New tag") : T("New folder");
            var plus = IconButton("plus", tip);
            plus.Width = plus.Height = 26;
            plus.HorizontalAlignment = HorizontalAlignment.Right;
            plus.Margin = new Thickness(0, 0, 2, 0);
            plus.Click += (_, _) => { if (add == "tag") Safe(NewTagAsync(null)); else Safe(NewFolderAsync(NewFolderParent())); };
            g.Children.Add(plus);
        }
        return g;
    }

    FrameworkElement PlaceItem(Place p)
    {
        var g = new Grid { ColumnSpacing = 10, Tag = p.Key, Padding = new Thickness(14 * p.Depth, 0, 0, 0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var dim = IsFolder(p.Key) && !FolderLoaded(p.Key);
        var fg = p.Warn ? Res("ApexWarningBrush") : Res("ApexMutedBrush");
        g.Children.Add(p.Color is { } c
            ? new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 10, Height = 10, Fill = TagDot.BrushOf(c), Margin = new Thickness(3, 0, 3, 0), VerticalAlignment = VerticalAlignment.Center }
            : Lucide.Icon(p.Icon, 16, fg));
        var name = new TextBlock { Text = p.Label, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center, Opacity = dim ? 0.55 : 1 };
        if (p.Warn) name.Foreground = fg;
        Grid.SetColumn(name, 1);
        g.Children.Add(name);
        var n = new TextBlock { Text = p.Count < 0 ? "" : p.Count.ToString(), FontSize = 12, Foreground = fg, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(n, 2);
        g.Children.Add(n);
        ToolTipService.SetToolTip(g, dim ? T("{0}: the game does not load packages from this folder (Mods\\Resource.cfg).", p.Label) : p.Label);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(g, p.Count < 0 ? p.Label : $"{p.Label} {p.Count}");

        if (IsFolder(p.Key))
        {
            var dir = p.Key;
            DropTarget(g, () => _dragFolder is { } f ? CanNest(f, dir) : _dragRows is not null,
                () => _dragFolder is { } f ? T("Move {0} into {1}", Path.GetFileName(f), p.Label) : T("Move to {0}", p.Label),
                () => { if (_dragFolder is { } f) Safe(MoveFolderAsync(f, dir)); else if (_dragRows is { } rows) MoveMods(rows, dir); });
            g.DoubleTapped += (_, _) => BeginRename(g, name, dir);
            g.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.F2) { e.Handled = true; BeginRename(g, name, dir); } };
            var menu = new MenuFlyout();
            menu.Items.Add(MenuItem(T("Rename"), () => BeginRename(g, name, dir), "pencil", Windows.System.VirtualKey.F2));
            menu.Items.Add(MenuItem(T("New folder inside"), () => Safe(NewFolderAsync(dir)), "folder-plus"));
            menu.Items.Add(MenuItem(T("Turn every mod in it on"), () => SetMods(_modRows.Where(r => InDir(r.File, dir) && Mine(r)).ToList(), true), "power"));
            menu.Items.Add(MenuItem(T("Turn every mod in it off"), () => SetMods(_modRows.Where(r => InDir(r.File, dir) && Mine(r)).ToList(), false), "power-off"));
            menu.Items.Add(MenuItem(T("Merge this folder…"), () => Safe(MergeAsync(_modRows.Where(r => InDir(r.File, dir) && Mine(r)).ToList(), Path.GetFileName(dir), dir)), "merge"));
            menu.Items.Add(MenuItem(T("Open in Explorer"), () => OpenFolder(Path.Combine(Docs, "Mods", dir)), "folder-search"));
            menu.Items.Add(new MenuFlyoutSeparator());
            menu.Items.Add(MenuItem(T("Delete folder"), () => Safe(DeleteFolderAsync(dir)), "trash-2"));
            g.ContextFlyout = menu;
        }
        else if (p.Key.StartsWith('#'))
        {
            var tag = p.Label;
            DropTarget(g, () => _dragRows is not null, () => T("Tag as {0}", tag), () => { if (_dragRows is { } rows) TagMods(rows, tag, true); });
            var menu = new MenuFlyout();
            menu.Items.Add(MenuItem(T("Edit tag"), () => Safe(EditTagAsync(tag)), "pencil"));
            menu.Items.Add(MenuItem(T("Delete tag"), () => Safe(DeleteTagAsync(tag)), "trash-2"));
            g.ContextFlyout = menu;
        }
        else if (p.Key == OffMods) DropTarget(g, () => _dragRows is not null, () => T("Turn off"), () => { if (_dragRows is { } rows) SetMods(rows, false); });
        return g;
    }

    // a folder can be dragged into another; the list drags it (a row made draggable itself takes the click, and a click on a
    // folder then picked nothing)
    void OnFolderDragStart(object sender, DragItemsStartingEventArgs e)
    {
        if (e.Items.FirstOrDefault() is FrameworkElement { Tag: string key } && IsFolder(key))
        {
            _dragFolder = key;
            _dragRows = null;
            e.Data.SetText(key);
            e.Data.RequestedOperation = DataPackageOperation.Move;
        }
        else e.Cancel = true;
    }

    void OnFolderDragDone(ListViewBase sender, DragItemsCompletedEventArgs e) => _dragFolder = null;

    static bool CanNest(string folder, string into) =>
        !into.Equals(folder, StringComparison.OrdinalIgnoreCase) && !into.StartsWith(folder + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
        && !string.Equals(Path.GetDirectoryName(folder) ?? "", into, StringComparison.OrdinalIgnoreCase);

    // a place that takes a drop: it lights up and grows a little while something is held over it
    static void DropTarget(Grid g, Func<bool> can, Func<string> caption, Action drop)
    {
        g.AllowDrop = true;
        var normal = g.Background;
        g.CornerRadius = new CornerRadius(4);
        g.BorderThickness = new Thickness(1);
        g.BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        // (the tint fades in and out, the row lifts a touch: a held item sees where it lands without a jump)
        if (AnimationsOn) g.BackgroundTransition = new BrushTransition { Duration = TimeSpan.FromMilliseconds(120) };
        void Light(bool on)
        {
            g.Background = on ? Res("ApexAccentSoftBrush") : normal;
            g.BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            if (!AnimationsOn) return;
            g.CenterPoint = new System.Numerics.Vector3((float)g.ActualWidth / 2, (float)g.ActualHeight / 2, 0);
            g.ScaleTransition ??= new Vector3Transition { Duration = TimeSpan.FromMilliseconds(160) };
            g.Scale = on ? new System.Numerics.Vector3(1.02f, 1.02f, 1) : new System.Numerics.Vector3(1, 1, 1);
        }
        g.DragEnter += (_, _) => { if (can()) Light(true); };
        g.DragLeave += (_, _) => Light(false);
        g.DragOver += (_, e) =>
        {
            if (!can()) return;
            e.AcceptedOperation = DataPackageOperation.Move;
            e.DragUIOverride.Caption = caption();
        };
        g.Drop += (_, _) => { Light(false); if (can()) drop(); };
    }

    static bool AnimationsOn => new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;

    // menus take an icon element: Windows' own Segoe Fluent glyphs that match the Lucide icons used elsewhere
    static IconElement MenuIcon(string name, Brush? brush = null)
    {
        var glyph = name switch
        {
            "power" or "power-off" => "", "pencil" => "", "folder" => "", "folder-plus" => "", "folder-input" => "",
            "folder-search" => "", "merge" => "", "trash-2" => "", "copy" => "", "tag" => "", "plus" => "",
            "layers" => "", "sparkles" => "", "triangle-alert" => "", "plug" => "", "package" => "", "library" => "",
            _ => "",
        };
        var i = new FontIcon { Glyph = glyph };
        if (brush is not null) i.Foreground = brush;
        return i;
    }

    static MenuFlyoutItem MenuItem(string text, Action click, string? icon = null, Windows.System.VirtualKey? key = null, bool danger = false)
    {
        var i = new MenuFlyoutItem { Text = text };
        if (icon is not null) i.Icon = MenuIcon(icon);
        if (key is { } k) i.KeyboardAcceleratorTextOverride = k == Windows.System.VirtualKey.Space ? T("Space") : k.ToString();
        if (danger) i.Foreground = Res("ApexErrorBrush");
        i.Click += (_, _) => click();
        return i;
    }

    void OnModFolderPicked(object sender, SelectionChangedEventArgs e)
    {
        if (_fillingMods) return;
        if (ModFolders.SelectedItem is not FrameworkElement { Tag: string key } || key == "-") { DispatcherQueue.TryEnqueue(SelectPlace); return; }
        if (key == NewTag) { DispatcherQueue.TryEnqueue(() => { SelectPlace(); Safe(NewTagAsync(null)); }); return; }
        if (key == _modPlace) return;
        _modPlace = key;
        ModList.SelectedItems.Clear();
        ModGrid.SelectedItems.Clear();
        SaveViewPrefs();
        DispatcherQueue.TryEnqueue(FillMods);
    }

    // a narrow window: the side pane becomes this button's menu
    void OnModsPlaces(object sender, RoutedEventArgs e)
    {
        var menu = new MenuFlyout();
        void Add(string key, string label, string icon)
        {
            var i = new ToggleMenuFlyoutItem { Text = label, IsChecked = key == _modPlace, Icon = MenuIcon(icon) };
            i.Click += (_, _) => { _modPlace = key; SaveViewPrefs(); _folderSig = ""; FillMods(); };
            menu.Items.Add(i);
        }
        foreach (var (k, l, i) in new[] { (AllMods, T("All mods"), "layers"), (AsiMods, T("Game plugins (ASI)"), "plug"), (NewMods, T("New since last play"), "sparkles"), (DefaultMods, T("Default replacements"), "replace"), (LookMods, T("Needs a look"), "triangle-alert"),
                     (DupMods, T("Duplicates"), "copy"), (OffMods, T("Turned off"), "power-off") }) Add(k, l, i);
        menu.Items.Add(new MenuFlyoutSeparator());
        foreach (var d in _modDirs) Add(d, new string(' ', 3 * d.Count(c => c == Path.DirectorySeparatorChar)) + Path.GetFileName(d), "folder");
        if (ModNotes.Tags.Count > 0) menu.Items.Add(new MenuFlyoutSeparator());
        foreach (var t in ModNotes.Tags) Add("#" + t.Name, t.Name, "tag");
        menu.ShowAt(ModsPlaces);
    }

    // ---------- renaming a folder in place ----------

    void BeginRename(Grid row, TextBlock label, string dir)
    {
        if (row.Children.OfType<TextBox>().Any()) return;
        var box = new TextBox { Text = Path.GetFileName(dir), MinWidth = 0, Padding = new Thickness(6, 2, 6, 2), FontSize = 13 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, T("New name"));
        Grid.SetColumn(box, 1);
        Grid.SetColumnSpan(box, 2);
        label.Visibility = Visibility.Collapsed;
        row.Children.Add(box);
        box.Loaded += (_, _) => { box.Focus(FocusState.Programmatic); box.SelectAll(); };
        var done = false;
        void End(bool save)
        {
            if (done) return;
            done = true;
            row.Children.Remove(box);
            label.Visibility = Visibility.Visible;
            if (save && box.Text.Trim() is { Length: > 0 } name && name != Path.GetFileName(dir)) Safe(RenameFolderAsync(dir, name));
        }
        box.KeyDown += (_, e) =>
        {
            if (e.Key == Windows.System.VirtualKey.Enter) { e.Handled = true; End(true); }
            else if (e.Key == Windows.System.VirtualKey.Escape) { e.Handled = true; End(false); }
        };
        box.LostFocus += (_, _) => End(OnScreen(box));
    }

    // (a rebuild, as a resize into another layout makes, takes a rename box away and its focus with it: that is not the
    // player done typing, so the half-typed name is not saved)
    bool OnScreen(UIElement e)
    {
        for (DependencyObject? p = e; p is not null; p = Microsoft.UI.Xaml.Media.VisualTreeHelper.GetParent(p))
        {
            if (p is UIElement { Visibility: Visibility.Collapsed }) return false;
            if (p == Content) return true;
        }
        return false;
    }

    // ---------- folders ----------

    async Task<string?> AskName(string title, string initial, string yes)
    {
        var box = new TextBox { Text = initial, MinWidth = 320 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, title);
        box.Loaded += (_, _) => box.SelectAll();
        var d = new ContentDialog { Title = title, Content = box, PrimaryButtonText = yes, CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        return await ShowDialog(d) == ContentDialogResult.Primary && box.Text.Trim().Length > 0 ? box.Text : null;
    }

    // new folders go in the chosen folder, else in Packages (where the game loads them), else in Mods
    string NewFolderParent() => IsFolder(_modPlace) ? _modPlace : _modDirs.FirstOrDefault(d => d.Equals("Packages", StringComparison.OrdinalIgnoreCase)) ?? "";

    async Task NewFolderAsync(string parent)
    {
        if (await AskName(T("New folder"), T("New folder"), T("Create")) is not { } name) return;
        await ModsChange(() => { _modPlace = ModLibrary.CreateFolder(Docs, parent, name); }, T("Folder created"));
    }

    async Task RenameFolderAsync(string dir, string name)
    {
        string? to = null;
        await ModsChange(() =>
        {
            to = ModLibrary.RenameFolder(Docs, dir, name);
            if (_modPlace.Equals(dir, StringComparison.OrdinalIgnoreCase) || _modPlace.StartsWith(dir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                _modPlace = to + _modPlace[dir.Length..];
        }, T("Folder renamed"), () => { if (to is not null) Safe(RenameFolderAsync(to, Path.GetFileName(dir))); });
    }

    async Task MoveFolderAsync(string dir, string into)
    {
        string? to = null;
        var from = Path.GetDirectoryName(dir) ?? "";
        await ModsChange(() =>
        {
            to = ModLibrary.MoveFolder(Docs, dir, into);
            if (_modPlace.Equals(dir, StringComparison.OrdinalIgnoreCase)) _modPlace = to;
        }, T("{0} moved into {1}", Path.GetFileName(dir), Path.GetFileName(into)), () => { if (to is not null) Safe(MoveFolderAsync(to, from)); });
    }

    async Task DeleteFolderAsync(string dir)
    {
        if (!await Confirm(T("Delete this folder?"), T("{0} is deleted. Only an empty folder can be deleted; your mods are never deleted with it.", Path.GetFileName(dir)), T("Delete"), danger: true)) return;
        await ModsChange(() => { ModLibrary.DeleteFolder(Docs, dir); _modPlace = Path.GetDirectoryName(dir) is { Length: > 0 } up ? up : AllMods; }, T("Folder deleted"));
    }

    // a change to the folders or files: done, said on the bar (with Undo when it can be undone), the list read again
    async Task ModsChange(Action work, string done, Action? undo = null)
    {
        try { work(); ModsResult(done, undo); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }

    void ModsResult(string done, Action? undo, string detail = "", InfoBarSeverity severity = InfoBarSeverity.Success)
    {
        _modUndo = undo;
        SetInfo(ModsInfo, severity, done, detail, undo is null ? null : T("Undo"), undo is null ? null : () => { var u = _modUndo; _modUndo = null; ModsInfo.IsOpen = false; u?.Invoke(); });
    }

    // ---------- moving, switching, adding ----------

    void OnModDragStart(object sender, DragItemsStartingEventArgs e)
    {
        _dragFolder = null;
        _dragRows = e.Items.OfType<ModRow>().Where(Mine).ToList();
        if (_dragRows.Count == 0) { e.Cancel = true; _dragRows = null; return; }
        e.Data.RequestedOperation = DataPackageOperation.Move;
        e.Data.SetText(string.Join("\n", _dragRows.Select(r => r.Name)));
    }

    // (a drag let go outside a target or cancelled with Esc: a later drop from Explorer must not act on these rows)
    void OnModDragDone(ListViewBase sender, DragItemsCompletedEventArgs e) => _dragRows = null;

    void MoveMods(List<ModRow> rows, string dir)
    {
        var back = new List<(ModRow Row, string Dir)>();
        string? error = null;
        foreach (var r in rows.Where(Mine))
        {
            var from = DirOf(r.File);
            try { var moved = ModLibrary.Move(r.File, Docs, dir); if (!ReferenceEquals(moved, r.File)) { Replace(r, moved); back.Add((r, from)); } }
            catch (Exception e) { App.Log(e); error ??= ErrorText(e); }
        }
        _dragRows = null;
        var where = Path.GetFileName(dir) is { Length: > 0 } f ? f : "Mods";
        void Undo()
        {
            string? failed = null;
            foreach (var (r, d) in back) try { Replace(r, ModLibrary.Move(r.File, Docs, d)); } catch (Exception e) { App.Log(e); failed ??= ErrorText(e); }
            if (failed is not null) SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some mods were not moved"), failed);
            _folderSig = "";
            FillMods();
        }
        // (some moved, some not: the ones that moved can still be undone)
        if (error is not null && back.Count == 0) SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some mods were not moved"), error);
        else if (error is not null) ModsResult(T("Some mods were not moved"), Undo, error, InfoBarSeverity.Warning);
        else if (back.Count > 0) ModsResult(back.Count == 1 ? T("Mod moved to {0}", where) : T("{0} mods moved to {1}", back.Count, where), Undo);
        _folderSig = "";
        FillMods();
    }

    // the row and the list keep the mod's new place
    void Replace(ModRow r, ModFile moved)
    {
        var i = _mods.IndexOf(r.File);
        if (i >= 0) _mods[i] = moved;
        if (_modFindings.Remove(r.File.Path, out var f)) _modFindings[moved.Path] = f;
        r.Update(moved);
    }

    ModFile? ToggleMod(ModRow row, bool on)
    {
        try
        {
            var path = Mods.SetEnabled(row.File, on, Docs);
            var moved = row.File with { Path = path, Enabled = on };
            var i = _mods.IndexOf(row.File);
            if (i >= 0) _mods[i] = moved;
            if (_modFindings.Remove(row.File.Path, out var f)) _modFindings[moved.Path] = f;
            DispatcherQueue.TryEnqueue(() => { _folderSig = ""; FillMods(); });
            return moved;
        }
        catch (Exception e)
        {
            App.Log(e);
            SetInfo(ModsInfo, InfoBarSeverity.Error, T("The mod was not moved"), ErrorText(e));
            return null;
        }
    }

    async void SetMods(List<ModRow> rows, bool on)
    {
        var todo = rows.Where(r => !r.File.Locked && r.File.Enabled != on).ToList();
        _dragRows = null;
        if (todo.Count <= 1)
        {
            foreach (var r in todo) if (ToggleMod(r, on) is { } one) r.Update(one);
            return;
        }
        // many at once: the moves off the UI thread, then the rows and the list once (a refresh per mod froze it for minutes)
        var docs = Docs;
        var done = await Task.Run(() =>
        {
            var list = new List<(ModRow Row, string? Path, Exception? Error)>();
            foreach (var r in todo)
                try { list.Add((r, Mods.SetEnabled(r.File, on, docs), null)); }
                catch (Exception e) { list.Add((r, null, e)); if (e is SetupException or InvalidOperationException) break; } // (game running, dry run: so would every other)
            return list;
        });
        var at = _mods.Select((m, i) => (m.Path, i)).ToDictionary(x => x.Path, x => x.i, StringComparer.OrdinalIgnoreCase);
        var changed = new List<ModRow>();
        Exception? error = null;
        foreach (var (r, path, e) in done)
        {
            if (path is null) { App.Log(e!); error ??= e; continue; }
            var moved = r.File with { Path = path, Enabled = on };
            if (at.TryGetValue(r.File.Path, out var i)) _mods[i] = moved;
            if (_modFindings.Remove(r.File.Path, out var f)) _modFindings[moved.Path] = f;
            r.Update(moved);
            changed.Add(r);
        }
        _folderSig = "";
        FillMods();
        // (some moved, some not: the ones that moved can still be undone)
        if (error is not null && changed.Count == 0) SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some mods were not moved"), ErrorText(error));
        else if (error is not null) ModsResult(on ? T("{0} of {1} mods turned on", changed.Count, todo.Count) : T("{0} of {1} mods turned off", changed.Count, todo.Count), () => SetMods(changed, !on), ErrorText(error), InfoBarSeverity.Warning);
        else if (changed.Count > 1) ModsResult(on ? T("{0} mods turned on", changed.Count) : T("{0} mods turned off", changed.Count), () => SetMods(changed, !on));
    }

    async void OnModsAdd(object sender, RoutedEventArgs e)
    {
        var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
        foreach (var x in new[] { ".package", ".zip" }) picker.FileTypeFilter.Add(x);
        var files = await picker.PickMultipleFilesAsync();
        if (files is null || files.Count == 0) return;
        await InstallFilesAsync(files.Select(f => f.Path).ToList());
    }

    void OnModFilesOver(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        e.AcceptedOperation = DataPackageOperation.Copy;
        e.DragUIOverride.Caption = T("Add to {0}", PlaceTitle(IsFolder(_modPlace) ? _modPlace : NewFolderParent()).Title);
    }

    async void OnModFilesDrop(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        var items = await e.DataView.GetStorageItemsAsync();
        await InstallFilesAsync(items.Select(i => i.Path).Where(p => p is { Length: > 0 }).ToList());
    }

    void OnModSearchFocus(object sender, RoutedEventArgs e) =>
        ModSearchBox.BorderBrush = ModSearch.FocusState != FocusState.Unfocused ? Res("ApexAccentBrush") : Res("ApexInputBorderBrush");

    // the list follows the search once typing pauses (each keystroke refiltered every mod before)
    Microsoft.UI.Dispatching.DispatcherQueueTimer? _searchTimer;
    void OnModSearch(object sender, TextChangedEventArgs e)
    {
        if (!_modsLoaded || _fillingMods) return;
        if (_searchTimer is null)
        {
            _searchTimer = DispatcherQueue.CreateTimer();
            _searchTimer.Interval = TimeSpan.FromMilliseconds(200);
            _searchTimer.IsRepeating = false;
            _searchTimer.Tick += (_, _) => FillMods();
        }
        _searchTimer.Stop();
        _searchTimer.Start();
    }

    // Backspace in an empty search takes away the last filter chip
    void OnModSearchKey(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != Windows.System.VirtualKey.Back || ModSearch.Text.Length > 0) return;
        if (_fTags.Count > 0) _fTags.Remove(_fTags.Last());
        else if (_fStates.Count > 0) _fStates.Remove(_fStates.Last());
        else if (_fKinds.Count > 0) _fKinds.Remove(_fKinds.Last());
        else if (_fFiles.Count > 0) _fFiles.Remove(_fFiles.Last());
        else if (_fCreators.Count > 0) _fCreators.Remove(_fCreators.Last());
        else if (_fAddedDays != 0) _fAddedDays = 0;
        else if (_fSizeMb > 0) _fSizeMb = 0;
        else return;
        e.Handled = true;
        FillMods();
    }

    void OnModsFind(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; ModSearch.Focus(FocusState.Keyboard); }
    void OnModsView(object sender, RoutedEventArgs e) { _modsGrid = sender == ModsViewGallery; SaveViewPrefs(); FillMods(); }
    void OnModPicked(object sender, SelectionChangedEventArgs e)
    {
        foreach (var r in e.AddedItems.OfType<ModRow>()) r.Picked = true;
        foreach (var r in e.RemovedItems.OfType<ModRow>()) r.Picked = false;
        if (!_fillingMods) { FillModDetails(); FillFooter(); }
    }

    // a row's checkbox: in or out of the selection, the rest stays picked
    void OnModCheck(ModRow r)
    {
        var list = _modsGrid ? (ListViewBase)ModGrid : ModList;
        if (list.SelectedItems.Contains(r)) list.SelectedItems.Remove(r); else list.SelectedItems.Add(r);
        r.Picked = list.SelectedItems.Contains(r);
    }
    void OnModsSelectAll(object sender, RoutedEventArgs e) { if (_modsGrid) ModGrid.SelectAll(); else ModList.SelectAll(); }
    void OnModsSelectNone(object sender, RoutedEventArgs e) => CloseModDetails();
    void OnModsOpenFolder(object sender, RoutedEventArgs e) => OpenFolder(_modPlace switch
    {
        AsiMods => _game?.Bin ?? "", OffMods => Mods.Disabled(Docs),
        _ when IsFolder(_modPlace) => Path.Combine(Docs, "Mods", _modPlace), _ => Path.Combine(Docs, "Mods"),
    });

    void FillFooter()
    {
        var picked = Picked();
        ModsFooter.Text = string.Join(" · ", new[]
        {
            _modShown.Count == 1 ? T("1 shown") : T("{0} shown", _modShown.Count),
            picked.Count > 0 ? T("{0} selected", picked.Count) : null,
            picked.Count > 0 ? ModRow.SizeText(picked.Sum(r => r.File.Size)) : null,
        }.Where(x => x is not null));
        ModsSelectNone.Visibility = picked.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        FillSelectionBar(picked);
    }

    // ---------- the filters ----------

    // the chips in the search box: every filter in use, each with a button that takes it away
    void FillChips()
    {
        ModChips.Children.Clear();
        void Chip(string text, string? icon, string? color, Action remove)
        {
            var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            if (color is not null) s.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 6, Height = 6, Fill = TagDot.BrushOf(color), VerticalAlignment = VerticalAlignment.Center });
            else if (icon is not null) s.Children.Add(Lucide.Icon(icon, 12, Res("ApexSoftTextBrush")));
            s.Children.Add(new TextBlock { Text = text, FontSize = 12, VerticalAlignment = VerticalAlignment.Center });
            s.Children.Add(Lucide.Icon("x", 11, Res("ApexMutedBrush")));
            var b = new Button
            {
                Content = s, Height = 24, Padding = new Thickness(8, 0, 6, 0), CornerRadius = new CornerRadius(6), BorderThickness = new Thickness(0),
                Background = color is not null ? TagDot.BrushOf(color, 0x26) : Res("ApexAccentSoftBrush"),
            };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, T("Remove filter {0}", text));
            b.Click += (_, _) => { remove(); FillMods(); };
            ModChips.Children.Add(b);
        }
        foreach (var f in _fFiles.ToList()) Chip(FileTypeName(f), FileTypeIcon(f), null, () => _fFiles.Remove(f));
        foreach (var k in _fKinds.ToList()) Chip(KindLabel(k), KindIcon(k), null, () => _fKinds.Remove(k));
        foreach (var s in _fStates.ToList()) Chip(StateName(s), StateIcon(s), null, () => _fStates.Remove(s));
        foreach (var t in _fTags.ToList()) Chip(t == "" ? T("Untagged") : t, "tag", t == "" ? null : ModNotes.ColorOf(t), () => _fTags.Remove(t));
        foreach (var c in _fCreators.ToList()) Chip(c == "" ? T("Unknown creator") : c, "users", null, () => _fCreators.Remove(c));
        if (_fAddedDays != 0) Chip(AddedLabel(_fAddedDays), "calendar-clock", null, () => _fAddedDays = 0);
        if (_fSizeMb > 0) Chip(T("Over {0} MB", _fSizeMb), "hard-drive", null, () => _fSizeMb = 0);
        ModChipsScroll.Visibility = ModChips.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        var n = FilterCount;
        ModFilterCount.Visibility = n > 0 ? Visibility.Visible : Visibility.Collapsed;
        ModFilterCountText.Text = n.ToString();
        ModSearch.PlaceholderText = n > 0 ? T("Search") : T("Search mods");
    }

    // added within d days (d > 0), or more than -d days ago (d < 0)
    static bool AddedWithin(ModFile m, int d) => d > 0 ? Added(m) >= DateTime.Now.AddDays(-d) : Added(m) < DateTime.Now.AddDays(d);
    static string AddedOption(int d) => d switch
    {
        1 => T("Today"), 7 => T("In the last 7 days"), 30 => T("In the last 30 days"), 90 => T("In the last 3 months"),
        182 => T("In the last 6 months"), 365 => T("In the last year"), _ => T("Over a year ago"),
    };
    static string AddedLabel(int d) => d switch
    {
        1 => T("Added today"), 7 => T("Added this week"), 30 => T("Added this month"), 90 => T("Added in 3 months"),
        182 => T("Added in 6 months"), 365 => T("Added this year"), _ => T("Added over a year ago"),
    };

    int FilterCount => _fFiles.Count + _fKinds.Count + _fStates.Count + _fTags.Count + _fCreators.Count + (_fAddedDays != 0 ? 1 : 0) + (_fSizeMb > 0 ? 1 : 0);

    static string FileTypeName(string f) => f == "asi" ? T("ASI plugin") : T("Package");
    static string FileTypeIcon(string f) => f == "asi" ? "plug" : "package";
    static string StateName(string s) => s switch { "on" => T("On"), "off" => T("Off"), "look" => T("Needs a look"), "new" => T("New"), "default" => T("Default replacement"), _ => T("Duplicate") };
    static string StateIcon(string s) => s switch { "on" => "power", "off" => "power-off", "look" => "triangle-alert", "new" => "sparkles", "default" => "replace", _ => "copy" };

    void OnModFiltersOpening(object sender, object e) => FillFilterPanel();

    // two columns: the groups (with how many choices each has in use), then the chosen group's choices with their counts
    void FillFilterPanel()
    {
        var panel = ModFilterPanel;
        panel.Children.Clear();
        panel.RowDefinitions.Clear();
        panel.ColumnDefinitions.Clear();
        panel.RowDefinitions.Add(new RowDefinition());
        panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        panel.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(176) });
        panel.ColumnDefinitions.Add(new ColumnDefinition());
        var inPlace = _modRows.Where(r => InPlace(r, _modPlace)).ToList();

        var groups = new (string Label, string Icon, int InUse)[]
        {
            (T("File type"), "package", _fFiles.Count), (T("What it is"), "blocks", _fKinds.Count), (T("State"), "power", _fStates.Count),
            (T("Tags"), "tag", _fTags.Count), (T("Creator"), "users", _fCreators.Count), (T("Added"), "calendar-clock", _fAddedDays != 0 ? 1 : 0), (T("Size"), "hard-drive", _fSizeMb > 0 ? 1 : 0),
        };
        var nav = new StackPanel { Spacing = 2, Padding = new Thickness(8), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 0, 1, 0) };
        for (int i = 0; i < groups.Length; i++)
        {
            var (label, icon, used) = groups[i];
            var at = i;
            var g = new Grid { ColumnSpacing = 9 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(Lucide.Icon(icon, 15, Res(i == _filterGroup ? "ApexAccentBrush" : "ApexMutedBrush")));
            var t = new TextBlock { Text = label, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            Grid.SetColumn(t, 1);
            g.Children.Add(t);
            if (used > 0)
            {
                var badge = new Border { MinWidth = 16, Height = 16, CornerRadius = new CornerRadius(8), Background = Res("ApexAccentBrush"),
                    Child = new TextBlock { Text = used.ToString(), FontSize = 10, FontWeight = FontWeights.Bold, Foreground = Res("TextOnAccentFillColorPrimaryBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } };
                Grid.SetColumn(badge, 2);
                g.Children.Add(badge);
            }
            var b = new Button { Content = g, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, Height = 34, Padding = new Thickness(10, 0, 10, 0),
                BorderThickness = new Thickness(0), Background = i == _filterGroup ? Res("ApexAccentSoftBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, label);
            b.Click += (_, _) => { _filterGroup = at; FillFilterPanel(); };
            nav.Children.Add(b);
        }
        panel.Children.Add(nav);

        var list = new StackPanel { Spacing = 1, Padding = new Thickness(8, 8, 10, 8) };
        void Head(string text) => list.Children.Add(new TextBlock { Text = text, FontSize = 10, FontWeight = FontWeights.SemiBold, CharacterSpacing = 80, Foreground = Res("ApexMutedBrush"), Margin = new Thickness(8, 10, 0, 2) });
        void Opt(string label, string? icon, string? color, int count, bool on, Action<bool> set)
        {
            var g = new Grid { ColumnSpacing = 10 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(color is not null ? new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 9, Height = 9, Fill = TagDot.BrushOf(color), Margin = new Thickness(3, 0, 3, 0), VerticalAlignment = VerticalAlignment.Center }
                : icon is not null ? Lucide.Icon(icon, 15, Res(on ? "ApexAccentBrush" : "ApexMutedBrush")) : new Border { Width = _filterGroup >= 4 ? 0 : 15 }); // (creators, dates, sizes: no icon column at all)
            var t = new TextBlock { Text = label, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            Grid.SetColumn(t, 1);
            g.Children.Add(t);
            var n = new TextBlock { Text = count.ToString(), FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(n, 2);
            g.Children.Add(n);
            g.VerticalAlignment = VerticalAlignment.Center;
            var box = new CheckBox { Content = g, IsChecked = on, MinWidth = 0, MinHeight = 36, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
                VerticalContentAlignment = VerticalAlignment.Center, Padding = new Thickness(8, 0, 4, 0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, label);
            box.Click += (_, _) => { set(box.IsChecked == true); FillMods(); FillFilterPanel(); };
            list.Children.Add(box);
        }
        static void Flip(HashSet<string> set, string v, bool on) { if (on) set.Add(v); else set.Remove(v); }
        switch (_filterGroup)
        {
            case 0:
                foreach (var f in new[] { "package", "asi" })
                    if (inPlace.Count(r => r.File.FileType == f) is var n && (n > 0 || _fFiles.Contains(f))) Opt(FileTypeName(f), FileTypeIcon(f), null, n, _fFiles.Contains(f), on => Flip(_fFiles, f, on));
                break;
            case 1:
                // (an icon two kinds share tells them apart from nothing: those rows go without)
                var kinds = inPlace.SelectMany(r => r.Kinds).Concat(_fKinds).Distinct().ToList();
                var shared = kinds.GroupBy(KindIcon).Where(x => x.Count() > 1).Select(x => x.Key).ToHashSet();
                foreach (var grp in kinds.GroupBy(KindGroup).OrderBy(x => KindGroupOrder(x.Key)))
                {
                    Head(KindGroupName(grp.Key));
                    foreach (var k in grp.Distinct().OrderBy(KindLabel))
                        Opt(KindLabel(k), shared.Contains(KindIcon(k)) ? null : KindIcon(k), null, inPlace.Count(r => r.Kinds.Contains(k)), _fKinds.Contains(k), on => Flip(_fKinds, k, on));
                }
                break;
            case 2:
                foreach (var s in new[] { "on", "off", "look", "new", "dup", "default" })
                {
                    var n = inPlace.Count(r => s switch { "on" => r.File.Enabled, "off" => !r.File.Enabled, "look" => NeedsLook(r), "new" => r.IsNew, "default" => r.Defaults.IsDefault, _ => r.DuplicateOf is not null });
                    if (n > 0 || _fStates.Contains(s)) Opt(StateName(s), StateIcon(s), null, n, _fStates.Contains(s), on => Flip(_fStates, s, on));
                }
                break;
            case 3:
                foreach (var t in ModNotes.Tags) Opt(t.Name, null, t.Color, inPlace.Count(r => ModNotes.TagsOf(r.File.Rel).Contains(t.Name, StringComparer.OrdinalIgnoreCase)), _fTags.Contains(t.Name), on => Flip(_fTags, t.Name, on));
                Opt(T("Untagged"), "tag", null, inPlace.Count(r => ModNotes.TagsOf(r.File.Rel).Count == 0), _fTags.Contains(""), on => Flip(_fTags, "", on));
                if (ModNotes.Tags.Count == 0) list.Children.Add(new TextBlock { Text = T("No tags yet: make one from a mod's details or the side pane."), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(8) });
                break;
            case 4:
                foreach (var c in inPlace.GroupBy(r => CreatorOf(r.File) ?? "").OrderBy(g => g.Key == "" ? 1 : 0).ThenByDescending(g => g.Count()).ThenBy(g => g.Key, StringComparer.OrdinalIgnoreCase))
                    Opt(c.Key == "" ? T("Unknown creator") : c.Key, null, null, c.Count(), _fCreators.Contains(c.Key), on => Flip(_fCreators, c.Key, on));
                break;
            case 5:
                foreach (var d in new[] { 0, 1, 7, 30, 90, 182, 365, -365 })
                    Opt(d == 0 ? T("Any time") : AddedOption(d), null, null, d == 0 ? inPlace.Count : inPlace.Count(r => AddedWithin(r.File, d)), _fAddedDays == d, on => _fAddedDays = on ? d : 0);
                break;
            default:
                foreach (var mb in new[] { 0, 10, 50, 200 })
                    Opt(mb == 0 ? T("Any size") : T("Over {0} MB", mb), null, null, mb == 0 ? inPlace.Count : inPlace.Count(r => r.File.Size >= (long)mb << 20), _fSizeMb == mb, on => _fSizeMb = on ? mb : 0);
                break;
        }
        var scroll = new ScrollViewer { Content = list, MaxHeight = 400 };
        Grid.SetColumn(scroll, 1);
        panel.Children.Add(scroll);

        var foot = new Grid { Padding = new Thickness(14, 10, 12, 10), ColumnSpacing = 12, BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.Children.Add(new TextBlock { Text = _modShown.Count == 1 ? T("1 mod matches") : T("{0} mods match", _modShown.Count), FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
        var clear = new HyperlinkButton { Content = T("Clear all"), FontSize = 12, Padding = new Thickness(0), IsEnabled = FilterCount > 0 };
        clear.Click += (_, _) => { _fFiles.Clear(); _fKinds.Clear(); _fStates.Clear(); _fTags.Clear(); _fCreators.Clear(); _fAddedDays = _fSizeMb = 0; FillMods(); FillFilterPanel(); };
        Grid.SetColumn(clear, 1);
        foot.Children.Add(clear);
        var ok = new Button { Content = T("Done"), Style = (Style)Application.Current.Resources["AccentButtonStyle"] };
        ok.Click += (_, _) => ModFilterFlyout.Hide();
        Grid.SetColumn(ok, 2);
        foot.Children.Add(ok);
        Grid.SetRow(foot, 1);
        Grid.SetColumnSpan(foot, 2);
        panel.Children.Add(foot);
    }

    // a kind's name, icon and group: ModKinds.Kind names, and the older ModKind names for files ModKinds does not read
    // the package reader names build surfaces "BuildWall", "BuildFence"...: read as "Wall", "Fence" here (without this
    // every one of them fell to "Other")
    static readonly HashSet<string> Surfaces = ["Wall", "WallPattern", "FloorPattern", "Fence", "Railing", "Stairs", "RoofStyle", "RoofPattern", "Foundation", "Pool", "TerrainPaint", "TerrainTool", "Water"];
    static string Bare(string k) => k.StartsWith("Build") && Surfaces.Contains(k[5..]) ? k[5..] : k;

    static string KindLabel(string k) => Bare(k) switch
    {
        "Script" => T("Script mod"), "CoreMod" => T("Core mod"), "Tuning" => T("Tuning"), "Skill" => T("Skills"), "Buff" => T("Moodlets"),
        "Hair" => T("Hair"), "Clothing" => T("Clothing"), "Accessories" => T("Accessories"), "Makeup" => T("Makeup"), "FacialHair" => T("Facial hair"),
        "Eyebrows" => T("Eyebrows"), "Eyes" => T("Eyes"), "SkinDetails" => T("Skin details"), "Tattoos" => T("Tattoos"), "SkinTones" => T("Skin tones"),
        "Sliders" => T("Sliders"), "Pets" => T("Pets"), "CasOther" or "Cas" => T("Create a Sim"),
        "BuyAppliances" => T("Appliances"), "BuyElectronics" => T("Electronics"), "BuyEntertainment" => T("Entertainment"), "BuyLighting" => T("Lights"),
        "BuyPlumbing" => T("Plumbing"), "BuyDecor" => T("Decor"), "BuyKids" => T("Kids"), "BuyStorage" => T("Storage"), "BuyComfort" => T("Comfort"),
        "BuySurfaces" => T("Surfaces"), "BuyVehicles" => T("Vehicles"), "BuyPets" => T("Pet objects"), "BuyShowStage" => T("Show stage"), "BuyResort" => T("Resort"),
        "BuyDebug" => T("Debug objects"), "BuyMisc" or "Objects" => T("Objects"), "BuyModular" => T("Modular objects"),
        "BuildDoor" => T("Doors"), "BuildWindow" => T("Windows"), "BuildGate" => T("Gates"), "BuildColumn" => T("Columns"), "BuildRabbitHole" => T("Rabbit holes"),
        "BuildFireplace" => T("Fireplaces"), "BuildChimney" => T("Chimneys"), "BuildArch" => T("Arches"), "BuildFlower" => T("Flowers"), "BuildShrub" => T("Shrubs"),
        "BuildTree" => T("Trees"), "BuildRug" => T("Rugs"), "BuildRock" => T("Rocks"), "BuildShell" => T("Shells"), "BuildLandmark" => T("Landmarks"),
        "BuildElevator" => T("Elevators"), "BuildSpiralStaircase" => T("Spiral stairs"), "BuildBlueprint" => T("Blueprints"), "BuildResortObjects" => T("Resort build"),
        "BuildModularArch" => T("Modular arches"), "Wall" => T("Walls"), "WallPattern" => T("Wall patterns"), "FloorPattern" => T("Floors"), "Fence" => T("Fences"),
        "Railing" => T("Railings"), "Stairs" => T("Stairs"), "RoofStyle" => T("Roofs"), "RoofPattern" => T("Roof patterns"), "Foundation" => T("Foundations"),
        "Pool" => T("Pools and fountains"), "TerrainPaint" => T("Terrain paints"), "TerrainTool" => T("Terrain tools"), "Water" => T("Water"),
        "Lighting" => T("Lighting"), "Effects" => T("Effects"), "Patterns" => T("Patterns"), "Animations" => T("Animations and poses"), "Sound" => T("Music and sound"),
        "Interface" => T("Interface"), "Textures" => T("Textures"), "Translations" => T("Translations"),
        "Asi" => T("ASI plugin"),
        _ => T("Other"),
    };

    static string KindIcon(string k) => Bare(k) switch
    {
        "Script" => "code-xml", "CoreMod" => "cpu", "Tuning" => "sliders-vertical", "Skill" => "graduation-cap", "Buff" => "heart-pulse",
        "Hair" => "scissors", "Clothing" => "shirt", "Accessories" => "gem", "Makeup" => "brush", "FacialHair" or "Eyebrows" => "scan-face", "Eyes" => "eye",
        "SkinDetails" or "SkinTones" => "droplet", "Sliders" => "person-standing", "Tattoos" => "heart-pulse", "Pets" or "BuyPets" => "paw-print", "CasOther" or "Cas" => "shirt",
        "BuyLighting" => "lightbulb", "BuyModular" => "blocks", "BuyElectronics" => "cpu", "BuyKids" => "puzzle", "BuyPlumbing" => "waves", "BuyDecor" => "palette",
        "BuildDoor" or "BuildWindow" or "BuildGate" or "BuildColumn" or "BuildArch" or "BuildModularArch" => "hammer", "BuildFireplace" or "BuildChimney" => "flame",
        "BuildFlower" or "BuildShrub" or "BuildTree" => "mountain", "Wall" or "WallPattern" => "brick-wall", "FloorPattern" => "layout-grid", "Fence" or "Railing" => "fence",
        "Stairs" or "BuildSpiralStaircase" or "BuildElevator" => "layers", "RoofStyle" or "RoofPattern" or "Foundation" => "house", "Pool" or "Water" => "waves",
        "TerrainPaint" or "TerrainTool" => "mountain", "Lighting" => "lamp", "Effects" => "wand-sparkles", "Patterns" => "palette", "Animations" => "person-standing",
        "Sound" => "music", "Interface" => "app-window", "Textures" => "image", "Translations" => "languages", "Asi" => "plug",
        _ when k.StartsWith("Buy") || k == "Objects" => "sofa", _ when k.StartsWith("Build") => "hammer",
        _ => "puzzle",
    };

    static int KindGroup(string k) => k switch
    {
        "Script" or "CoreMod" or "Tuning" or "Skill" or "Buff" => 0,
        "Hair" or "Clothing" or "Accessories" or "Makeup" or "FacialHair" or "Eyebrows" or "Eyes" or "SkinDetails" or "Tattoos" or "SkinTones" or "Sliders" or "Pets" or "CasOther" or "Cas" => 1,
        _ when k.StartsWith("Buy") || k == "Objects" => 2,
        _ when k.StartsWith("Build") || k is "Wall" or "WallPattern" or "FloorPattern" or "Fence" or "Railing" or "Stairs" or "RoofStyle" or "RoofPattern" or "Foundation" or "Pool" or "TerrainPaint" or "TerrainTool" or "Water" => 3,
        "Lighting" or "Effects" or "Patterns" or "Animations" or "Sound" or "Interface" or "Textures" or "Translations" => 4,
        _ => 6,
    };
    static int KindGroupOrder(int g) => g;
    static string KindGroupName(int g) => g switch
    {
        0 => T("GAMEPLAY"), 1 => T("CREATE A SIM"), 2 => T("BUY MODE"), 3 => T("BUILD MODE"), 4 => T("LOOK AND SOUND"), 5 => T("PLACES AND PEOPLE"), _ => T("OTHER"),
    };

    void FillSortText() => ModSortText.Text = SortNames()[Math.Clamp(_modSort, 0, 5)];
    static string[] SortNames() => [T("Name"), T("Size"), T("Type"), T("Folder"), T("Newest"), T("Needs a look first")];

    void OnModSortClick(object sender, RoutedEventArgs e)
    {
        var menu = new MenuFlyout();
        var names = SortNames();
        for (int i = 0; i < names.Length; i++)
        {
            var at = i;
            var item = new RadioMenuFlyoutItem { Text = names[i], IsChecked = i == _modSort, GroupName = "sort" };
            item.Click += (_, _) => { _modSort = at; SaveViewPrefs(); FillMods(); };
            menu.Items.Add(item);
        }
        menu.ShowAt(ModSortButton);
    }

    // ---------- the selection bar, the row menu and keys ----------

    // while the details float over the list (a medium window), the bar slides into the free part on their left and fits
    // its width there; it slides back when they close
    void PlaceSelectionBar()
    {
        if (ModSelectionBar.Parent is not FrameworkElement list) return;
        var over = _modsNarrow && ModDetailsPane.Visibility == Visibility.Visible ? ModDetailsPane.Width : 0;
        ModSelectionBar.MaxWidth = over > 0 ? Math.Max(160, list.ActualWidth - over - 24) : double.PositiveInfinity;
        ModSelectionBar.Translation = new System.Numerics.Vector3((float)(-over / 2), 0, 24);
    }

    void FillSelectionBar(List<ModRow> picked)
    {
        var mine = picked.Where(Mine).ToList();
        ModSelectionBar.Visibility = picked.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
        PlaceSelectionBar();
        if (picked.Count < 2) return;
        var a = ModSelectionActions;
        a.Children.Clear();
        a.Children.Add(new TextBlock { Text = T("{0} selected", picked.Count), FontSize = 13, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) });
        Button Act(string icon, string tip, Action? run, bool enabled = true, string? brush = null)
        {
            var b = Dims(new Button { Content = Lucide.Icon(icon, 16, brush is null ? null : Res(brush)), Width = 34, Height = 34, Padding = new Thickness(0), IsEnabled = enabled });
            ToolTipService.SetToolTip(b, tip);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, tip);
            if (run is not null) b.Click += (_, _) => run();
            a.Children.Add(b);
            return b;
        }
        var allOn = mine.Count > 0 && mine.All(r => r.File.Enabled);
        Act(allOn ? "power-off" : "power", allOn ? T("Turn off") : T("Turn on"), () => SetMods(mine, !allOn), mine.Count > 0);
        var mv = Act("folder-input", T("Move to…"), null, mine.Count > 0);
        mv.Click += (_, _) => MoveMenu(mine).ShowAt(mv);
        var tg = Act("tag", T("Tag"), null);
        tg.Click += (_, _) => TagMenu(picked).ShowAt(tg);
        Act("merge", T("Merge into one package…"), () => Safe(MergeAsync(mine, null, null)), mine.Count > 1);
        Act("trash-2", T("Remove to the Recycle Bin"), () => RemoveMods(mine), mine.Count > 0, "ApexErrorBrush");
        a.Children.Add(new Border { Width = 1, Height = 20, Background = Res("ApexCardBorderBrush"), Margin = new Thickness(4, 0, 4, 0) });
        Act("x", T("Clear the selection"), CloseModDetails);
    }

    void OnModRightTapped(object sender, RightTappedRoutedEventArgs e)
    {
        if ((e.OriginalSource as FrameworkElement)?.DataContext is not ModRow row) return;
        var view = (ListViewBase)sender;
        if (!view.SelectedItems.Contains(row)) { view.SelectedItems.Clear(); view.SelectedItems.Add(row); }
        var picked = Picked();
        var mine = picked.Where(Mine).ToList();
        var menu = new MenuFlyout();
        if (mine.Count > 0)
        {
            var on = mine.All(r => r.File.Enabled);
            menu.Items.Add(MenuItem(on ? T("Turn off") : T("Turn on"), () => SetMods(mine, !on), on ? "power-off" : "power", Windows.System.VirtualKey.Space));
        }
        if (picked.Count == 1 && mine.Count == 1) menu.Items.Add(MenuItem(T("Rename"), () => Safe(RenameModAsync(mine[0])), "pencil", Windows.System.VirtualKey.F2));
        if (mine.Count > 0)
        {
            var move = new MenuFlyoutSubItem { Text = T("Move to"), Icon = MenuIcon("folder-input") };
            foreach (var i in MoveMenu(mine).Items.ToList()) move.Items.Add(i);
            menu.Items.Add(move);
        }
        var tag = new MenuFlyoutSubItem { Text = T("Tag"), Icon = MenuIcon("tag") };
        foreach (var i in TagMenu(picked).Items.ToList()) tag.Items.Add(i);
        menu.Items.Add(tag);
        if (mine.Count > 1) menu.Items.Add(MenuItem(T("Merge into one package…"), () => Safe(MergeAsync(mine, null, null)), "merge"));
        if (picked.Count == 1 && ModMerge.IsMerged(picked[0].File)) menu.Items.Add(MenuItem(T("Split back into its mods…"), () => Safe(SplitAsync(picked[0])), "merge"));
        if (picked.Count == 1) menu.Items.Add(MenuItem(T("Show in Explorer"), () => ShowFile(picked[0].File.Path), "folder-search"));
        menu.Items.Add(MenuItem(picked.Count == 1 ? T("Copy name") : T("Copy names"), () => CopyText(string.Join(Environment.NewLine, picked.Select(r => r.File.Name))), "copy"));
        if (mine.Count > 0)
        {
            menu.Items.Add(new MenuFlyoutSeparator());
            menu.Items.Add(MenuItem(T("Remove"), () => RemoveMods(mine), "trash-2", Windows.System.VirtualKey.Delete, danger: true));
        }
        menu.ShowAt(view, e.GetPosition(view));
        e.Handled = true;
    }

    static void CopyText(string text)
    {
        var p = new DataPackage();
        p.SetText(text);
        Clipboard.SetContent(p);
    }

    void OnModListKey(object sender, KeyRoutedEventArgs e)
    {
        var picked = Picked();
        if (picked.Count == 0) return;
        var mine = picked.Where(Mine).ToList();
        switch (e.Key)
        {
            case Windows.System.VirtualKey.Space when mine.Count > 0: SetMods(mine, !mine.All(r => r.File.Enabled)); break;
            case Windows.System.VirtualKey.F2 when mine.Count == 1 && picked.Count == 1: Safe(RenameModAsync(mine[0])); break;
            case Windows.System.VirtualKey.Delete when mine.Count > 0: RemoveMods(mine); break;
            case Windows.System.VirtualKey.Escape: CloseModDetails(); break;
            default: return;
        }
        e.Handled = true;
    }

    MenuFlyout MoveMenu(List<ModRow> rows)
    {
        var menu = new MenuFlyout();
        menu.Items.Add(MenuItem("Mods", () => MoveMods(rows, ""), "folder"));
        foreach (var d in _modDirs)
            menu.Items.Add(MenuItem(new string(' ', 4 * d.Count(c => c == Path.DirectorySeparatorChar)) + Path.GetFileName(d), () => MoveMods(rows, d), "folder"));
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(MenuItem(T("New folder…"), () => Safe(MoveToNewFolderAsync(rows)), "folder-plus"));
        return menu;
    }

    async Task MoveToNewFolderAsync(List<ModRow> rows)
    {
        if (await AskName(T("New folder"), T("New folder"), T("Create")) is not { } name) return;
        try { var dir = ModLibrary.CreateFolder(Docs, NewFolderParent(), name); _modDirs.Add(dir); MoveMods(rows, dir); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
    }

    // ---------- the details pane ----------

    // wide: side pane, list and details side by side; medium: the details slide over the list while mods are chosen;
    // narrow: the side pane becomes a menu and the list drops its type and size columns
    bool _modsNarrow, _modsCompact;
    void OnModsSized(object sender, SizeChangedEventArgs e)
    {
        var w = e.NewSize.Width;
        var narrow = w < 1100;
        var compact = w < 780;
        DispatcherQueue.TryEnqueue(PlaceSelectionBar);
        if (narrow == _modsNarrow && compact == _modsCompact && e.PreviousSize.Width > 0) return;
        _modsNarrow = narrow;
        _modsCompact = compact;
        ModColFolders.Width = new GridLength(compact ? 0 : narrow ? 220 : 248);
        ModSide.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
        ModsPlaces.Visibility = compact ? Visibility.Visible : Visibility.Collapsed;
        ModsHuntText.Visibility = narrow ? Visibility.Collapsed : Visibility.Visible;
        ModColDetails.Width = new GridLength(narrow ? 0 : 340);
        Grid.SetColumn(ModDetailsPane, narrow ? 1 : 2);
        ModDetailsPane.Shadow = narrow ? new ThemeShadow() : null;
        ModDetailsPane.Translation = new System.Numerics.Vector3(0, 0, narrow ? 32 : 0);
        // (the type and size columns go on a small window; the type stays as the row's icon)
        ModRow.KindColumn = new GridLength(compact ? 0 : 140);
        ModRow.SizeColumn = new GridLength(compact ? 0 : 72);
        ModHeadKind.Width = ModRow.KindColumn;
        ModHeadSize.Width = ModRow.SizeColumn;
        ModHeadKindText.Visibility = ModHeadSizeText.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
        if (_modsLoaded)
        {
            // (the rows are made again so their columns take the new widths)
            _fillingMods = true;
            var keep = _modShown.ToList();
            _modShown.Clear();
            foreach (var r in keep) _modShown.Add(r);
            _fillingMods = false;
        }
        FillModDetails();
    }

    // the tiles fill the row: as many columns of at least 168 px as fit, sharing the width, the picture square
    void OnModGridSized(object sender, SizeChangedEventArgs e) => FitModGrid();
    void FitModGrid()
    {
        if (ModGrid.ItemsPanelRoot is not ItemsWrapGrid panel) return;
        var width = ModGrid.ActualWidth - ModGrid.Padding.Left - ModGrid.Padding.Right - 2;
        if (width <= 0) return;
        var cell = Math.Floor(width / Math.Max(1, (int)(width / 168)));
        panel.ItemWidth = cell;
        panel.ItemHeight = cell + 52;
    }

    void CloseModDetails() { ModList.SelectedItems.Clear(); ModGrid.SelectedItems.Clear(); FillModDetails(); FillFooter(); }

    // the player's choice of type, else the package's own
    static void ApplyKinds(ModRow r)
    {
        var manual = ModNotes.Get(r.File.Rel)?.Kinds;
        r.Kinds = manual is { Count: > 0 } ? manual : r.DetectedKinds;
        r.KindOverride = manual is { Count: > 0 } ? KindLabel(manual[0]) : null;
        r.KindIconOverride = manual is { Count: > 0 } ? KindIcon(manual[0]) : null;
    }

    // every type, by group, ticked as the mod is now; Done saves it, Back to detected forgets the choice
    void PickKinds(ModRow r, FrameworkElement anchor)
    {
        var picked = r.Kinds.ToHashSet();
        var list = new StackPanel { Spacing = 2 };
        foreach (var group in Enum.GetNames<Kind>().Where(k => k != nameof(Kind.Other)).Concat(["Asi"])
                     .GroupBy(KindGroup).OrderBy(g => KindGroupOrder(g.Key)))
        {
            list.Children.Add(new TextBlock { Text = KindGroupName(group.Key), FontSize = 11, CharacterSpacing = 60, Foreground = Res("ApexMutedBrush"), Margin = new Thickness(4, list.Children.Count == 0 ? 0 : 12, 0, 4) });
            foreach (var k in group.OrderBy(KindLabel, StringComparer.CurrentCultureIgnoreCase))
            {
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                row.Children.Add(Lucide.Icon(KindIcon(k), 14, Res("ApexMutedBrush")));
                row.Children.Add(new TextBlock { Text = KindLabel(k), FontSize = 13 });
                var box = new CheckBox { Content = row, IsChecked = picked.Contains(k), MinWidth = 0, Padding = new Thickness(8, 0, 0, 0) };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, KindLabel(k));
                box.Checked += (_, _) => picked.Add(k);
                box.Unchecked += (_, _) => picked.Remove(k);
                list.Children.Add(box);
            }
        }
        var flyout = new Flyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.Left };
        void Save(IEnumerable<string>? kinds)
        {
            try
            {
                // (ticking exactly what the package says is no choice: it stays automatic)
                var same = kinds is not null && kinds.ToHashSet().SetEquals(r.DetectedKinds);
                ModNotes.SetKinds(r.File.Rel, same ? null : kinds?.OrderBy(k => r.Kinds.Contains(k) ? r.Kinds.ToList().IndexOf(k) : int.MaxValue));
                ApplyKinds(r);
                r.Refresh();
                flyout.Hide();
                FillMods();
                FillModDetails();
            }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        }
        var back = new HyperlinkButton { Content = T("Back to detected"), FontSize = 12, Padding = new Thickness(0), VerticalAlignment = VerticalAlignment.Center,
            IsEnabled = ModNotes.Get(r.File.Rel)?.Kinds is not null };
        back.Click += (_, _) => Save(null);
        var done = new Button { Content = T("Done"), Style = (Style)Application.Current.Resources["AccentButtonStyle"], HorizontalAlignment = HorizontalAlignment.Right };
        done.Click += (_, _) => { if (picked.Count == 0) Save(null); else Save(picked); };
        var foot = new Grid { Padding = new Thickness(12, 10, 12, 10), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        foot.Children.Add(back);
        foot.Children.Add(done);
        var body = new Grid { Width = 300 };
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition());
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.Children.Add(new TextBlock { Text = T("What is {0}?", r.Name), FontSize = 14, FontWeight = FontWeights.SemiBold, Margin = new Thickness(16, 14, 16, 8), TextTrimming = TextTrimming.CharacterEllipsis });
        var scroll = new ScrollViewer { Content = list, MaxHeight = 420, Padding = new Thickness(12, 0, 16, 8) };
        Grid.SetRow(scroll, 1);
        body.Children.Add(scroll);
        Grid.SetRow(foot, 2);
        body.Children.Add(foot);
        flyout.Content = body;
        flyout.FlyoutPresenterStyle = new Style(typeof(FlyoutPresenter)) { Setters = { new Setter(Control.PaddingProperty, new Thickness(0)), new Setter(FrameworkElement.MaxWidthProperty, 360.0) } };
        flyout.ShowAt(anchor);
    }

    void FillModDetails()
    {
        var host = ModDetails;
        host.Children.Clear();
        ModDetailsHead.Children.Clear();
        ModDetailsFoot.Children.Clear();
        ModDetailsFoot.ColumnDefinitions.Clear();
        ModDetailsFoot.Visibility = Visibility.Collapsed;
        var picked = Picked();
        ModDetailsPane.Visibility = _modsNarrow && picked.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        // (over the grid the pane would hide whole columns of tiles: the grid makes room for it instead)
        var room = _modsNarrow && ModDetailsPane.Visibility == Visibility.Visible ? ModDetailsPane.Width : 0;
        if (ModGrid.Padding.Right != 24 + room) { ModGrid.Padding = new Thickness(0, 0, 24 + room, 72); FitModGrid(); }
        PlaceSelectionBar();
        if (picked.Count == 0)
        {
            var empty = new StackPanel { Spacing = 12, Margin = new Thickness(0, 56, 0, 0), HorizontalAlignment = HorizontalAlignment.Center };
            var icon = Lucide.Icon("layout-grid", 30, Res("ApexMutedBrush"), 1.5);
            icon.HorizontalAlignment = HorizontalAlignment.Center;
            empty.Children.Add(icon);
            var t = Muted(T("Choose a mod to see what it is, add a picture, notes or tags, rename, move or remove it. Choose several to change them together, or drag them onto a folder or tag."));
            t.TextAlignment = TextAlignment.Center;
            t.MaxWidth = 250;
            empty.Children.Add(t);
            host.Children.Add(empty);
            return;
        }
        if (picked.Count > 1) { FillManyDetails(picked); return; }
        FillOneModDetails(picked[0]);
    }

    // an icon button that is off reads as off: its icon keeps a fixed colour, so the whole button fades
    static T Dims<T>(T b) where T : Control
    {
        void Show() => b.Opacity = b.IsEnabled ? 1 : 0.35;
        b.IsEnabledChanged += (_, _) => Show();
        Show();
        return b;
    }

    static Button IconButton(string icon, string tip, string? brush = null)
    {
        var b = new Button { Content = Lucide.Icon(icon, 15, brush is null ? null : Res(brush)), Width = 32, Height = 32, Padding = new Thickness(0),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, tip);
        ToolTipService.SetToolTip(b, tip);
        return Dims(b);
    }

    // (brush: the icon and the text in that colour, as the red of Remove)
    static Button WideButton(string icon, string text, Action click, string? brush = null)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        s.Children.Add(Lucide.Icon(icon, 15, brush is null ? null : Res(brush)));
        var label = new TextBlock { Text = text, TextTrimming = TextTrimming.CharacterEllipsis };
        if (brush is not null) label.Foreground = Res(brush);
        s.Children.Add(label);
        var b = new Button { Content = s, HorizontalAlignment = HorizontalAlignment.Stretch, Height = 34 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, text);
        b.Click += (_, _) => click();
        return Dims(b);
    }

    static Button SmallButton(string text, string icon, Action click)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        s.Children.Add(Lucide.Icon(icon, 13));
        s.Children.Add(new TextBlock { Text = text, FontSize = 12 });
        var b = new Button { Content = s, Padding = new Thickness(10, 4, 10, 5), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(210, 0x14, 0x14, 0x14)) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, text);
        b.Click += (_, _) => click();
        return Dims(b);
    }

    static Border Warn(string title, string? text, string? action = null, Action? run = null)
    {
        var s = new StackPanel { Spacing = 4 };
        var head = new Grid { ColumnSpacing = 8 };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.Children.Add(Lucide.Icon("triangle-alert", 15, Res("ApexWarningBrush")));
        var t = new TextBlock { Text = title, FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexWarningBrush"), TextWrapping = TextWrapping.Wrap };
        Grid.SetColumn(t, 1);
        head.Children.Add(t);
        s.Children.Add(head);
        if (text is not null) s.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexWarningTextBrush"), TextWrapping = TextWrapping.Wrap });
        if (action is not null && run is not null) s.Children.Add(Link(action, run));
        return new Border
        {
            Child = s, Padding = new Thickness(12), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1),
            Background = Res("ApexWarningSoftBrush"), BorderBrush = Res("ApexWarningLineBrush"),
        };
    }

    void SetPicture(ModRow r, string? file)
    {
        try
        {
            ModNotes.SetPicture(r.File.Rel, file);
            r.Image = null;
            Safe(LoadModPicturesAsync([r]).ContinueWith(_ => DispatcherQueue.TryEnqueue(FillModDetails)));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The picture was not saved"), ErrorText(e)); }
    }

    async Task RenameModAsync(ModRow r)
    {
        if (await AskName(T("Rename mod"), r.Name, T("Rename")) is { } name) RenameMod(r, name);
    }

    void RenameMod(ModRow r, string name)
    {
        var old = r.Name;
        try
        {
            Replace(r, ModLibrary.Rename(r.File, name));
            ModsResult(T("Renamed to {0}", r.Name), () =>
            {
                try { Replace(r, ModLibrary.Rename(r.File, old)); }
                catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
                FillMods();
            });
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        FillMods();
    }

    async void RemoveMods(List<ModRow> rows)
    {
        rows = rows.Where(Mine).ToList();
        if (rows.Count == 0) return;
        var merged = rows.Count(r => ModMerge.IsMerged(r.File));
        var text = (rows.Count == 1 ? T("{0} goes to the Recycle Bin. You can restore it from there.", rows[0].Name)
            : T("{0} mods go to the Recycle Bin. You can restore them from there.", rows.Count))
            + (merged > 0 ? "\n\n" + T("A merged mod's originals stay kept in Mods\\{0}; use Split instead to get them back.", ModMerge.FolderName) : "")
            + (rows.Any(HasItems) ? "\n\n" + T("Lots and Sims that use its objects or outfits lose them the next time a game is saved without it.") : "");
        if (!await Confirm(rows.Count == 1 ? T("Remove this mod?") : T("Remove these mods?"), text, T("Remove"), danger: true)) return;
        await ModsChange(() => { foreach (var r in rows) ModLibrary.Remove(r.File); }, rows.Count == 1 ? T("Mod moved to the Recycle Bin") : T("{0} mods moved to the Recycle Bin", rows.Count));
    }

    static void ShowFile(string path)
    {
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true }); }
        catch (Exception e) { App.Log(e); }
    }

    // ---------- a default replacement ----------

    // the game items it replaces: objects and CAS parts by name, each with the game's picture and the mod's (when it brings
    // one); the first few here, every one of them in See all
    UIElement ReplacesPanel(ModRow r, GameIndex gi)
    {
        var d = r.Defaults;
        var panel = new StackPanel { Spacing = 8 };
        var all = d.Objects.Concat(d.CasParts).ToList();
        // (a mod of textures only has no items to show: one line says what it changes)
        if (all.Count == 0) { panel.Children.Add(Muted(ReplacesSummary(d))); return panel; }
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(22) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        TextBlock H(string text, int col) { var x = new TextBlock { Text = text, FontSize = 11, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = col == 3 ? HorizontalAlignment.Left : HorizontalAlignment.Center }; Grid.SetColumn(x, col); return x; }
        head.Children.Add(H(T("Game"), 0));
        head.Children.Add(H(T("Mod"), 2));
        panel.Children.Add(head);
        const int shown = 8;
        foreach (var it in all.Take(shown)) panel.Children.Add(ReplacedRow(it, gi, r.File.Path, d.Objects.Contains(it)));
        if (all.Count > shown)
        {
            var more = new Button { Content = T("See all {0}", all.Count), HorizontalAlignment = HorizontalAlignment.Stretch, Margin = new Thickness(0, 4, 0, 0) };
            more.Click += (_, _) => Safe(SeeAllReplacedAsync(r, gi, all));
            panel.Children.Add(more);
        }
        return panel;
    }

    // one replaced item: the game's picture, an arrow, the mod's picture (or "same" when it brings none), its name
    Grid ReplacedRow(ReplacedItem it, GameIndex gi, string package, bool isObject)
    {
        var g = new Grid { ColumnSpacing = 0 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(22) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        var (game, mod) = (ThumbBox(isObject), ThumbBox(isObject));
        g.Children.Add(game);
        var arrow = Lucide.Icon("chevron-right", 14, Res("ApexMutedBrush"));
        arrow.HorizontalAlignment = HorizontalAlignment.Center;
        arrow.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(arrow, 1);
        g.Children.Add(arrow);
        Grid.SetColumn(mod, 2);
        g.Children.Add(mod);
        var name = new TextBlock { Text = it.Name ?? T("A game item"), FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, MaxLines = 2, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 0, 0) };
        ToolTipService.SetToolTip(name, $"{it.Name}  {it.Type:X8}:{it.Group:X8}:{it.Instance:X16}");
        Grid.SetColumn(name, 3);
        g.Children.Add(name);
        LoadReplacedThumbs(it, gi, package, game, mod);
        return g;
    }

    static Border ThumbBox(bool isObject, double size = 40)
    {
        var ph = Lucide.Icon(isObject ? "sofa" : "shirt", size * 0.4, Res("ApexMutedBrush"));
        ph.HorizontalAlignment = HorizontalAlignment.Center;
        ph.VerticalAlignment = VerticalAlignment.Center;
        return new Border { Width = size, Height = size, CornerRadius = new CornerRadius(6), Background = Res("ApexPillBrush"), Child = ph };
    }

    void LoadReplacedThumbs(ReplacedItem it, GameIndex gi, string package, Border game, Border mod)
    {
        _ = Task.Run(() => (DefaultReplacements.OriginalThumbnail(gi, it), DefaultReplacements.ModThumbnail(package, it))).ContinueWith(async t =>
        {
            try
            {
                if (!t.IsCompletedSuccessfully) return;
                var (g, m) = t.Result;
                if (g is not null && await SaveCard.ImageOf(g) is { } gimg) game.Child = new Image { Source = gimg, Stretch = Stretch.UniformToFill };
                if (m is not null && await SaveCard.ImageOf(m) is { } mimg) mod.Child = new Image { Source = mimg, Stretch = Stretch.UniformToFill };
                else
                {
                    // (no picture of its own: the game keeps showing its own for it)
                    mod.Child = new TextBlock { Text = T("same"), FontSize = 10, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                    ToolTipService.SetToolTip(mod, T("The mod brings no picture of its own for it: the game shows its own."));
                }
            }
            catch (Exception e) { App.Log(e); }
        }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    // every item it replaces, searchable, as tiles: the game's picture and the mod's side by side; pictures load as the
    // tiles come into view
    async Task SeeAllReplacedAsync(ModRow r, GameIndex gi, List<ReplacedItem> all)
    {
        var grid = new GridView { SelectionMode = ListViewSelectionMode.None, Height = 520, Padding = new Thickness(0, 4, 0, 0) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(grid, T("What it replaces"));
        var loaded = new HashSet<ReplacedItem>();
        StackPanel Tile(ReplacedItem it)
        {
            var isObject = r.Defaults.Objects.Contains(it);
            var pics = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, HorizontalAlignment = HorizontalAlignment.Center };
            var (game, mod) = (ThumbBox(isObject, 72), ThumbBox(isObject, 72));
            pics.Children.Add(game);
            pics.Children.Add(mod);
            var tile = new StackPanel { Width = 156, Spacing = 6, Padding = new Thickness(4, 6, 4, 6), Tag = (it, game, mod) };
            tile.Children.Add(pics);
            tile.Children.Add(new TextBlock { Text = it.Name ?? T("A game item"), FontSize = 11, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis, HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center });
            ToolTipService.SetToolTip(tile, $"{it.Name}  {it.Type:X8}:{it.Group:X8}:{it.Instance:X16}");
            return tile;
        }
        var tiles = all.Select(Tile).ToList();
        grid.ContainerContentChanging += (_, args) =>
        {
            if (args.InRecycleQueue || args.Item is not StackPanel { Tag: (ReplacedItem it, Border game, Border mod) } || !loaded.Add(it)) return;
            LoadReplacedThumbs(it, gi, r.File.Path, game, mod);
        };
        grid.ItemsSource = tiles;
        var search = new TextBox { PlaceholderText = T("Search by name") };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(search, T("Search by name"));
        var count = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
        void Filter()
        {
            var q = search.Text.Trim();
            var shown = q.Length == 0 ? tiles : tiles.Where(x => x.Tag is (ReplacedItem it, Border, Border) && (it.Name ?? "").Contains(q, StringComparison.OrdinalIgnoreCase)).ToList();
            grid.ItemsSource = shown;
            count.Text = shown.Count == tiles.Count ? T("{0} items · game picture, then the mod's", tiles.Count) : T("{0} of {1}", shown.Count, tiles.Count);
        }
        search.TextChanged += (_, _) => Filter();
        Filter();
        var top = new Grid { ColumnSpacing = 12 };
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(260) });
        top.ColumnDefinitions.Add(new ColumnDefinition());
        top.Children.Add(search);
        Grid.SetColumn(count, 1);
        top.Children.Add(count);
        var body = new StackPanel { Spacing = 12, Width = 860 };
        body.Children.Add(top);
        body.Children.Add(grid);
        var d = new ContentDialog { Title = T("What {0} replaces", r.Name), Content = body, CloseButtonText = T("Close"), XamlRoot = Content.XamlRoot };
        d.Resources["ContentDialogMaxWidth"] = 940.0;
        await ShowDialog(d);
    }

    // ---------- what a package holds ----------

    UIElement InsidePanel(ModRow r)
    {
        var panel = new StackPanel { Spacing = 8 };
        panel.Children.Add(new TextBlock { Text = T("INSIDE"), FontSize = 11, FontWeight = FontWeights.SemiBold, CharacterSpacing = 60, Foreground = Res("ApexMutedBrush") });
        var path = r.File.Path;
        void Fill(ModContents c)
        {
            while (panel.Children.Count > 1) panel.Children.RemoveAt(1);
            if (c.Parts.Count == 0 && c.Names.Count == 0)
            {
                panel.Children.Add(Muted(r.File.Kind == ModKind.Script ? T("Game code and settings: no objects or Create a Sim parts.") : T("No objects or Create a Sim parts: it changes what the game already has.")));
                return;
            }
            var wrap = new WrapPanel { Spacing = 6 };
            foreach (var (kind, n) in c.Parts) wrap.Children.Add(Pill(PartsText(kind, n), "ApexSoftTextBrush"));
            panel.Children.Add(wrap);
            var names = c.Names.Count > 0 ? c.Names : c.PartNames;
            const int shown = 8;
            foreach (var n in names.Take(shown))
                panel.Children.Add(new TextBlock { Text = "•  " + n, FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            var more = c.Parts.Sum(x => x.Count) - Math.Min(shown, names.Count);
            if (names.Count > 0 && more > 0) panel.Children.Add(Muted(T("and {0} more", more)));
        }
        if (_modContents.TryGetValue(path, out var known)) Fill(known);
        else
        {
            panel.Children.Add(Muted(T("Reading what it holds…")));
            // (a damaged package of any kind reads as empty, never an error box)
            _ = Task.Run(() => { try { return ModContent.Read(path); } catch (Exception e) { App.Log(e); return new ModContents([], [], []); } })
                .ContinueWith(t => DispatcherQueue.TryEnqueue(() => { _modContents[path] = t.Result; Fill(t.Result); }));
        }
        return panel;
    }

    // "12 objects", "1 hairstyle"
    static string PartsText(string kind, int n) => (kind, n == 1) switch
    {
        ("Object", true) => T("1 object"), ("Object", _) => T("{0} objects", n),
        ("Hair", true) => T("1 hairstyle"), ("Hair", _) => T("{0} hairstyles", n),
        ("Outfit", true) => T("1 outfit"), ("Outfit", _) => T("{0} outfits", n),
        ("Top", true) => T("1 top"), ("Top", _) => T("{0} tops", n),
        ("Bottom", true) => T("1 bottom"), ("Bottom", _) => T("{0} bottoms", n),
        ("Shoes", true) => T("1 pair of shoes"), ("Shoes", _) => T("{0} pairs of shoes", n),
        ("Accessory", true) => T("1 accessory"), ("Accessory", _) => T("{0} accessories", n),
        ("Facial hair", true) => T("1 facial hair"), ("Facial hair", _) => T("{0} facial hair", n),
        ("Makeup", true) => T("1 makeup"), ("Makeup", _) => T("{0} makeup", n),
        ("Eyebrows", true) => T("1 eyebrows"), ("Eyebrows", _) => T("{0} eyebrows", n),
        ("Eye colour", true) => T("1 eye colour"), ("Eye colour", _) => T("{0} eye colours", n),
        ("Skin detail", true) => T("1 skin detail"), ("Skin detail", _) => T("{0} skin details", n),
        ("Tattoo", true) => T("1 tattoo"), ("Tattoo", _) => T("{0} tattoos", n),
        (_, true) => T("1 Create a Sim part"), _ => T("{0} Create a Sim parts", n),
    };

    // ---------- tags ----------

    UIElement TagsRow(List<ModRow> rows)
    {
        var wrap = new WrapPanel { Spacing = 6 };
        var all = ModNotes.Tags.Where(t => rows.All(r => ModNotes.TagsOf(r.File.Rel).Contains(t.Name, StringComparer.OrdinalIgnoreCase))).ToList();
        foreach (var t in all)
        {
            var chip = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            chip.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 7, Height = 7, Fill = TagDot.BrushOf(t.Color), VerticalAlignment = VerticalAlignment.Center });
            chip.Children.Add(new TextBlock { Text = t.Name, FontSize = 12, Foreground = TagDot.BrushOf(t.Color), VerticalAlignment = VerticalAlignment.Center });
            var x = new Button { Content = Lucide.Icon("x", 10, TagDot.BrushOf(t.Color)), Padding = new Thickness(3), MinWidth = 0, MinHeight = 0, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(x, T("Remove tag {0}", t.Name));
            var name = t.Name;
            x.Click += (_, _) => TagMods(rows, name, false);
            chip.Children.Add(x);
            wrap.Children.Add(new Border { Child = chip, Height = 28, Padding = new Thickness(10, 0, 4, 0), CornerRadius = new CornerRadius(14), Background = TagDot.BrushOf(t.Color, 0x26) });
        }
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        content.Children.Add(Lucide.Icon("tag", 12));
        content.Children.Add(new TextBlock { Text = T("Add tag"), FontSize = 12 });
        var add = new Button { Content = content, Height = 28, Padding = new Thickness(10, 0, 12, 0), CornerRadius = new CornerRadius(14), VerticalAlignment = VerticalAlignment.Center };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(add, T("Add tag"));
        add.Click += (_, _) => TagMenu(rows).ShowAt(add);
        wrap.Children.Add(add);
        return wrap;
    }

    MenuFlyout TagMenu(List<ModRow> rows)
    {
        var menu = new MenuFlyout();
        foreach (var t in ModNotes.Tags)
        {
            var name = t.Name;
            var on = rows.All(r => ModNotes.TagsOf(r.File.Rel).Contains(name, StringComparer.OrdinalIgnoreCase));
            var item = new ToggleMenuFlyoutItem { Text = name, IsChecked = on, Icon = MenuIcon("tag", TagDot.BrushOf(t.Color)) };
            item.Click += (_, _) => TagMods(rows, name, item.IsChecked);
            menu.Items.Add(item);
        }
        if (ModNotes.Tags.Count > 0) menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(MenuItem(T("New tag…"), () => Safe(NewTagAsync(rows)), "plus"));
        return menu;
    }

    void TagMods(List<ModRow> rows, string tag, bool on)
    {
        // (Undo takes the tag off only the mods that did not have it)
        var added = rows.Where(r => !ModNotes.TagsOf(r.File.Rel).Contains(tag, StringComparer.OrdinalIgnoreCase)).ToList();
        try
        {
            foreach (var r in rows) { ModNotes.SetTag(r.File.Rel, tag, on); r.Refresh(); }
            if (rows.Count > 1 && on) ModsResult(T("{0} mods tagged {1}", rows.Count, tag), () => TagMods(added, tag, false));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        _dragRows = null;
        _folderSig = "";
        FillMods();
    }

    // a tag's name and colour from the player; null when cancelled
    async Task<(string Name, string Color)?> AskTag(string title, string name, string color, string yes)
    {
        var box = new TextBox { Text = name, PlaceholderText = T("Tag name"), MinWidth = 320, MaxLength = 40 };
        var colors = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var chosen = color;
        var rings = new List<Border>();
        foreach (var c in ModNotes.Colors)
        {
            var ring = new Border { Width = 32, Height = 32, CornerRadius = new CornerRadius(16), BorderThickness = new Thickness(2), Padding = new Thickness(3), Tag = c };
            var dot = new Button { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), Padding = new Thickness(0), MinWidth = 0, MinHeight = 0, Background = TagDot.BrushOf(c), BorderThickness = new Thickness(0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(dot, T("Colour {0}", Array.IndexOf(ModNotes.Colors, c) + 1));
            var cc = c;
            dot.Click += (_, _) => { chosen = cc; foreach (var x in rings) x.BorderBrush = (string)x.Tag == chosen ? Res("ApexSoftTextBrush") : null; };
            ring.Child = dot;
            ring.BorderBrush = c == chosen ? Res("ApexSoftTextBrush") : null;
            rings.Add(ring);
            colors.Children.Add(ring);
        }
        var body = new StackPanel { Spacing = 14 };
        body.Children.Add(box);
        body.Children.Add(colors);
        var d = new ContentDialog { Title = title, Content = body, PrimaryButtonText = yes, CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        return await ShowDialog(d) == ContentDialogResult.Primary && box.Text.Trim().Length > 0 ? (box.Text, chosen) : null;
    }

    async Task NewTagAsync(List<ModRow>? rows)
    {
        var next = ModNotes.Colors[ModNotes.Tags.Count % ModNotes.Colors.Length];
        if (await AskTag(T("New tag"), "", next, T("Create")) is not { } t) return;
        try
        {
            ModNotes.CreateTag(t.Name, t.Color);
            if (rows is not null) TagMods(rows, t.Name.Trim(), true);
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        _folderSig = "";
        FillMods();
    }

    async Task EditTagAsync(string name)
    {
        if (await AskTag(T("Edit tag"), name, ModNotes.ColorOf(name), T("Save")) is not { } t) return;
        try
        {
            ModNotes.EditTag(name, t.Name, t.Color);
            if (_modPlace.Equals("#" + name, StringComparison.OrdinalIgnoreCase)) _modPlace = "#" + t.Name.Trim();
            if (_fTags.Remove(name)) _fTags.Add(t.Name.Trim());
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        foreach (var r in _modRows) r.Refresh();
        _folderSig = "";
        FillMods();
    }

    async Task DeleteTagAsync(string name)
    {
        if (!await Confirm(T("Delete this tag?"), T("{0} is taken off every mod. The mods stay as they are.", name), T("Delete"), danger: true)) return;
        try { ModNotes.DeleteTag(name); _fTags.Remove(name); } catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        foreach (var r in _modRows) r.Refresh();
        _folderSig = "";
        FillMods();
    }

    // ---------- remembered between visits ----------

    void LoadViewPrefs()
    {
        var p = ModViewPrefs.Load();
        _modPlace = p.Place ?? AllMods;
        _modsGrid = p.Grid;
        _modSort = p.Sort;
        _collapsed.Clear();
        foreach (var c in p.Collapsed) _collapsed.Add(c);
        ModSortButton.Click -= OnModSortClick;
        ModSortButton.Click += OnModSortClick;
    }

    void SaveViewPrefs() => ModViewPrefs.Save(new ModViewPrefs.Prefs(_modPlace, _modsGrid, _modSort, [.. _collapsed]));
}
