using System.Diagnostics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// The Mods screen: the player's folders on the left (and the places the launcher lists but does not manage), the mods of
// the chosen place in one list or grid, and the chosen mod's details on the right (picture, name, notes, what it is,
// what needs a look, on/off, remove). Moves, renames and removals go through ModLibrary; notes and pictures, ModNotes.
public sealed partial class MainWindow
{
    // the folders pane's places besides the player's own folders
    const string NewTag = "*newtag", AllMods = "*all", OffMods = "*off", AsiMods = "*asi", PackMods = "*sims3pack", LibraryMods = "*library", LookMods = "*look";
    string _modPlace = AllMods;
    List<ModRow> _modRows = [];
    Dictionary<string, List<string>> _modFindings = new(StringComparer.OrdinalIgnoreCase); // by path: the findings' texts
    Func<string, bool>? _modLoadedBy;
    List<string> _modDirs = [];
    string _folderSig = "";
    bool _fillingMods;
    List<ModRow>? _dragRows;

    async Task LoadModsAsync()
    {
        if (_game is not { } g || _modsLoading) return;
        _modsLoading = true;
        ModsSummary.Text = T("Reading your mods…");
        var docs = Docs;
        try
        {
            var (mods, found, dirs, loaded) = await Task.Run(() =>
            {
                var m = Mods.List(g, docs);
                return (m, ModChecks.Run(m, g, docs), ModLibrary.Folders(docs), ModLibrary.LoadedBy(docs));
            });
            _mods = mods;
            _modFindings = new(StringComparer.OrdinalIgnoreCase);
            foreach (var f in found)
                foreach (var file in f.Files)
                    (_modFindings.TryGetValue(file, out var l) ? l : _modFindings[file] = []).Add(T(f.Finding.Text, f.Finding.Args));
            _modLooks = _modFindings.Keys.ToHashSet(StringComparer.OrdinalIgnoreCase);
            _modDirs = dirs;
            _modLoadedBy = loaded;
            _modRows = mods.Select(m => new ModRow(m, _modFindings.TryGetValue(m.Path, out var l) ? l : [])).ToList();
            _modsLoaded = true;
            _ = LoadModPicturesAsync(_modRows);
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The launcher could not read your mods"), ErrorText(e)); }
        finally { _modsLoading = false; }
        FillMods();
    }

    // the player's own picture first, else the one inside the package; one at a time, behind the list
    static async Task LoadModPicturesAsync(List<ModRow> rows)
    {
        foreach (var r in rows)
        {
            try
            {
                var bytes = await Task.Run(() => ModNotes.PicturePath(r.File.Rel) is { } own ? File.ReadAllBytes(own) : Mods.Thumbnail(r.File));
                r.Image = await SaveCard.ImageOf(bytes);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    }

    // a package's folder under Mods ("Packages\NRaas"; "" for Mods itself)
    static string DirOf(ModFile m) => m.FileType != "package" ? "" : Path.GetRelativePath("Mods", m.Folder) is var r && r == "." ? "" : r;
    static bool InDir(ModFile m, string dir) => m.FileType == "package"
        && (DirOf(m).Equals(dir, StringComparison.OrdinalIgnoreCase) || DirOf(m).StartsWith(dir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase));
    bool Loaded(ModFile m) => m.FileType != "package" || _modLoadedBy is not { } by || by(Path.GetRelativePath("Mods", m.Rel));
    bool FolderLoaded(string dir) => _modLoadedBy is not { } by || by(Path.Combine(dir, "x.package"));

    void FillMods()
    {
        if (_game is null) { ModsSummary.Text = T("Choose the game first."); return; }
        if (!_modsLoaded) { _ = LoadModsAsync(); return; }
        _fillingMods = true;
        try
        {
            ModRow.Toggle = ToggleMod;
            FillFolders();
            if (ModState.Items.Count == 0)
            {
                foreach (var t in new[] { T("Any state"), T("On"), T("Off"), T("Needs a look") }) ModState.Items.Add(t);
                foreach (var t in new[] { T("Name"), T("Size"), T("Type"), T("Folder") }) ModSort.Items.Add(t);
                ModState.SelectedIndex = ModSort.SelectedIndex = 0;
            }
            var inPlace = _modRows.Where(r => InPlace(r.File)).ToList();
            // the kinds in this place, each with its count
            var kind = ModKindBox.SelectedItem is ComboBoxItem { Tag: string k } ? k : "";
            ModKindBox.Items.Clear();
            ModKindBox.Items.Add(new ComboBoxItem { Content = T("All types"), Tag = "" });
            foreach (var g in inPlace.GroupBy(r => r.File.Kind).OrderBy(x => ModRow.KindName(x.Key)))
                ModKindBox.Items.Add(new ComboBoxItem { Content = $"{ModRow.KindName(g.Key)}  {g.Count()}", Tag = g.Key.ToString() });
            ModKindBox.SelectedItem = ModKindBox.Items.OfType<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == kind) ?? ModKindBox.Items[0];

            var shown = Sorted(inPlace.Where(Matches)).ToList();
            var picked = Picked().Select(r => r.File.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
            var grid = ModsViewGallery.IsSelected;
            ModList.Visibility = ModListHead.Visibility = grid ? Visibility.Collapsed : Visibility.Visible;
            ModGrid.Visibility = grid ? Visibility.Visible : Visibility.Collapsed;
            ListViewBase view = grid ? ModGrid : ModList;
            ModRow.ShowPlace = _modPlace.StartsWith('*');
            view.ItemsSource = shown;
            foreach (var r in shown.Where(r => picked.Contains(r.File.Path))) view.SelectedItems.Add(r);
            ModsEmpty.Visibility = shown.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            ModsEmpty.Text = inPlace.Count == 0 ? T("Nothing here yet.") : T("No mod matches these filters.");

            // the header: where this is, what it holds
            var (crumb, title) = _modPlace switch
            {
                AllMods => ("", T("All mods")), OffMods => ("", T("Turned off")), AsiMods => ("", T("Game plugins")),
                PackMods => ("", "Sims3Packs"), LibraryMods => ("", T("Library")), LookMods => ("", T("Needs a look")),
                _ when _modPlace.StartsWith('#') => (T("Tag"), _modPlace[1..]),
                _ => ("Mods \u203A " + string.Join(" \u203A ", _modPlace.Split(Path.DirectorySeparatorChar).SkipLast(1)), Path.GetFileName(_modPlace)),
            };
            ModsCrumb.Text = crumb.TrimEnd(' ', '\u203A');
            ModsCrumb.Visibility = crumb.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            ModsTitle.Text = title;
            var off = inPlace.Count(r => !r.File.Enabled);
            ModsSummary.Text = T("{0} files \u00B7 {1}", inPlace.Count, ModRow.SizeText(inPlace.Sum(r => r.File.Size)))
                + (off > 0 ? " \u00B7 " + T("{0} off", off) : "")
                + (!_modPlace.StartsWith('*') && !_modPlace.StartsWith('#') && !FolderLoaded(_modPlace) ? " \u00B7 " + T("the game does not load this folder") : "");
            ModsAdd.IsEnabled = _modPlace is not (AsiMods or PackMods or LibraryMods or LookMods or OffMods) && !_modPlace.StartsWith('#');
            FillModDetails();
        }
        finally { _fillingMods = false; }
    }

    bool InPlace(ModFile m) => _modPlace switch
    {
        AllMods => m.FileType == "package",
        OffMods => !m.Enabled,
        AsiMods => m.FileType == "asi",
        PackMods => m.FileType == "sims3pack",
        LibraryMods => m.FileType == "library",
        LookMods => _modFindings.ContainsKey(m.Path),
        _ when _modPlace.StartsWith('#') => ModNotes.TagsOf(m.Rel).Contains(_modPlace[1..], StringComparer.OrdinalIgnoreCase),
        _ => InDir(m, _modPlace),
    };

    bool Matches(ModRow r)
    {
        var q = ModSearch.Text?.Trim() ?? "";
        var kind = ModKindBox.SelectedItem is ComboBoxItem { Tag: string k } ? k : "";
        return (kind == "" || r.File.Kind.ToString() == kind)
            && (q.Length == 0 || r.File.Rel.Contains(q, StringComparison.OrdinalIgnoreCase) || (ModNotes.Get(r.File.Rel)?.Notes?.Contains(q, StringComparison.OrdinalIgnoreCase) ?? false)
                || ModNotes.TagsOf(r.File.Rel).Any(t => t.Contains(q, StringComparison.OrdinalIgnoreCase)))
            && ModState.SelectedIndex switch { 1 => r.File.Enabled, 2 => !r.File.Enabled, 3 => r.Findings.Count > 0, _ => true };
    }

    IEnumerable<ModRow> Sorted(IEnumerable<ModRow> rows) => ModSort.SelectedIndex switch
    {
        1 => rows.OrderByDescending(r => r.File.Size),
        2 => rows.OrderBy(r => r.Kind).ThenBy(r => r.Name, StringComparer.OrdinalIgnoreCase),
        3 => rows.OrderBy(r => r.File.Folder, StringComparer.OrdinalIgnoreCase).ThenBy(r => r.Name, StringComparer.OrdinalIgnoreCase),
        _ => rows.OrderBy(r => r.Name, StringComparer.OrdinalIgnoreCase),
    };

    List<ModRow> Picked() => (ModsViewGallery.IsSelected ? ModGrid.SelectedItems : ModList.SelectedItems).OfType<ModRow>().ToList();

    // ---------- the folders pane ----------

    void FillFolders()
    {
        // every folder there is, and those only switched-off mods still name
        var dirs = _modDirs.Concat(_mods.Where(m => m.FileType == "package").Select(DirOf).Where(d => d.Length > 0))
            .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase).ToList();
        int Count(string place) { var keep = _modPlace; _modPlace = place; var n = _mods.Count(InPlace); _modPlace = keep; return n; }
        var places = new List<(string Key, string Label, int Depth, string Glyph, int N)> { (AllMods, T("All mods"), 0, "\uE8B7", Count(AllMods)) };
        foreach (var d in dirs) places.Add((d, Path.GetFileName(d), d.Count(c => c == Path.DirectorySeparatorChar), "\uE8B7", _mods.Count(m => InDir(m, d))));
        places.Add(("-", "", 0, "", 0));
        places.Add((OffMods, T("Turned off"), 0, "\uE7E8", Count(OffMods)));
        places.Add((AsiMods, T("Game plugins"), 0, "\uE943", Count(AsiMods)));
        places.Add((PackMods, "Sims3Packs", 0, "\uE7B8", Count(PackMods)));
        places.Add((LibraryMods, T("Library"), 0, "\uE8F1", Count(LibraryMods)));
        if (_modFindings.Count > 0) { places.Add(("-", "", 0, "", 0)); places.Add((LookMods, T("Needs a look"), 0, "\uE7BA", Count(LookMods))); }
        places.Add(("=", T("TAGS"), 0, "", 0));
        foreach (var t in ModNotes.Tags) places.Add(("#" + t.Name, t.Name, 0, t.Color, Count("#" + t.Name)));
        places.Add((NewTag, T("New tag"), 0, "\uE710", -1));
        if (!dirs.Contains(_modPlace, StringComparer.OrdinalIgnoreCase) && !places.Any(p => p.Key == _modPlace)) _modPlace = AllMods;

        var sig = string.Join("|", places.Select(p => $"{p.Key}:{p.N}:{p.Glyph}")) + Lang;
        if (sig == _folderSig) return;
        _folderSig = sig;
        var items = new List<FrameworkElement>();
        foreach (var p in places)
        {
            if (p.Key == "-") { items.Add(new Border { Height = 1, Background = Res("ApexCardBorderBrush"), Margin = new Thickness(4, 6, 4, 6), Tag = "-", IsHitTestVisible = false }); continue; }
            if (p.Key == "=") { items.Add(new TextBlock { Text = p.Label, FontSize = 11, FontWeight = FontWeights.SemiBold, CharacterSpacing = 60, Foreground = Res("ApexMutedBrush"), Margin = new Thickness(0, 14, 0, 4), Tag = "-", IsHitTestVisible = false }); continue; }
            var g = new Grid { ColumnSpacing = 10, Tag = p.Key, Padding = new Thickness(p.Key.StartsWith('*') ? 0 : 14 * (p.Depth + 1), 0, 0, 0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var look = p.Key == LookMods;
            var dim = !p.Key.StartsWith('*') && !FolderLoaded(p.Key);
            var tag = p.Key.StartsWith('#');
            g.Children.Add(tag ? new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 10, Height = 10, Fill = TagDot.BrushOf(p.Glyph), Margin = new Thickness(2, 0, 2, 0) }
                : new FontIcon { Glyph = p.Glyph, FontSize = 14, Foreground = Res(look ? "ApexWarningBrush" : "ApexMutedBrush") });
            var name = new TextBlock { Text = p.Label, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center, Opacity = dim ? 0.55 : 1 };
            if (look) name.Foreground = Res("ApexWarningBrush");
            Grid.SetColumn(name, 1);
            g.Children.Add(name);
            var n = new TextBlock { Text = p.N < 0 ? "" : p.N.ToString(), FontSize = 12, Foreground = Res(look ? "ApexWarningBrush" : "ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(n, 2);
            g.Children.Add(n);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(g, $"{p.Label} {p.N}");
            if (dim) ToolTipService.SetToolTip(g, T("The game does not load packages from this folder (Mods\\Resource.cfg)."));
            // a real folder takes dropped mods and has its own menu
            if (!p.Key.StartsWith('*'))
            {
                var dir = p.Key;
                g.AllowDrop = true;
                g.DragOver += (_, e) =>
                {
                    if (_dragRows is null) return;
                    e.AcceptedOperation = Windows.ApplicationModel.DataTransfer.DataPackageOperation.Move;
                    e.DragUIOverride.Caption = T("Move to {0}", Path.GetFileName(dir));
                };
                g.Drop += (_, _) => { if (_dragRows is { } rows) { _dragRows = null; MoveMods(rows, dir); } };
                var menu = new MenuFlyout();
                menu.Items.Add(MenuItem(T("New folder inside"), () => _ = NewFolderAsync(dir)));
                menu.Items.Add(MenuItem(T("Rename"), () => _ = RenameFolderAsync(dir)));
                menu.Items.Add(MenuItem(T("Open in Explorer"), () => OpenFolder(Path.Combine(Docs, "Mods", dir))));
                menu.Items.Add(new MenuFlyoutSeparator());
                menu.Items.Add(MenuItem(T("Delete folder"), () => _ = DeleteFolderAsync(dir)));
                g.ContextFlyout = menu;
            }
            // a tag takes dropped mods too (they get the tag), and has its own menu
            if (tag)
            {
                var t = p.Label;
                g.AllowDrop = true;
                g.DragOver += (_, e) =>
                {
                    if (_dragRows is null) return;
                    e.AcceptedOperation = Windows.ApplicationModel.DataTransfer.DataPackageOperation.Link;
                    e.DragUIOverride.Caption = T("Tag as {0}", t);
                };
                g.Drop += (_, _) => { if (_dragRows is { } rows) { _dragRows = null; TagMods(rows, t, true); } };
                var menu = new MenuFlyout();
                menu.Items.Add(MenuItem(T("Edit tag"), () => _ = EditTagAsync(t)));
                menu.Items.Add(MenuItem(T("Delete tag"), () => _ = DeleteTagAsync(t)));
                g.ContextFlyout = menu;
            }
            items.Add(g);
        }
        ModFolders.ItemsSource = items;
        ModFolders.SelectedItem = items.FirstOrDefault(i => (string)i.Tag == _modPlace);
    }

    static MenuFlyoutItem MenuItem(string text, Action click)
    {
        var i = new MenuFlyoutItem { Text = text };
        i.Click += (_, _) => click();
        return i;
    }

    void OnModFolderPicked(object sender, SelectionChangedEventArgs e)
    {
        if (_fillingMods) return;
        if (ModFolders.SelectedItem is not FrameworkElement { Tag: string key } || key == "-") { DispatcherQueue.TryEnqueue(() => { _folderSig = ""; FillMods(); }); return; }
        if (key == NewTag) { DispatcherQueue.TryEnqueue(() => { _folderSig = ""; FillMods(); _ = NewTagAsync(null); }); return; }
        if (key == _modPlace) return;
        _modPlace = key;
        ModList.SelectedItems.Clear();
        ModGrid.SelectedItems.Clear();
        DispatcherQueue.TryEnqueue(FillMods);
    }

    // a folder name from the player; null when cancelled
    async Task<string?> AskName(string title, string initial, string yes)
    {
        var box = new TextBox { Text = initial, SelectionStart = 0, SelectionLength = initial.Length, MinWidth = 320 };
        var d = new ContentDialog { Title = title, Content = box, PrimaryButtonText = yes, CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        return await ShowDialog(d) == ContentDialogResult.Primary && box.Text.Trim().Length > 0 ? box.Text : null;
    }

    // new folders go in the chosen folder, else in Packages (where the game loads them), else in Mods
    string NewFolderParent() => !_modPlace.StartsWith('*') ? _modPlace
        : _modDirs.FirstOrDefault(d => d.Equals("Packages", StringComparison.OrdinalIgnoreCase)) ?? "";

    void OnModsNewFolder(object sender, RoutedEventArgs e) => _ = NewFolderAsync(NewFolderParent());

    async Task NewFolderAsync(string parent)
    {
        if (await AskName(T("New folder"), T("New folder"), T("Create")) is not { } name) return;
        await ModsChange(() => { _modPlace = ModLibrary.CreateFolder(Docs, parent, name); }, T("Folder created"));
    }

    async Task RenameFolderAsync(string dir)
    {
        if (await AskName(T("Rename folder"), Path.GetFileName(dir), T("Rename")) is not { } name) return;
        await ModsChange(() =>
        {
            var to = ModLibrary.RenameFolder(Docs, dir, name);
            if (_modPlace.Equals(dir, StringComparison.OrdinalIgnoreCase) || _modPlace.StartsWith(dir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                _modPlace = to + _modPlace[dir.Length..];
        }, T("Folder renamed"));
    }

    async Task DeleteFolderAsync(string dir)
    {
        if (!await Confirm(T("Delete this folder?"), T("{0} is deleted. Only an empty folder can be deleted; your mods are never deleted with it.", Path.GetFileName(dir)), T("Delete"))) return;
        await ModsChange(() => { ModLibrary.DeleteFolder(Docs, dir); _modPlace = Path.GetDirectoryName(dir) is { Length: > 0 } up ? up : AllMods; }, T("Folder deleted"));
    }

    // a change to the folders or files: done, said on the bar, the list read again
    async Task ModsChange(Action work, string done)
    {
        try { work(); SetInfo(ModsInfo, InfoBarSeverity.Success, done, ""); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }

    // ---------- moving, switching, adding ----------

    void OnModDragStart(object sender, DragItemsStartingEventArgs e)
    {
        _dragRows = e.Items.OfType<ModRow>().Where(r => !r.File.Locked && r.File.FileType == "package").ToList();
        if (_dragRows.Count == 0) { e.Cancel = true; _dragRows = null; return; }
        e.Data.RequestedOperation = Windows.ApplicationModel.DataTransfer.DataPackageOperation.Move;
        e.Data.SetText(string.Join("\n", _dragRows.Select(r => r.Name)));
    }

    void MoveMods(List<ModRow> rows, string dir)
    {
        int n = 0;
        string? error = null;
        foreach (var r in rows.Where(r => !r.File.Locked && r.File.FileType == "package"))
        {
            try { var moved = ModLibrary.Move(r.File, Docs, dir); Replace(r, moved); n++; }
            catch (Exception e) { App.Log(e); error ??= ErrorText(e); }
        }
        if (error is not null) SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some mods were not moved"), error);
        else if (n > 0) SetInfo(ModsInfo, InfoBarSeverity.Success, n == 1 ? T("Mod moved to {0}", Path.GetFileName(dir) is { Length: > 0 } f ? f : "Mods") : T("{0} mods moved to {1}", n, Path.GetFileName(dir) is { Length: > 0 } f2 ? f2 : "Mods"), "");
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

    void SetMods(List<ModRow> rows, bool on)
    {
        foreach (var r in rows.Where(r => !r.File.Locked && r.File.Enabled != on))
            if (ToggleMod(r, on) is { } moved) r.Update(moved);
    }

    async void OnModsAdd(object sender, RoutedEventArgs e)
    {
        var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
        picker.FileTypeFilter.Add(".package");
        var files = await picker.PickMultipleFilesAsync();
        if (files is null || files.Count == 0) return;
        var dir = _modPlace == AllMods ? NewFolderParent() : _modPlace;
        await ModsChange(() => ModLibrary.Add(Docs, dir, files.Select(f => f.Path)), files.Count == 1 ? T("Mod added") : T("{0} mods added", files.Count));
    }

    void OnModSearch(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args) { if (_modsLoaded && !_fillingMods) FillMods(); }
    void OnModFilter(object sender, SelectionChangedEventArgs e) { if (_modsLoaded && !_fillingMods) FillMods(); }
    void OnModsView(SelectorBar sender, SelectorBarSelectionChangedEventArgs args) { if (_modsLoaded && !_fillingMods) FillMods(); }
    void OnModPicked(object sender, SelectionChangedEventArgs e) { if (!_fillingMods) FillModDetails(); }
    void OnModsOpenFolder(object sender, RoutedEventArgs e) =>
        OpenFolder(_modPlace switch { AsiMods => _game?.Bin ?? "", PackMods => Path.Combine(Docs, "Downloads"), LibraryMods => Path.Combine(Docs, "Library"), OffMods => Mods.Disabled(Docs), AllMods or LookMods => Path.Combine(Docs, "Mods"), _ => Path.Combine(Docs, "Mods", _modPlace) });

    // ---------- the details pane ----------

    // wide: folders, list and details side by side; narrow: the details slide over the list while a mod is chosen
    bool _modsNarrow;
    void OnModsSized(object sender, SizeChangedEventArgs e)
    {
        var narrow = e.NewSize.Width < 1060;
        if (narrow == _modsNarrow && e.PreviousSize.Width > 0) return;
        _modsNarrow = narrow;
        ModColFolders.Width = new GridLength(narrow ? 210 : 240);
        ModColDetails.Width = new GridLength(narrow ? 0 : 330);
        Grid.SetColumn(ModDetailsPane, narrow ? 1 : 2);
        ModDetailsPane.Shadow = narrow ? new ThemeShadow() : null;
        ModDetailsPane.Translation = new System.Numerics.Vector3(0, 0, narrow ? 32 : 0);
        FillModDetails();
    }

    void CloseModDetails() { ModList.SelectedItems.Clear(); ModGrid.SelectedItems.Clear(); FillModDetails(); }

    void FillModDetails()
    {
        var host = ModDetails;
        host.Children.Clear();
        var picked = Picked();
        ModDetailsPane.Visibility = _modsNarrow && picked.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        if (_modsNarrow && picked.Count > 0)
        {
            var close = new Button { Content = new FontIcon { Glyph = "\uE711", FontSize = 12 }, Width = 32, Height = 32, Padding = new Thickness(0), HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, -8, 0, -8) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(close, T("Close"));
            close.Click += (_, _) => CloseModDetails();
            host.Children.Add(close);
        }
        if (picked.Count == 0)
        {
            var empty = new StackPanel { Spacing = 10, Margin = new Thickness(0, 60, 0, 0), HorizontalAlignment = HorizontalAlignment.Center };
            empty.Children.Add(new FontIcon { Glyph = "\uE8A9", FontSize = 28, Foreground = Res("ApexMutedBrush") });
            var t = Muted(T("Choose a mod to see what it is, add a picture or notes, rename, move or remove it. Drag mods onto a folder to move them."));
            t.TextAlignment = TextAlignment.Center;
            t.MaxWidth = 240;
            empty.Children.Add(t);
            host.Children.Add(empty);
            return;
        }
        if (picked.Count > 1) { FillManyDetails(picked); return; }
        var r = picked[0];
        var m = r.File;
        var mine = !m.Locked && m.FileType == "package";

        // the picture: the player's own, else the package's; a button to add or change it
        var pic = new Grid { Height = 180, CornerRadius = new CornerRadius(10), Background = r.TileBrush };
        pic.Children.Add(new TextBlock { Text = r.Mono, FontSize = 30, FontWeight = FontWeights.Bold, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(160, 255, 255, 255)), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
        // (a package's own thumbnail is small: shown at twice its size, not blown up to the whole box)
        if (r.Image is { } img)
            pic.Children.Add(img is BitmapImage { PixelWidth: > 0 and < 160 } small
                ? new Image { Source = img, Stretch = Stretch.Uniform, Width = small.PixelWidth * 2, Height = small.PixelHeight * 2, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center }
                : new Image { Source = img, Stretch = Stretch.UniformToFill });
        var own = ModNotes.PicturePath(m.Rel) is not null;
        var picButtons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(10) };
        if (own) picButtons.Children.Add(SmallButton(T("Remove picture"), () => SetPicture(r, null)));
        picButtons.Children.Add(SmallButton(own ? T("Change picture") : T("Add picture"), async () =>
        {
            var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
            foreach (var x in new[] { ".png", ".jpg", ".jpeg", ".bmp", ".gif" }) picker.FileTypeFilter.Add(x);
            if (await picker.PickSingleFileAsync() is { } file) SetPicture(r, file.Path);
        }));
        pic.Children.Add(picButtons);
        host.Children.Add(pic);

        // the name, and renaming it
        var nameRow = new Grid { ColumnSpacing = 4 };
        nameRow.ColumnDefinitions.Add(new ColumnDefinition());
        nameRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        nameRow.Children.Add(new TextBlock { Text = r.Name, FontSize = 18, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center });
        if (mine)
        {
            var rename = new Button { Content = new FontIcon { Glyph = "\uE70F", FontSize = 14 }, Width = 32, Height = 32, Padding = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0), VerticalAlignment = VerticalAlignment.Top };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(rename, T("Rename"));
            ToolTipService.SetToolTip(rename, T("Rename"));
            rename.Click += async (_, _) =>
            {
                if (await AskName(T("Rename mod"), r.Name, T("Rename")) is not { } name) return;
                try { Replace(r, ModLibrary.Rename(r.File, name)); SetInfo(ModsInfo, InfoBarSeverity.Success, T("Mod renamed"), ""); }
                catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
                FillMods();
            };
            Grid.SetColumn(rename, 1);
            nameRow.Children.Add(rename);
        }
        host.Children.Add(nameRow);

        // notes, saved when the box loses focus
        var notes = new TextBox
        {
            Text = ModNotes.Get(m.Rel)?.Notes ?? "", PlaceholderText = T("Add notes: where it came from, what it changes…"), AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap, MinHeight = 72, MaxHeight = 220,
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(notes, T("Notes"));
        notes.LostFocus += (_, _) =>
        {
            try { ModNotes.SetNotes(r.File.Rel, notes.Text); r.Refresh(); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The notes were not saved"), ErrorText(e)); }
        };
        host.Children.Add(notes);
        host.Children.Add(TagsRow([r]));

        // what it is
        var facts = new Grid { ColumnSpacing = 12, RowSpacing = 8 };
        facts.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(70) });
        facts.ColumnDefinitions.Add(new ColumnDefinition());
        void Fact(string k, UIElement v)
        {
            facts.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var key = Muted(k);
            Grid.SetRow(key, facts.RowDefinitions.Count - 1);
            if (v is FrameworkElement fe) { Grid.SetRow(fe, facts.RowDefinitions.Count - 1); Grid.SetColumn(fe, 1); }
            facts.Children.Add(key);
            facts.Children.Add(v);
        }
        TextBlock Val(string t) => new() { Text = t, FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap };
        Fact(T("Type"), Val(r.Kind));
        Fact(T("File"), Val(m.Name));
        if (mine)
        {
            var where = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            where.Children.Add(Val(ModRow.Place(m)));
            var move = new HyperlinkButton { Content = T("Move…"), Padding = new Thickness(0), FontSize = 12 };
            var menu = MoveMenu([r]);
            move.Click += (_, _) => menu.ShowAt(move);
            where.Children.Add(move);
            Fact(T("Folder"), where);
        }
        else Fact(T("Where"), Val(ModRow.Place(m)));
        Fact(T("Size"), Val(r.Size));
        try { Fact(T("Changed"), Val(File.GetLastWriteTime(m.Path).ToString("d"))); } catch (Exception) { }
        Fact(T("State"), Val(m.Locked ? ModRow.LockText(m) : m.Enabled ? T("On") : T("Off: the game does not load it")));
        host.Children.Add(facts);
        if (m.FileType is "package" or "library") host.Children.Add(InsidePanel(r));

        // what needs a look
        if (m.Enabled && !Loaded(m))
            host.Children.Add(Warn(T("The game does not load this folder"), T("Mods\\Resource.cfg does not list it, so the game skips this mod. Move it into Packages (or a folder inside it) to use it.")));
        foreach (var f in r.Findings) host.Children.Add(Warn(f, null, T("See in Health"), () => Show(HealthScreen)));

        // actions
        var acts = new Grid { ColumnSpacing = 8, Margin = new Thickness(0, 8, 0, 0) };
        acts.ColumnDefinitions.Add(new ColumnDefinition());
        acts.ColumnDefinitions.Add(new ColumnDefinition());
        acts.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var show = Button(T("Show file"), () => ShowFile(m.Path));
        show.HorizontalAlignment = HorizontalAlignment.Stretch;
        if (!m.Locked)
        {
            var onOff = Button(m.Enabled ? T("Turn off") : T("Turn on"), () => SetMods([r], !r.File.Enabled));
            onOff.HorizontalAlignment = HorizontalAlignment.Stretch;
            acts.Children.Add(onOff);
            Grid.SetColumn(show, 1);
        }
        else Grid.SetColumnSpan(show, 2);
        acts.Children.Add(show);
        if (mine)
        {
            var remove = Button(T("Remove"), () => RemoveMods([r]), danger: true);
            Grid.SetColumn(remove, 2);
            acts.Children.Add(remove);
        }
        host.Children.Add(acts);
    }

    void FillManyDetails(List<ModRow> picked)
    {
        var host = ModDetails;
        var mine = picked.Where(r => !r.File.Locked && r.File.FileType == "package").ToList();
        host.Children.Add(new TextBlock { Text = T("{0} mods selected", picked.Count), FontSize = 18, FontWeight = FontWeights.SemiBold });
        host.Children.Add(Muted(ModRow.SizeText(picked.Sum(r => r.File.Size))
            + (mine.Count < picked.Count ? " \u00B7 " + T("{0} of them can't be changed here", picked.Count - mine.Count) : "")));
        if (mine.Count == 0) return;
        host.Children.Add(TagsRow(picked));
        var acts = new StackPanel { Spacing = 8 };
        Button Wide(Button b) { b.HorizontalAlignment = HorizontalAlignment.Stretch; return b; }
        acts.Children.Add(Wide(Button(T("Turn on"), () => SetMods(mine, true))));
        acts.Children.Add(Wide(Button(T("Turn off"), () => SetMods(mine, false))));
        var move = Wide(Button(T("Move to…"), () => { }));
        move.Flyout = MoveMenu(mine);
        acts.Children.Add(move);
        acts.Children.Add(Wide(Button(T("Remove"), () => RemoveMods(mine), danger: true)));
        host.Children.Add(acts);
        host.Children.Add(Muted(T("Tip: drag the selected mods onto a folder to move them.")));
    }

    // ---------- tags ----------

    // the tags of the chosen mods (on all of them), each with a remove button, and Add tag
    UIElement TagsRow(List<ModRow> rows)
    {
        var wrap = new WrapPanel { Spacing = 6 };
        var all = ModNotes.Tags.Where(t => rows.All(r => ModNotes.TagsOf(r.File.Rel).Contains(t.Name, StringComparer.OrdinalIgnoreCase))).ToList();
        foreach (var t in all)
        {
            var chip = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            chip.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = TagDot.BrushOf(t.Color), VerticalAlignment = VerticalAlignment.Center });
            chip.Children.Add(new TextBlock { Text = t.Name, FontSize = 12, VerticalAlignment = VerticalAlignment.Center });
            var x = new Button { Content = new FontIcon { Glyph = "\uE711", FontSize = 8 }, Padding = new Thickness(4), MinWidth = 0, MinHeight = 0, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(x, T("Remove tag {0}", t.Name));
            var name = t.Name;
            x.Click += (_, _) => TagMods(rows, name, false);
            chip.Children.Add(x);
            wrap.Children.Add(new Border { Child = chip, Padding = new Thickness(10, 2, 4, 2), CornerRadius = new CornerRadius(12), Background = Res("ApexPillBrush") });
        }
        var add = new Button { Content = Icon("\uE8EC", T("Add tag")), FontSize = 12, Padding = new Thickness(10, 3, 10, 4), CornerRadius = new CornerRadius(12) };
        var menu = new MenuFlyout();
        foreach (var t in ModNotes.Tags)
        {
            var name = t.Name;
            var item = new ToggleMenuFlyoutItem { Text = name, IsChecked = all.Contains(t), Icon = new FontIcon { Glyph = "\uEA3B", Foreground = TagDot.BrushOf(t.Color) } };
            item.Click += (_, _) => TagMods(rows, name, item.IsChecked);
            menu.Items.Add(item);
        }
        if (ModNotes.Tags.Count > 0) menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(MenuItem(T("New tag…"), () => _ = NewTagAsync(rows)));
        add.Click += (_, _) => menu.ShowAt(add);
        wrap.Children.Add(add);
        return wrap;
    }

    void TagMods(List<ModRow> rows, string tag, bool on)
    {
        try { foreach (var r in rows) { ModNotes.SetTag(r.File.Rel, tag, on); r.Refresh(); } }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
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
            var ring = new Border { Width = 32, Height = 32, CornerRadius = new CornerRadius(16), BorderThickness = new Thickness(2), Padding = new Thickness(3) };
            var dot = new Button { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), Padding = new Thickness(0), MinWidth = 0, MinHeight = 0, Background = TagDot.BrushOf(c), BorderThickness = new Thickness(0) };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(dot, T("Colour {0}", Array.IndexOf(ModNotes.Colors, c) + 1));
            var cc = c;
            dot.Click += (_, _) => { chosen = cc; foreach (var x in rings) x.BorderBrush = (string)x.Tag == chosen ? Res("ApexSoftTextBrush") : null; };
            ring.Child = dot;
            ring.Tag = c;
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
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        foreach (var r in _modRows) r.Refresh();
        _folderSig = "";
        FillMods();
    }

    async Task DeleteTagAsync(string name)
    {
        if (!await Confirm(T("Delete this tag?"), T("{0} is taken off every mod. The mods stay as they are.", name), T("Delete"))) return;
        try { ModNotes.DeleteTag(name); } catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        foreach (var r in _modRows) r.Refresh();
        _folderSig = "";
        FillMods();
    }

    // ---------- what a package holds ----------

    readonly Dictionary<string, ModContents> _modContents = new(StringComparer.OrdinalIgnoreCase);

    // "Inside": how many objects and Create a Sim parts of each kind, and their names, read when the mod is chosen
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
                panel.Children.Add(new TextBlock { Text = "\u2022  " + n, FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            var more = c.Parts.Sum(x => x.Count) - Math.Min(shown, names.Count);
            if (names.Count > 0 && more > 0) panel.Children.Add(Muted(T("and {0} more", more)));
        }
        if (_modContents.TryGetValue(path, out var known)) Fill(known);
        else
        {
            panel.Children.Add(Muted(T("Reading what it holds…")));
            _ = Task.Run(() => { try { return ModContent.Read(path); } catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { return new ModContents([], [], []); } })
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

    MenuFlyout MoveMenu(List<ModRow> rows)
    {
        var menu = new MenuFlyout();
        menu.Items.Add(MenuItem("Mods", () => MoveMods(rows, "")));
        foreach (var d in _modDirs)
            menu.Items.Add(MenuItem(new string(' ', 4 * d.Count(c => c == Path.DirectorySeparatorChar)) + Path.GetFileName(d), () => MoveMods(rows, d)));
        return menu;
    }

    static Border Warn(string title, string? text, string? action = null, Action? run = null)
    {
        var s = new StackPanel { Spacing = 4 };
        s.Children.Add(new TextBlock { Text = title, FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexWarningBrush"), TextWrapping = TextWrapping.Wrap });
        if (text is not null) s.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0xD9, 0xC3, 0x9A)), TextWrapping = TextWrapping.Wrap });
        if (action is not null && run is not null) s.Children.Add(Link(action, run));
        return new Border
        {
            Child = s, Padding = new Thickness(12), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1),
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x1F, 0x1A, 0x12)), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x4A, 0x3A, 0x1E)),
        };
    }

    static Button SmallButton(string text, Action click)
    {
        var b = new Button { Content = text, FontSize = 12, Padding = new Thickness(10, 4, 10, 5), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(200, 0x13, 0x14, 0x17)) };
        b.Click += (_, _) => click();
        return b;
    }

    void SetPicture(ModRow r, string? file)
    {
        try
        {
            ModNotes.SetPicture(r.File.Rel, file);
            r.Image = null;
            _ = LoadModPicturesAsync([r]).ContinueWith(_ => DispatcherQueue.TryEnqueue(FillModDetails));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The picture was not saved"), ErrorText(e)); }
    }

    async void RemoveMods(List<ModRow> rows)
    {
        var text = rows.Count == 1 ? T("{0} goes to the Recycle Bin. You can restore it from there.", rows[0].Name)
            : T("{0} mods go to the Recycle Bin. You can restore them from there.", rows.Count);
        if (!await Confirm(rows.Count == 1 ? T("Remove this mod?") : T("Remove these mods?"), text, T("Remove"))) return;
        await ModsChange(() => { foreach (var r in rows) ModLibrary.Remove(r.File); }, rows.Count == 1 ? T("Mod moved to the Recycle Bin") : T("{0} mods moved to the Recycle Bin", rows.Count));
    }

    static void ShowFile(string path)
    {
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true }); }
        catch (Exception e) { App.Log(e); }
    }
}
