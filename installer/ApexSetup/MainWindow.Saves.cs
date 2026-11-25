using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Saves: the saves on one side (with the ones that are gone and the ones kept out of the game), and on the other the one
// chosen: its backups, what can be cleaned, and its other work in its menu (MainWindow.SaveBackups.cs has the backups;
// the pictures that stay are a dialog, MainWindow.SaveDialogs.cs). Nothing here reads or writes a save on the window's
// thread; what changes one goes through SaveClean, SaveCopies and SaveBackups, behind a dialog that says how far it is.
public sealed partial class MainWindow
{
    const CleanParts AllPictures = CleanParts.Memories | CleanParts.RepeatedMemories | CleanParts.Generated;
    const long LeastGain = 50 << 10; // (less than this is not worth a clean, and would read as 0 MB)

    string _savesSig = "";      // what SavesHost shows (the same again is left as it is)
    bool _savesBusy;
    int _savesRun, _savesNote;
    SavesNow? _savesNow;
    CleanParts _saveParts = CleanParts.Portraits | CleanParts.Thumbnails | CleanParts.RepeatedMemories;
    string? _savePick;                                    // the save on the right, by its Key
    Border? _saveDetail;                                  // the right side, and the two ways of choosing what is on it
    ListView? _saveList;
    ComboBox? _savePicker;
    string? _saveWant;                                    // the one to choose once it is read (a new save, a renamed one)
    string _savesPage = "";                               // "", "auto" (the automatic backups) or "all" (every backup)
    readonly Dictionary<string, Task> _saveReads = new(); // what was read of the saves, kept while the launcher is open
    readonly SemaphoreSlim _saveReading = new(2);

    sealed record SaveLook(Save Save, SaveInfo Info);
    enum SaveKindOf { Here, Gone, Stored }
    // a line of the list. Save: the name its backups have; Look is null for one only its backups remain of
    sealed record SaveEntry(SaveKindOf Kind, string Key, string Save, string Name, SaveLook? Look, List<SavePoint> Points);
    // FolderGone: the chosen backup folder that is not there now; Free: what is free on its drive
    sealed record SavesNow(string Docs, List<SaveLook> Here, List<SaveEntry> Entries, string? FolderGone, long? Free)
    {
        public Dictionary<string, List<SavePoint>> Points { get; } = Entries.ToDictionary(e => e.Key, e => e.Points);
    }
    sealed record SaveRead(SaveContents Contents, CleanPlan Plan);

    // what was known shows at once; the folders are read off the window's thread and only a change is drawn
    void FillSaves()
    {
        if (_game is null)
        {
            _savesSig = "";
            SavesHost.Children.Clear();
            SavesHost.Children.Add(Heading(T("Saves")));
            SavesHost.Children.Add(Muted(T("Choose the game first.")));
            return;
        }
        // (what could not be read is tried again: a file held for a moment, the kept pictures' list)
        foreach (var key in _saveReads.Where(r => r.Value.IsCompleted && (r.Value is Task<SaveRead?> { IsCompletedSuccessfully: true } t ? t.Result is null or { Plan.Gains.Count: 0 } : r.Value.IsFaulted)).Select(r => r.Key).ToList()) { _saveReads.Remove(key); _savesSig = ""; }
        var docs = Docs;
        if (_savesNow is { } last && last.Docs == docs) DrawSaves(last);
        else if (_savesNow is not null) { _savesNow = null; _savePick = null; _savesSig = ""; SavesHost.Children.Clear(); }
        _ = Read(++_savesRun);

        async Task Read(int run)
        {
            try
            {
                var now = await Task.Run(() => ReadSaves(docs));
                if (run != _savesRun) return;
                _savesNow = now;
                if (_step == SavesScreen) DrawSaves(now);
            }
            catch (Exception e) { App.Log(e); if (run == _savesRun) SavesSay(InfoBarSeverity.Error, T("The launcher couldn't read your saves"), ErrorText(e)); }
        }
    }

    static SavesNow ReadSaves(string docs)
    {
        static SaveLook Look(Save s) => new(s, SaveInfo.Of(s));
        var zips = SaveCopies.Zips(docs, _ => true).GroupBy(z => z.Name, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.Select(z => z.Point).ToList(), StringComparer.OrdinalIgnoreCase);
        List<SavePoint> Of(string name, string? game)
        {
            var points = zips.GetValueOrDefault(name)?.ToList() ?? [];
            if (game is not null && Directory.Exists(game) && SaveInfo.Read(game) is { Bytes: > 0 } i) points.Add(new(CopyKind.Game, game, i.LastPlayed, i.Bytes, null, Stored: i.Bytes));
            return points.OrderByDescending(p => p.When).ToList();
        }
        var here = Saves.List(docs).Select(Look).ToList();
        var entries = here.Select(l => new SaveEntry(SaveKindOf.Here, l.Save.Dir, l.Save.Name, FamilyName(l.Save, l.Info), l, Of(l.Save.Name, l.Save.Dir + ".backup"))).ToList();
        // (a save kept out of the game may have the name of one that is in it: the backups are that one's)
        var taken = here.Select(l => l.Save.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var l in SaveCopies.Stored(docs).Select(Look)) entries.Add(new(SaveKindOf.Stored, l.Save.Dir, l.Save.Name, FamilyName(l.Save, l.Info), l, taken.Add(l.Save.Name) ? Of(l.Save.Name, null) : []));
        foreach (var l in SaveCopies.Lost(docs).Select(Look)) if (taken.Add(l.Save.Name)) entries.Add(new(SaveKindOf.Gone, l.Save.Dir, l.Save.Name, FamilyName(l.Save, l.Info), l, Of(l.Save.Name, l.Save.Dir)));
        foreach (var (name, points) in zips.Where(z => !taken.Contains(z.Key)).OrderByDescending(z => z.Value.Max(p => p.When))) entries.Add(new(SaveKindOf.Gone, name, name, name, null, points.OrderByDescending(p => p.When).ToList()));
        var dir = Saves.BackupDir(docs);
        var gone = BackupChoices.Load().FolderOf(docs) is { } chosen && !Directory.Exists(chosen) ? chosen : null;
        long? free = null;
        try { free = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir))!).AvailableFreeSpace; } catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { }
        return new(docs, here, entries, gone, free);
    }

    // the same screen drawn again, for a choice made on it
    void SavesAgain() { _savesSig = ""; if (_savesNow is { } now && _step == SavesScreen) DrawSaves(now); }

    // what the screen shows, as one text
    string SavesSig(SavesNow now) => string.Join('\n', now.Entries.Select(e => $"{e.Kind}|{e.Key}|{e.Look?.Info.LastPlayed.Ticks}|{e.Look?.Info.Bytes}|" + string.Join('|', e.Points.Select(p => p.Path + p.When.Ticks + p.Label + p.Pinned + p.Stored)))
            .Prepend($"{SideFits}|{Lang}|{_light}|{_running}|{_busy}|{Maintenance}|{_settings.InstallUpdatesOnPlay}|{_savePick}|{_savesPage}|{now.FolderGone}|{_autoError}"));

    void DrawSaves(SavesNow now)
    {
        if (_saveWant is not null && now.Entries.Any(e => e.Key.Equals(_saveWant, StringComparison.OrdinalIgnoreCase))) (_savePick, _saveWant, _saveAllBackups) = (_saveWant, null, false);
        if (_savePick is null || now.Entries.All(e => !e.Key.Equals(_savePick, StringComparison.OrdinalIgnoreCase))) { _savePick = now.Entries.FirstOrDefault()?.Key; _saveAllBackups = false; }
        FillSaveSide(now);
        var sig = SavesSig(now);
        if (sig == _savesSig) return;
        _savesSig = sig;
        SavesHost.Children.Clear();
        SavesBar.Visibility = Visibility.Collapsed;
        SavesNote.Margin = new Thickness(24, 0, 24, 16);
        ScreenSaves.MaxWidth = _savesPage == "auto" ? 920 : double.PositiveInfinity;
        if (_savesPage == "auto") { DrawAutomatic(now); return; }
        if (_savesPage == "all") { DrawAllBackups(now); return; }

        var several = Id(Button(T("Clean several"), () => CleanSeveral(now)), "SaveCleanSeveral");
        several.IsEnabled = now.Here.Count > 1;
        var import = Id(Button("", ImportSave), "SaveImport");
        import.Content = Labeled("download", T("Import"));
        import.IsEnabled = SavesBlocked() is null;
        var auto = Id(Button("", () => SavesGo("auto")), "SaveAutoPage");
        auto.Content = Labeled("calendar-clock", T("Automatic backups"));
        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var b in new[] { several, import, auto }) if (b != auto ? now.Here.Count > 0 || b != several : !SideFits) acts.Children.Add(Dims(b));
        SavesHost.Children.Add(PageHead(T("Saves"), now.Here.Count switch { 0 => "", 1 => T("1 save"), var n => T("{0} saves", n) } + (now.Here.Count > 0 ? ", " + SaveSize(now.Here.Sum(l => l.Info.Bytes)) : ""), acts));
        if (SavesBlocked() is { } why && now.Here.Count > 0) SavesHost.Children.Add(Warn(why, null));
        if (now.Entries.Count == 0) { SavesHost.Children.Add(EmptyPlace("users", T("No saves yet. They appear here after you save a game."))); return; }

        var at = now.Entries.First(e => e.Key.Equals(_savePick, StringComparison.OrdinalIgnoreCase));
        var page = new StackPanel { Spacing = 18 };
        // (a narrow window has no pane: the save is chosen in one line over it)
        var pick = _savePicker = SavePicker(now);
        if (!SideFits) page.Children.Add(pick);
        page.Children.Add(_saveDetail = new Border { Child = SaveDetail(now, at) });
        page.SizeChanged += (_, e) => several.Visibility = e.NewSize.Width < 520 ? Visibility.Collapsed : Visibility.Visible;
        SavesHost.Children.Add(page);
    }

    string? SavesBlocked() => DryRun ? T("Test run: the launcher changes nothing.") : _running || Games.IsRunning() ? T("Close The Sims 3 to clean, back up or restore your saves.") : null;

    // ---------- what is read of a save ----------

    // read once for a save as it is; a save played or cleaned since is read again
    static string ReadKey(SaveLook l, string what) => $"{what}|{l.Save.Dir}|{l.Info.LastPlayed.Ticks}|{l.Info.Bytes}";
    Task<R> OfSave<R>(SaveLook l, string what, Func<Task<R>> read) =>
        _saveReads.TryGetValue(ReadKey(l, what), out var t) ? (Task<R>)t : (Task<R>)(_saveReads[ReadKey(l, what)] = read());

    // off the window's thread, two saves at a time; null when it can't be read
    async Task<R?> OffThread<R>(Func<R?> read)
    {
        await _saveReading.WaitAsync();
        try { return await Task.Run(read); }
        catch (Exception e) { App.Log(e); return default; }
        finally { _saveReading.Release(); }
    }

    Task<ImageSource?> FamilyPicture(SaveLook l) => OfSave(l, "family", async () => await SaveCard.SimsOf(await OffThread(() => Saves.Photo(l.Save)), small: true));

    Task<SaveRead?> CleanRead(SaveLook l) => OfSave(l, "clean", () => OffThread(() =>
    {
        var c = SaveContents.Read(l.Save.Dir);
        return new SaveRead(c, SaveClean.Plan(Docs, l.Save, CleanParts.None, null, c));
    }));

    // null: still being read
    SaveRead? Known(SaveLook l) => _saveReads.TryGetValue(ReadKey(l, "clean"), out var t) && t is Task<SaveRead?> { IsCompletedSuccessfully: true, Result: { } r } ? r : null;

    // what a clean of those parts frees; the plan of nothing chosen already gives back the room nothing uses
    static long Gain(SaveRead r, CleanParts parts)
    {
        var gain = r.Plan.Gains.Where(g => (parts & g.Key) != 0).Sum(g => g.Value.Bytes);
        return parts == CleanParts.None || gain + r.Plan.Before - r.Plan.After < LeastGain ? 0 : gain + r.Plan.Before - r.Plan.After;
    }

    // ---------- words ----------

    static string FamilyName(Save s, SaveInfo info) => info.Household.Length > 0 ? T("{0} household", info.Household) : s.Name;

    // the worlds it has, the one being played first; two saves of one household in one world differ only by the save's name
    static string SavePlace(Save s, SaveInfo info)
    {
        var w = info.Worlds.DefaultIfEmpty(info.World).Select(Worlds.Pretty).Where(n => n.Length > 0).Distinct().ToList();
        var place = w.Count == 2 ? T("{0} and {1}", w[0], w[1]) : string.Join(", ", w);
        return place.Length == 0 ? s.Name : w.Any(n => n.StartsWith(s.Name, StringComparison.OrdinalIgnoreCase)) ? place : s.Name + " · " + place;
    }

    static string SaveDay(DateTime t, bool hour = false) => Day(t, hour);

    static string SaveSize(long bytes) => bytes < 1 << 20 ? (Math.Max(0, bytes) / 1048576.0).ToString("0.#", LangCulture()) + " MB" : ModRow.SizeText(bytes);

    static TextBlock SaveText(string text, double size, string brush = "TextFillColorPrimaryBrush", bool strong = false) => new()
    {
        Text = text, FontSize = size, Foreground = Res(brush), FontWeight = strong ? FontWeights.SemiBold : FontWeights.Normal, TextTrimming = TextTrimming.CharacterEllipsis,
    };

    // ---------- the list ----------

    Grid SavePhoto(SaveLook? l, double side)
    {
        var photo = new Grid { Width = side, Height = side, CornerRadius = new CornerRadius(12), Background = Res("ApexPillBrush"), VerticalAlignment = VerticalAlignment.Center };
        _ = Photo();
        return photo;

        async Task Photo()
        {
            try
            {
                if (l is not null && await FamilyPicture(l) is { } img)
                    photo.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(2, 4, 2, 0) });
                else photo.Children.Add(Lucide.Icon("users", side / 2, Res("ApexIconMutedBrush"), 1.4));
            }
            catch (Exception e) { App.Log(e); }
        }
    }

    // two saves of one household differ by the save's name
    static bool Namesake(SavesNow now, SaveEntry e) => e.Name != e.Save && now.Entries.Any(x => x != e && x.Name == e.Name);
    static string EntryName(SavesNow now, SaveEntry e) => Namesake(now, e) ? e.Name + " · " + e.Save : e.Name;
    string EntrySub(SavesNow now, SaveEntry e) => SaveDay(e.Look?.Info.LastPlayed ?? e.Points[0].When) + " · " + (Namesake(now, e) ? e.Save : SaveSize(e.Look?.Info.Bytes ?? e.Points[0].Stored));
    static string EntryGroup(SaveKindOf k) => k == SaveKindOf.Stored ? T("Kept out of the game") : T("Gone from the game");

    void SavePicked(string key)
    {
        if (key.Equals(_savePick, StringComparison.OrdinalIgnoreCase)) return;
        (_savePick, _saveAllBackups) = (key, false);
        Scroller.ChangeView(null, 0, null, true);
        // (only its side: the list keeps its place and the keyboard)
        if (_savesNow is not { } now || _saveDetail is null || _saveList is null || _savePicker is null || now.Entries.FirstOrDefault(e => e.Key == key) is not { } at) { SavesAgain(); return; }
        _savesSig = SavesSig(now);
        _saveDetail.Child = SaveDetail(now, at);
        SaveSideAt();
        _savePicker.SelectedItem = _savePicker.Items.FirstOrDefault(i => i is ComboBoxItem { Tag: string k } && k == key);
    }

    // the pane: the backups' own pages, then the saves by where they are
    string _saveQuery = "", _saveSideSig = "";
    void FillSaveSide(SavesNow now)
    {
        if (now.Entries.Count == 0) { HideSide(); return; }
        var auto = BackupChoices.Load() is { OnClose: false, BeforePlay: false, Daily: false } ? T("Off") : T("On");
        var sig = $"{Lang}|{_light}|{auto}|{_saveQuery}|{string.Join(',', _collapsed.Order())}|" + string.Join('\n', now.Entries.Select(e => $"{e.Kind}|{e.Key}|{e.Name}|{EntrySub(now, e)}|{e.Points.Count}"));
        if (_sideOn && sig == _saveSideSig) { SaveSideAt(); return; }
        _saveSideSig = sig;
        var items = new List<FrameworkElement>();
        void Group(string key, string label, IEnumerable<FrameworkElement> rows)
        {
            var all = rows.ToList();
            if (all.Count == 0) return;
            items.Add(SideGroup(key, label, () => { _saveSideSig = ""; FillSaveSide(now); }));
            if (!_collapsed.Contains(key) || _saveQuery.Length > 0) items.AddRange(all);
        }
        bool Has(string t) => t.Contains(_saveQuery, StringComparison.CurrentCultureIgnoreCase);
        if (_saveQuery.Length == 0)
            Group("saves:backups", T("Backups"),
            [
                Place("@all", "archive", T("All backups"), now.Entries.Sum(e => e.Points.Count) is var n && n > 0 ? n.ToString() : "", "SavesAll"),
                Place("@auto", "calendar-clock", T("Automatic"), auto, "SaveAutoPage"),
            ]);
        foreach (var (kind, label) in new[] { (SaveKindOf.Here, T("In the game")), (SaveKindOf.Stored, T("Kept out of the game")), (SaveKindOf.Gone, T("Gone from the game")) })
            Group("saves:" + kind, label, now.Entries.Where(e => e.Kind == kind && (_saveQuery.Length == 0 || Has(e.Name) || Has(e.Save) || e.Look is { } l && Has(SavePlace(l.Save, l.Info)))).Select(e => SaveLine(now, e)));
        var list = _saveList = SideList(items, T("Your saves"), "SaveList", key =>
        {
            if (key[0] == '@') { if (_savesPage != key[1..]) SavesGo(key[1..]); }
            else if (_savesPage.Length == 0) SavePicked(key);
            else { (_savePick, _saveAllBackups) = (key, false); SavesGo(""); }
        });
        if (_sideOn && SideHost.Child is Grid { Children.Count: 2 } open) { open.Children.RemoveAt(1); Grid.SetRow(list, 1); open.Children.Add(list); }
        else ShowSide(SaveSearch(), list, SavesAgain);
        SaveSideAt();

        static FrameworkElement Place(string key, string icon, string label, string count, string id)
        {
            var g = SideLine(icon, label, count);
            g.Tag = key;
            AutomationProperties.SetAutomationId(g, id);
            AutomationProperties.SetName(g, label);
            return g;
        }
    }

    void SaveSideAt()
    {
        var key = _savesPage.Length > 0 ? "@" + _savesPage : _savePick;
        if (_saveList?.ItemsSource is List<FrameworkElement> rows) _saveList.SelectedItem = rows.FirstOrDefault(r => r.Tag is string k && k.Equals(key, StringComparison.OrdinalIgnoreCase));
    }

    FrameworkElement SaveSearch()
    {
        var box = new TextBox { PlaceholderText = T("Search"), Text = _saveQuery };
        AutomationProperties.SetName(box, T("Search saves"));
        box.TextChanged += (_, _) => { if (box.Text.Trim() == _saveQuery) return; _saveQuery = box.Text.Trim(); if (_savesNow is { } now) FillSaveSide(now); };
        box.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Escape && box.Text.Length > 0) { box.Text = ""; e.Handled = true; } };
        return SearchField(box);
    }

    FrameworkElement SaveLine(SavesNow now, SaveEntry e)
    {
        var sub = EntrySub(now, e);
        var line = new Grid { ColumnSpacing = 12, MinHeight = 60, Tag = e.Key };
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        line.ColumnDefinitions.Add(new ColumnDefinition());
        line.Children.Add(SavePhoto(e.Look, 40));
        var words = (StackPanel)Col(SaveText(e.Name, 13, strong: true), SaveText(sub, 12, "ApexMutedBrush"));
        (words.Spacing, words.VerticalAlignment) = (2, VerticalAlignment.Center);
        Grid.SetColumn(words, 1);
        line.Children.Add(words);
        if (e.Kind != SaveKindOf.Here) line.Opacity = 0.75;
        ToolTipService.SetToolTip(line, e.Look is null ? e.Name : e.Name + "\n" + SavePlace(e.Look.Save, e.Look.Info));
        AutomationProperties.SetName(line, $"{e.Name}, {sub}");
        if (e.Kind == SaveKindOf.Here) line.ContextFlyout = SaveMenu(e.Look!);
        else
        {
            var menu = new MenuFlyout();
            menu.Items.Add(MenuItem(T("Delete"), () => Safe(DeleteGone(e)), "trash-2", danger: true));
            menu.Opening += (_, _) => menu.Items[0].IsEnabled = SavesBlocked() is null;
            line.ContextFlyout = menu;
        }
        return line;
    }

    ComboBox SavePicker(SavesNow now)
    {
        var box = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
        AutomationProperties.SetName(box, T("Your saves"));
        foreach (var e in now.Entries)
        {
            var i = new ComboBoxItem { Content = e.Name + " · " + EntrySub(now, e), Tag = e.Key };
            box.Items.Add(i);
            if (e.Key.Equals(_savePick, StringComparison.OrdinalIgnoreCase)) box.SelectedItem = i;
        }
        box.SelectionChanged += (_, _) => { if (box.SelectedItem is ComboBoxItem { Tag: string key }) SavePicked(key); };
        return box;
    }

    // ---------- the chosen save ----------

    StackPanel SaveDetail(SavesNow now, SaveEntry e)
    {
        var s = new StackPanel();
        s.Children.Add(SaveHero(e));
        if (e.Kind == SaveKindOf.Stored && e.Points.Count == 0) return s;
        BackupsPart(s, now, e);
        if (e.Kind == SaveKindOf.Here) CleanPart(s, now, e.Look!);
        return s;
    }

    // the chosen save: its name and place, Play, its household standing on the right, and what there is to know under them
    Border SaveHero(SaveEntry e)
    {
        var name = new TextBlock { Text = e.Name, FontFamily = new FontFamily("Segoe UI Variable Display"), FontSize = 32, LineHeight = 40, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        ToolTipService.SetToolTip(name, e.Name);
        AutomationProperties.SetHeadingLevel(name, AutomationHeadingLevel.Level2);
        var sub = PageSub(e.Kind switch
        {
            SaveKindOf.Here => SavePlace(e.Look!.Save, e.Look.Info),
            SaveKindOf.Stored => T("The game doesn't see this save until you put it back."),
            _ => T("This save left the game, but its backups are still here."),
        });
        sub.TextWrapping = TextWrapping.Wrap;
        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X2, Margin = new Thickness(0, Design.Space.X6, 0, 0) };
        if (e.Kind == SaveKindOf.Here)
        {
            var play = MainButton("", () => Safe(PlayAsync(e.Look!.Save.Name, update: _settings.InstallUpdatesOnPlay)));
            play.Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Children = { new LucideIcon { Solid = true, Kind = "play", Size = 14, Follow = true, StrokeWidth = 1.75, VerticalAlignment = VerticalAlignment.Center }, new TextBlock { Text = T("Play"), FontSize = 14, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center } } };
            (play.MinWidth, play.Height, play.IsEnabled) = (136, 44, !_busy && !Maintenance && !_running);
            AutomationProperties.SetName(play, T("Play {0}", e.Name));
            AutomationProperties.SetAutomationId(play, "SavePlay");
            acts.Children.Add(play);
            var more = MoreButton(T("More options") + ", " + e.Name, SaveMenu(e.Look!));
            (more.Width, more.Height) = (44, 44);
            more.Background = Res("ApexTileBrush");
            acts.Children.Add(more);
        }
        else if (e.Kind == SaveKindOf.Stored)
        {
            var back = MainButton(T("Put back in the game"), () => Safe(PutBack(e.Look!.Save)));
            (back.Padding, back.Height, back.IsEnabled) = (new Thickness(16, 0, 16, 0), 44, SavesBlocked() is null);
            acts.Children.Add(back);
        }
        if (e.Kind != SaveKindOf.Here)
        {
            var delete = Id(Dims(Button("", () => DeleteGone(e))), "SaveDelete");
            delete.Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { Lucide.Icon("trash-2", 16, Res("ApexErrorBrush")), new TextBlock { Text = T("Delete"), VerticalAlignment = VerticalAlignment.Center, Foreground = Res("ApexErrorBrush") } } };
            (delete.IsEnabled, delete.Height) = (SavesBlocked() is null, 44);
            AutomationProperties.SetName(delete, T("Delete") + ", " + e.Name);
            acts.Children.Add(delete);
        }
        var words = new StackPanel { Padding = new Thickness(28), Spacing = Design.Space.TitleToSub, VerticalAlignment = VerticalAlignment.Center, Children = { name, sub, acts } };
        var photo = SaveFamily(e.Look, 188);
        var top = new Grid { MinHeight = 204 };
        top.ColumnDefinitions.Add(new ColumnDefinition());
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        top.Children.Add(words);
        Grid.SetColumn(photo, 1);
        top.Children.Add(photo);
        top.SizeChanged += (_, a) => photo.Visibility = a.NewSize.Width < 520 ? Visibility.Collapsed : Visibility.Visible;

        var facts = new Grid { BorderThickness = new Thickness(0, 1, 0, 0), BorderBrush = Res("ApexHairlineBrush") };
        void Fact(string what, string value)
        {
            var cell = new Border { Padding = new Thickness(facts.Children.Count == 0 ? 28 : 20, 16, 20, 16), BorderThickness = new Thickness(facts.Children.Count == 0 ? 0 : 1, 0, 0, 0), BorderBrush = Res("ApexHairlineBrush"),
                Child = new StackPanel { Spacing = 2, Children = { Wrapped(SaveText(what, 12, "ApexMutedBrush")), Wrapped(SaveText(value, 14, strong: true)) } } };
            static TextBlock Wrapped(TextBlock t) { t.TextWrapping = TextWrapping.Wrap; return t; }
            ToolTipService.SetToolTip(cell, what + ": " + value);
            facts.ColumnDefinitions.Add(new ColumnDefinition());
            Grid.SetColumn(cell, facts.Children.Count);
            facts.Children.Add(cell);
        }
        if (e.Look is { } look)
        {
            Fact(T("Last played"), SaveDay(look.Info.LastPlayed, hour: true));
            Fact(T("Size"), SaveSize(look.Info.Bytes));
        }
        Fact(T("Backups"), e.Points.Count == 0 ? T("No backups yet") : e.Points.Count + " \u00b7 " + SaveSize(e.Points.Sum(p => p.Stored)));
        Fact(T("Last backup"), e.Points.Count == 0 ? "\u2014" : SaveDay(e.Points.Max(p => p.When)));
        return new Border { Child = new StackPanel { Children = { top, facts } }, CornerRadius = new CornerRadius(Design.Radius.Panel), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexCardBrush") };
    }

    // the household standing on the card's line, in a light of no colour
    Grid SaveFamily(SaveLook? l, double height)
    {
        var g = new Grid { Width = 260, VerticalAlignment = VerticalAlignment.Stretch };
        g.Children.Add(new SoftGlow(Tone("Muted"), App.Light ? 0.08 : 0.22, 0.6) { Width = 320, Height = 260, Margin = new Thickness(-30, 0, -30, 0), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom });
        if (l is not null) _ = Photo();
        else g.Children.Add(new Grid { Children = { Lucide.Icon("users", 56, Res("ApexIconMutedBrush"), 1.4) }, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
        return g;

        async Task Photo()
        {
            try
            {
                if (await OfSave(l, "family-large", async () => await SaveCard.SimsOf(await OffThread(() => Saves.Photo(l.Save)))) is { } img)
                    g.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform, MaxHeight = height, MaxWidth = 220, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom });
            }
            catch (Exception x) { App.Log(x); }
        }
    }

    MenuFlyout SaveMenu(SaveLook l)
    {
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
        var changes = new List<MenuFlyoutItem>();
        MenuFlyoutItem Item(string text, Func<Task> run, string icon, bool change = true, bool danger = false)
        {
            var i = MenuItem(text, () => Safe(run()), icon, danger: danger);
            if (change) changes.Add(i);
            menu.Items.Add(i);
            return i;
        }
        Item(T("Open folder"), () => { OpenFolder(l.Save.Dir); return Task.CompletedTask; }, "folder", change: false);
        menu.Items.Add(new MenuFlyoutSeparator());
        Item(T("Duplicate save"), () => DuplicateSave(l), "layers");
        Item(T("Rename save"), () => RenameSave(l), "pencil");
        Item(T("Keep out of the game"), () => StoreSave(l), "folder-input");
        menu.Items.Add(new MenuFlyoutSeparator());
        Item(T("Move to the Recycle Bin"), () => DeleteSave(l), "trash-2", danger: true);
        menu.Opening += (_, _) => { foreach (var i in changes) i.IsEnabled = SavesBlocked() is null; };
        return menu;
    }

    // ---------- what goes ----------

    static readonly (CleanParts Parts, string Name, string Info, string Icon)[] CleanOptions =
    [
        (CleanParts.Portraits | CleanParts.Thumbnails, "Portraits and thumbnails", "The game makes them again as you play", "users"),
        (CleanParts.RepeatedMemories, "Repeated memories", "One of each picture stays", "layers"),
        (AllPictures, "All memories and photos", "Only the pictures you choose to keep stay", "image"),
    ];

    void CleanPart(StackPanel host, SavesNow now, SaveLook l)
    {
        var clean = Id(Button("", () => CleanSaves([l])), "SaveClean");
        var keep = Id2(Link(T("Choose the pictures that stay"), () => Safe(KeepAsync([l]))), "SaveKeep");
        keep.VerticalAlignment = VerticalAlignment.Center;
        host.Children.Add(Section(T("Clean"), new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, Children = { keep, Dims(clean) } }));
        var note = PageSub("");
        (note.Margin, note.Visibility) = (new Thickness(0, 8, 0, 0), Visibility.Collapsed);
        var rows = new StackPanel { Spacing = 4 };
        AutomationProperties.SetName(rows, T("Clean"));
        var says = new List<Action<SaveRead?, bool>>();
        var bar = new Grid { Height = 8, CornerRadius = new CornerRadius(4), Background = Res("ApexTileBrush") };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0, GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition());
        var part = new Border { Background = Design.DesignSystem.MainBrush, CornerRadius = new CornerRadius(4) };
        bar.Children.Add(part);
        var barLeft = SaveText("", 13, "ApexSoftTextBrush");
        var barRight = SaveText(SaveSize(l.Info.Bytes), 13, "ApexMutedBrush");
        barRight.HorizontalAlignment = HorizontalAlignment.Right;
        var meter = new Border { Padding = new Thickness(Design.Space.CardPad), Child = new StackPanel { Spacing = Design.Space.X3, Children = { new Grid { Children = { barLeft, barRight } }, bar } } };
        Soft(meter);
        rows.Children.Add(Join(meter));
        says.Add((read, done) =>
        {
            var gain = read is null ? 0 : Gain(read, _saveParts);
            barLeft.Text = !done ? T("Reading the save\u2026") : gain > 0 ? T("{0} can be freed", SaveSize(gain)) : T("Nothing to free with these choices");
            bar.ColumnDefinitions[0].Width = new GridLength(gain, GridUnitType.Star);
            bar.ColumnDefinitions[1].Width = new GridLength(Math.Max(1, l.Info.Bytes - gain), GridUnitType.Star);
            part.MinWidth = gain > 0 ? 8 : 0;
        });
        foreach (var (parts, title, info, icon) in CleanOptions)
        {
            var size = SaveText("", 13, "ApexMutedBrush");
            size.VerticalAlignment = VerticalAlignment.Center;
            var sw = new ToggleSwitch { IsOn = (_saveParts & parts) == parts, OnContent = "", OffContent = "", MinWidth = 0, Width = 40, VerticalAlignment = VerticalAlignment.Center };
            AutomationProperties.SetName(sw, T(title) + ". " + T(info));
            sw.Toggled += (_, _) =>
            {
                var was = _saveParts;
                if (sw.IsOn) _saveParts |= parts;
                // (the last picture of each can't go while its copies stay)
                else if ((_saveParts & parts) == parts) _saveParts &= parts == CleanParts.RepeatedMemories ? ~AllPictures : parts == AllPictures ? ~(CleanParts.Memories | CleanParts.Generated) : ~parts;
                if (was != _saveParts) Say();
            };
            rows.Children.Add(Row(icon, T(title), T(info), new StackPanel { Orientation = Orientation.Horizontal, Spacing = 20, Children = { size, sw } }));
            var only = parts == AllPictures ? CleanParts.Memories | CleanParts.Generated : parts;
            says.Add((read, done) =>
            {
                var on = (_saveParts & parts) == parts;
                sw.IsOn = on;
                var bytes = read?.Plan.Gains.Where(x => (only & x.Key) != 0).Sum(x => x.Value.Bytes) ?? 0;
                size.Text = read is null ? "" : SaveSize(bytes);
                size.Foreground = Res(on && bytes > 0 ? "ApexSuccessBrush" : "ApexMutedBrush");
            });
        }
        host.Children.Add(rows);
        host.Children.Add(note);
        // (this one first; the rest is read for the window that cleans several)
        _ = Read();
        Say();

        async Task Read()
        {
            try { await CleanRead(l); Say(); foreach (var x in now.Here.Take(12)) await CleanRead(x); }
            catch (Exception e) { App.Log(e); }
        }
        void Say()
        {
            var read = Known(l);
            var done = CleanRead(l).IsCompleted;
            foreach (var s in says) s(read, done);
            var gain = read is null ? 0 : Gain(read, _saveParts);
            clean.Content = !done ? T("Reading the save…") : read is null ? T("Can't be read") : read.Plan.Gains.Count == 0 ? T("Can't be cleaned") : gain > 0 ? T("Free up {0}", SaveSize(gain)) : T("Nothing to clean");
            clean.IsEnabled = gain > 0 && SavesBlocked() is null;
            if (read?.Plan is { Refusal: { } no, Gains.Count: 0 }) (note.Text, note.Visibility) = (T(no.Key, TArgs(no.Args)), Visibility.Visible);
        }
    }

    static HyperlinkButton Id2(HyperlinkButton b, string id) { AutomationProperties.SetAutomationId(b, id); return b; }

    // ---------- the work ----------

    sealed class CleanSay(Action<CleanProgress> say) : IProgress<CleanProgress> { public void Report(CleanProgress value) => say(value); }

    // one save after the other, each planned again from its files as they are now; one that fails leaves those before it clean
    async Task CleanSaves(List<SaveLook> picked)
    {
        var (docs, parts) = (Docs, _saveParts);
        var todo = picked.Where(l => Known(l) is { } r && Gain(r, parts) > 0).ToList();
        if (todo.Count == 0) return;
        Exception? error = null;
        var done = await SaveWork(todo.Count == 1 ? T("Cleaning the save") : T("Cleaning the saves"), (say, ct) =>
        {
            var list = new List<CleanResult>();
            for (var i = 0; i < todo.Count; i++)
            {
                var (l, name) = (todo[i], todo.Count == 1 ? "" : T("{0} of {1}", i + 1, todo.Count) + " · ");
                name += FamilyName(l.Save, l.Info) + "\n";
                try
                {
                    var plan = SaveClean.Plan(docs, l.Save, parts, null, null, ct);
                    list.Add(SaveClean.Run(plan, new CleanSay(c =>
                    {
                        var part = c.Total > 0 ? (double)c.Done / c.Total : 0;
                        if (c.Step == CleanStep.Copy) say(name + T("Making a copy of the save"), -1, true);
                        else if (c.Step == CleanStep.Write) say(name + T("Writing the smaller save"), 0.9 * part, true);
                        else if (c.Step == CleanStep.Verify) say(name + T("Comparing it with the original"), 0.9 * part, true);
                        else say(name + T("Putting it in place"), 0.9 + 0.1 * part, false);
                    }), ct));
                }
                catch (Exception e) when (list.Count > 0) { error = e; break; }
            }
            return list;
        });
        FillSaves();
        if (done is null) return;
        var freed = T("{0} freed", SaveSize(done.Sum(r => r.Before - r.After)));
        if (error is null) SavesSay(InfoBarSeverity.Success, done.Count == 1 ? T("Save cleaned") : T("{0} saves cleaned", done.Count), freed, T("Undo"), () => _ = UndoClean(done), stays: true);
        else
        {
            App.Log(error);
            SavesSay(InfoBarSeverity.Warning, T("{0} of {1} saves cleaned", done.Count, todo.Count), error is OperationCanceledException ? freed : freed + ". " + ErrorText(error), T("Undo"), () => _ = UndoClean(done), stays: true);
        }
    }

    async Task UndoClean(List<CleanResult> done)
    {
        var docs = Docs;
        Exception? error = null;
        var back = await SaveWork(T("Undoing the clean"), (say, ct) =>
        {
            var list = new List<CleanResult>();
            foreach (var r in done)
                try
                {
                    say(r.Save.Name + "\n" + T("Copying the save and checking the copy"), -1, true);
                    SaveClean.Undo(docs, r, ct);
                    list.Add(r);
                }
                catch (Exception e) when (list.Count > 0) { error = e; break; }
            return list;
        });
        FillSaves();
        if (back is null) return;
        if (error is null) { SavesSay(InfoBarSeverity.Success, T("The saves are as they were before")); return; }
        App.Log(error);
        var rest = done.Except(back).ToList();
        SavesSay(InfoBarSeverity.Warning, T("{0} of {1} saves put back", back.Count, done.Count), error is OperationCanceledException ? "" : ErrorText(error), T("Try again"), () => _ = UndoClean(rest), stays: true);
    }

    async Task CopySaves(List<Save> saves)
    {
        var docs = Docs;
        if (await SaveWork(T("Backing up"), (say, ct) =>
        {
            for (var i = 0; i < saves.Count; i++)
            {
                ct.ThrowIfCancellationRequested();
                say(saves[i].Name + "\n" + T("Copying the save and checking the copy"), saves.Count == 1 ? -1 : (double)i / saves.Count, true);
                Saves.Backup(saves[i], docs);
            }
            return saves;
        }) is null) return;
        FillSaves();
        SavesSay(InfoBarSeverity.Success, saves.Count == 1 ? T("Backup made") : T("{0} saves backed up", saves.Count), "", T("Open folder"), () => OpenFolder(Saves.BackupDir(docs)));
    }

    async Task DuplicateSave(SaveLook l)
    {
        var docs = Docs;
        var made = await SaveWork(T("Duplicate save"), (say, ct) => { say(T("Copying the save and checking the copy"), -1, true); return SaveCopies.Duplicate(docs, l.Save, null, ct); });
        FillSaves();
        if (made is not null) SavesSay(InfoBarSeverity.Success, T("The new save is called {0}", made.Name));
    }

    async Task RenameSave(SaveLook l)
    {
        if (await AskName(T("Rename save"), l.Save.Name, T("Rename")) is { } name) await RenameTo(l.Save, name, undo: true);
    }

    async Task RenameTo(Save s, string name, bool undo)
    {
        var docs = Docs;
        var now = await SaveWork(T("Rename save"), (say, _) => { say(T("Renaming the save and its backups"), -1, false); return SaveCopies.Rename(docs, s, name); });
        if (now is not null && now.Name != s.Name) SavesGalleryAgain(s.Name, now.Name);
        if (now is not null && s.Dir.Equals(_savePick, StringComparison.OrdinalIgnoreCase)) _saveWant = now.Dir;
        FillSaves();
        if (now is not null && now.Name != s.Name) SavesSay(InfoBarSeverity.Success, T("Renamed to {0}", now.Name), "", undo ? T("Undo") : null, () => _ = RenameTo(now, s.Name, undo: false));
    }

    async Task StoreSave(SaveLook l)
    {
        var docs = Docs;
        var now = await SaveWork(T("Keep out of the game"), (say, _) => { say(T("Moving the save"), -1, false); return SaveCopies.Store(docs, l.Save); });
        if (now is not null && now.Name != l.Save.Name) SavesGalleryAgain(l.Save.Name, now.Name);
        FillSaves();
        if (now is not null) SavesSay(InfoBarSeverity.Success, T("{0} is out of the game", FamilyName(l.Save, l.Info)), T("You can find it at the end of the list."), T("Undo"), () => _ = PutBack(now));
    }

    async Task PutBack(Save s)
    {
        var docs = Docs;
        var now = await SaveWork(T("Put back in the game"), (say, _) => { say(T("Moving the save"), -1, false); return SaveCopies.Unstore(docs, s); });
        if (now is not null && now.Name != s.Name) SavesGalleryAgain(s.Name, now.Name);
        FillSaves();
        if (now is not null) SavesSay(InfoBarSeverity.Success, T("The save is back in the game"));
    }

    async Task DeleteSave(SaveLook l)
    {
        if (!await Confirm(T("Move this save to the Recycle Bin?"), T("{0} leaves the game. You can restore it from the Recycle Bin, and its backups stay where they are.", FamilyName(l.Save, l.Info)), T("Move to the Recycle Bin"), danger: true)) return;
        await SaveWork(T("Move to the Recycle Bin"), (say, _) => { say(T("Moving the save"), -1, false); SaveCopies.Delete(l.Save); return l.Save; });
        FillSaves();
    }

    // the Gallery's own file was changed for it (its undo steps hold the old one)
    void SavesGalleryAgain(string from, string to)
    {
        if (_gal is not null) _gal = LoadGallery();
        if (_galAlbum == from) _galAlbum = to;
        _lastKept = null;
        _undoSteps.Clear();
        _redoSteps.Clear();
    }

    // Work off the window's thread behind a dialog that says how far it is (say: the words, its share or -1, and whether it
    // can still be stopped). Null when it was stopped or failed: then Saves says so.
    async Task<TR?> SaveWork<TR>(string title, Func<Action<string, double, bool>, CancellationToken, TR> work) where TR : class
    {
        if (_busy || _savesBusy) { SavesSay(InfoBarSeverity.Informational, T("The launcher is applying changes")); return null; }
        var bar = new ProgressBar { IsIndeterminate = true, Maximum = 1 };
        var text = Body(T("Reading the save…"));
        AutomationProperties.SetLiveSetting(text, AutomationLiveSetting.Polite);
        var body = new StackPanel { Spacing = 16, Width = Design.Dialogs.Small, Children = { text, bar } };
        using var cts = new CancellationTokenSource();
        var (running, canStop) = (true, true);
        var d = new ContentDialog { Tag = "save", Title = title, Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
        d.CloseButtonClick += (_, _) => { if (running && canStop && !cts.IsCancellationRequested) { cts.Cancel(); d.CloseButtonText = T("Stopping…"); } };
        d.Closing += (_, e) => e.Cancel = running; // (it closes when the work has ended)
        var opened = new TaskCompletionSource();
        d.Opened += (_, _) => opened.TrySetResult();
        SavesNote.IsOpen = false;
        var shown = ShowDialog(d);
        TR? result = null;
        Exception? error = null;
        SavesBusy(true);
        try
        {
            result = await ModWork(() => work((t, share, stop) => DispatcherQueue.TryEnqueue(() =>
            {
                if (!running) return;
                (text.Text, bar.IsIndeterminate, bar.Value) = (t, share < 0, Math.Max(0, share));
                if (stop == canStop) return;
                canStop = stop;
                if (!cts.IsCancellationRequested) d.CloseButtonText = stop ? T("Cancel") : "";
            }), cts.Token));
        }
        catch (Exception e) { error = e; }
        finally { SavesBusy(false); }
        running = false;
        await Task.WhenAny(opened.Task, shown);
        d.Hide();
        await shown;
        if (error is OperationCanceledException) SavesSay(InfoBarSeverity.Informational, T("Stopped"), T("Nothing was changed."));
        else if (error is not null) { App.Log(error); SavesSay(InfoBarSeverity.Error, T("Something went wrong"), ErrorText(error)); }
        return result;
    }

    void SavesBusy(bool on) => _savesBusy = on;

    // a note at the foot of Saves, gone after a while (an error, and what undoes a clean, stay until closed)
    void SavesSay(InfoBarSeverity sev, string title, string text = "", string? action = null, Action? run = null, bool stays = false)
    {
        if (_step != SavesScreen) return;
        var mine = ++_savesNote;
        SetInfo(SavesNote, sev, title, text, action, () => { SavesNote.IsOpen = false; run?.Invoke(); });
        if (sev != InfoBarSeverity.Error && !stays) After(action is null ? 7000 : 20000, () => { if (mine == _savesNote) SavesNote.IsOpen = false; });
    }
}
