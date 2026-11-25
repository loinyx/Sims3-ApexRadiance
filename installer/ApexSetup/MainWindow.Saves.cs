using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Saves: the saves to clean on one side, what goes and the clean on the other. A save's other work is in its menu; the
// copies and the pictures that stay are dialogs (MainWindow.SaveDialogs.cs). Nothing here reads or writes a save on the
// window's thread; what changes one goes through SaveClean and SaveCopies, behind a dialog that says how far it is.
public sealed partial class MainWindow
{
    const CleanParts AllPictures = CleanParts.Memories | CleanParts.RepeatedMemories | CleanParts.Generated;
    const long LeastGain = 50 << 10; // (less than this is not worth a clean, and would read as 0 MB)

    string _savesSig = "";      // what SavesHost shows (the same again is left as it is)
    bool _savesBusy;
    int _savesRun, _savesNote;
    SavesNow? _savesNow;
    CleanParts _saveParts = CleanParts.Portraits | CleanParts.Thumbnails | CleanParts.RepeatedMemories;
    HashSet<string>? _savePicks;                          // the saves to clean, by folder; null: not chosen yet
    Action? _saveSums;                                    // the numbers of the rows and of the panel, said again
    readonly Dictionary<string, Task> _saveReads = new(); // what was read of the saves, kept while the launcher is open
    readonly SemaphoreSlim _saveReading = new(2);

    sealed record SaveLook(Save Save, SaveInfo Info);
    sealed record SavesNow(string Docs, List<SaveLook> Here);
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
        var docs = Docs;
        if (_savesNow is { } last && last.Docs == docs) DrawSaves(last);
        else if (_savesNow is not null) { _savesNow = null; _savePicks = null; _savesSig = ""; SavesHost.Children.Clear(); }
        _ = Read(++_savesRun);

        async Task Read(int run)
        {
            try
            {
                var now = await Task.Run(() => new SavesNow(docs, Saves.List(docs).Select(s => new SaveLook(s, SaveInfo.Of(s))).ToList()));
                if (run != _savesRun) return;
                _savesNow = now;
                if (_step == SavesScreen) DrawSaves(now);
            }
            catch (Exception e) { App.Log(e); if (run == _savesRun) SavesSay(InfoBarSeverity.Error, T("The launcher can't read your saves"), ErrorText(e)); }
        }
    }

    void DrawSaves(SavesNow now)
    {
        var sig = string.Join('\n', now.Here.Select(l => $"{l.Save.Dir}|{l.Info.LastPlayed.Ticks}|{l.Info.Bytes}").Prepend($"{Lang}|{_light}|{_running}|{_busy}|{Maintenance}|{_settings.InstallUpdatesOnPlay}"));
        if (sig == _savesSig) return;
        _savesSig = sig;
        _savePicks ??= now.Here.Take(1).Select(l => l.Save.Dir).ToHashSet(StringComparer.OrdinalIgnoreCase);
        _savePicks.IntersectWith(now.Here.Select(l => l.Save.Dir));
        _saveSums = null;
        SavesHost.Children.Clear();

        var title = Heading(T("Saves"));
        AutomationProperties.SetHeadingLevel(title, AutomationHeadingLevel.Level1);
        var head = new Grid { ColumnSpacing = 16 };
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(Col(title, Body(now.Here.Count == 0 ? T("No saves yet. They show here after you save a game.") : T("Clean what makes a save grow. The household and the world stay the same."))));
        var copies = Id(Button(T("Copies"), CopiesAsync), "SaveCopies");
        (copies.Height, copies.Padding, copies.VerticalAlignment) = (36, new Thickness(18, 0, 18, 0), VerticalAlignment.Center);
        Grid.SetColumn(copies, 1);
        head.Children.Add(copies);
        SavesHost.Children.Add(head);
        if (SavesBlocked() is { } why && now.Here.Count > 0) SavesHost.Children.Add(Warn(why, null));
        if (now.Here.Count == 0) return;

        var page = new Grid { ColumnSpacing = 18, RowSpacing = 18 };
        page.ColumnDefinitions.Add(new ColumnDefinition());
        page.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var rows = new StackPanel();
        AutomationProperties.SetName(rows, T("Your saves"));
        foreach (var l in now.Here) rows.Children.Add(SaveRow(l, rows.Children.Count == 0));
        var list = new Border { Child = rows, Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(0), VerticalAlignment = VerticalAlignment.Top };
        page.Children.Add(list);
        var panel = SaveCleanPanel(now);
        page.Children.Add(panel);
        // beside the list; over it in a narrow window
        void Fit(double w)
        {
            var narrow = w < 860;
            panel.Width = narrow ? double.NaN : 328;
            Grid.SetColumn(panel, narrow ? 0 : 1);
            Grid.SetRow(list, narrow ? 1 : 0);
            Grid.SetColumnSpan(panel, narrow ? 2 : 1);
            Grid.SetColumnSpan(list, narrow ? 2 : 1);
        }
        Fit(Scroller.ActualWidth - 48);
        page.SizeChanged += (_, e) => Fit(e.NewSize.Width);
        SavesHost.Children.Add(page);
        _saveSums?.Invoke();
    }

    string? SavesBlocked() => DryRun ? T("Test run: the launcher changes nothing.") : _running || Games.IsRunning() ? T("Close The Sims 3 to clean your saves.") : null;

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

    Task<ImageSource?> FamilyPicture(SaveLook l) => OfSave(l, "family", async () => await SaveCard.SimsOf(await OffThread(() => Saves.Photo(l.Save))));

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

    string SaveDay(DateTime t, bool hour = false)
    {
        var c = LangCulture();
        var day = t.Date == DateTime.Today ? T("Today") : t.ToString(t.Year == DateTime.Today.Year ? "M" : "d", c);
        return hour ? day + ", " + t.ToString("t", c) : day;
    }

    // whole MB from 10 on
    string SaveSize(long bytes)
    {
        var c = LangCulture();
        var mb = Math.Max(0, bytes) / (1024.0 * 1024);
        return mb >= 1024 ? (mb / 1024).ToString("0.#", c) + " GB" : mb.ToString(mb >= 10 ? "0" : "0.#", c) + " MB";
    }

    static TextBlock SaveText(string text, double size, string brush = "TextFillColorPrimaryBrush", bool strong = false) => new()
    {
        Text = text, FontSize = size, Foreground = Res(brush), FontWeight = strong ? FontWeights.SemiBold : FontWeights.Normal, TextTrimming = TextTrimming.CharacterEllipsis,
    };

    // ---------- the list ----------

    FrameworkElement SaveRow(SaveLook l, bool first)
    {
        var (name, place) = (FamilyName(l.Save, l.Info), SavePlace(l.Save, l.Info));
        var sub = place + " · " + SaveDay(l.Info.LastPlayed);
        var line = new Grid { ColumnSpacing = 14 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) line.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var photo = new Grid { Width = 60, Height = 60, CornerRadius = new CornerRadius(8), Background = Res("ApexPillBrush"), Padding = new Thickness(2, 4, 2, 0) };
        line.Children.Add(photo);
        _ = Photo();
        var words = (FrameworkElement)Col(SaveText(name, 15, strong: true), SaveText(sub, 13, "ApexMutedBrush"));
        (words.VerticalAlignment, ((StackPanel)words).Spacing) = (VerticalAlignment.Center, 1);
        ToolTipService.SetToolTip(words, name + "\n" + sub);
        Grid.SetColumn(words, 1);
        line.Children.Add(words);
        var gain = SaveText("", 12, "ApexMutedBrush");
        var size = SaveText(SaveSize(l.Info.Bytes), 14, strong: true);
        size.HorizontalAlignment = gain.HorizontalAlignment = HorizontalAlignment.Right;
        var numbers = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Children = { size, gain } };
        Grid.SetColumn(numbers, 2);
        line.Children.Add(numbers);

        var box = new CheckBox
        {
            Content = line, MinWidth = 0, MinHeight = 84, Padding = new Thickness(14, 0, 0, 0), IsChecked = _savePicks!.Contains(l.Save.Dir),
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Center,
        };
        box.Click += (_, _) => { if (box.IsChecked == true) _savePicks.Add(l.Save.Dir); else _savePicks.Remove(l.Save.Dir); _saveSums?.Invoke(); };

        var play = Dims(Button("", () => PlayAsync(l.Save.Name, update: _settings.InstallUpdatesOnPlay)));
        (play.Width, play.Height, play.Padding, play.IsEnabled) = (36, 36, new Thickness(0), !_busy && !Maintenance && !_running);
        play.Content = Lucide.Icon("play", 16);
        AutomationProperties.SetName(play, T("Play {0}", name));
        ToolTipService.SetToolTip(play, T("Play this save"));
        var more = new Button { Width = 36, Height = 36, Padding = new Thickness(0), Content = Lucide.Icon("ellipsis", 16), Flyout = SaveMenu(l) };
        AutomationProperties.SetName(more, T("More options") + ", " + name);
        ToolTipService.SetToolTip(more, T("More options"));
        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center, Children = { play, more } };

        var row = new Grid { ColumnSpacing = 18 };
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(box);
        Grid.SetColumn(acts, 1);
        row.Children.Add(acts);
        _saveSums += Say;
        return new Border { Child = row, Padding = new Thickness(18, 0, 16, 0), BorderThickness = new Thickness(0, first ? 0 : 1, 0, 0), BorderBrush = Res("ApexHairlineBrush"), ContextFlyout = SaveMenu(l) };

        void Say()
        {
            var read = Known(l);
            var bytes = read is null ? 0 : Gain(read, _saveParts);
            gain.Text = read is null ? CleanRead(l).IsCompleted ? T("can't be read") : T("reading…") : read.Plan.Gains.Count == 0 ? T("can't be cleaned") : bytes == 0 ? T("nothing to clean") : T("{0} less", SaveSize(bytes));
            gain.Foreground = Res(bytes > 0 && box.IsChecked == true ? "ApexSuccessBrush" : "ApexMutedBrush");
            if (read?.Plan is { Refusal: { } no, Gains.Count: 0 }) ToolTipService.SetToolTip(numbers, T(no.Key, TArgs(no.Args)));
            AutomationProperties.SetName(box, $"{name}, {sub}, {size.Text}, {gain.Text}");
        }
        async Task Photo()
        {
            try
            {
                if (await FamilyPicture(l) is { } img) photo.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Bottom });
                else photo.Children.Add(Lucide.Icon("users", 24, Res("ApexIconMutedBrush"), 1.4));
            }
            catch (Exception e) { App.Log(e); }
        }
    }

    MenuFlyout SaveMenu(SaveLook l)
    {
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
        var changes = new List<MenuFlyoutItem>();
        void Item(string text, Func<Task> run, string icon, bool change = true, bool danger = false)
        {
            var i = MenuItem(text, () => _ = run(), icon, danger: danger);
            if (change) changes.Add(i);
            menu.Items.Add(i);
        }
        Item(T("Make a copy"), () => CopySaves([l.Save]), "copy", change: false);
        Item(T("Choose the pictures that stay"), () => KeepAsync([l]), "image", change: false);
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

    Border SaveCleanPanel(SavesNow now)
    {
        var s = new StackPanel { Spacing = 16 };
        var label = Muted("");
        var number = new TextBlock { FontFamily = new FontFamily("Segoe UI Variable Display"), FontSize = 36, FontWeight = FontWeights.SemiBold, LineHeight = 44 };
        var less = new TextBlock { FontSize = 14, Foreground = Res("ApexSuccessBrush"), VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 0, 7) };
        AutomationProperties.SetLiveSetting(less, AutomationLiveSetting.Polite);
        s.Children.Add(new StackPanel { Children = { label, new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Children = { number, less } } } });

        var options = new StackPanel();
        var boxes = new List<(CheckBox Box, CleanParts Parts, TextBlock Size)>();
        void Option(CleanParts parts, string name, string info, UIElement? under = null)
        {
            var g = new Grid { ColumnSpacing = 12 };
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var about = SaveText(info, 12, "ApexMutedBrush");
            about.TextWrapping = TextWrapping.Wrap;
            g.Children.Add(new StackPanel { Spacing = 1, Children = { SaveText(name, 14, strong: true), about } });
            var size = SaveText("", 14, "ApexMutedBrush");
            size.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(size, 1);
            g.Children.Add(size);
            var box = new CheckBox { Content = g, MinWidth = 0, MinHeight = 60, Padding = new Thickness(12, 0, 0, 0), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Center };
            AutomationProperties.SetName(box, name + ". " + info);
            box.Click += (_, _) =>
            {
                if (box.IsChecked == true) _saveParts |= parts;
                // (the last picture of each can't go while its copies stay)
                else _saveParts &= parts == CleanParts.RepeatedMemories ? ~AllPictures : parts == AllPictures ? ~(CleanParts.Memories | CleanParts.Generated) : ~parts;
                _saveSums?.Invoke();
            };
            boxes.Add((box, parts, size));
            var cell = new StackPanel { Children = { box } };
            if (under is not null) cell.Children.Add(under);
            options.Children.Add(new Border { Child = cell, BorderThickness = new Thickness(0, boxes.Count > 1 ? 1 : 0, 0, 0), BorderBrush = Res("ApexHairlineBrush") });
        }
        Option(CleanParts.Portraits | CleanParts.Thumbnails, T("Portraits and thumbnails"), T("The game makes them again as you play"));
        Option(CleanParts.RepeatedMemories, T("Repeated memories"), T("One of each picture stays"));
        List<SaveLook> Picked() => now.Here.Where(l => _savePicks!.Contains(l.Save.Dir)).ToList();
        var keep = Id2(Link(T("Choose the pictures that stay"), () => _ = KeepAsync(Picked())), "SaveKeep");
        (keep.FontSize, keep.Margin) = (12, new Thickness(32, -6, 0, 10));
        Option(AllPictures, T("All memories and photos"), T("These pictures are gone for good"), keep);
        s.Children.Add(options);

        var clean = MainButton("", () => _ = CleanSaves(Picked()));
        (clean.Height, clean.FontSize, clean.FontWeight) = (44, 15, FontWeights.SemiBold);
        AutomationProperties.SetAutomationId(clean, "SaveClean");
        var note = Muted(T("A copy is made first, and you can undo it."));
        s.Children.Add(new StackPanel { Spacing = 10, Children = { clean, note } });

        foreach (var l in now.Here) _ = Read(l);
        _saveSums += Say;
        var card = new Border { Child = s, Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(22, 20, 22, 20), VerticalAlignment = VerticalAlignment.Top };
        AutomationProperties.SetName(card, T("Cleaning"));
        return card;

        async Task Read(SaveLook l)
        {
            try { await CleanRead(l); if (_savesNow == now || _savesNow?.Docs == now.Docs) _saveSums?.Invoke(); }
            catch (Exception e) { App.Log(e); }
        }
        void Say()
        {
            var picked = Picked();
            var reads = picked.Select(Known).OfType<SaveRead>().ToList();
            var reading = picked.Any(l => !CleanRead(l).IsCompleted);
            foreach (var (box, parts, size) in boxes)
            {
                var on = (_saveParts & parts) == parts;
                box.IsChecked = on;
                var only = parts == AllPictures ? CleanParts.Memories | CleanParts.Generated : parts;
                size.Text = reading ? "" : SaveSize(reads.Sum(r => r.Plan.Gains.Where(g => (only & g.Key) != 0).Sum(g => g.Value.Bytes)));
                size.Foreground = Res(on ? "ApexSuccessBrush" : "ApexMutedBrush");
            }
            var gain = reads.Sum(r => Gain(r, _saveParts));
            label.Text = picked.Count switch { 0 => T("No save chosen"), 1 => T("1 save, after cleaning"), var n => T("{0} saves, after cleaning", n) };
            number.Text = SaveSize(picked.Sum(l => l.Info.Bytes) - gain);
            less.Text = gain > 0 && !reading ? T("{0} less", SaveSize(gain)) : "";
            clean.Content = reading ? T("Reading the saves…") : gain > 0 ? T("Clean {0}", SaveSize(gain)) : T("Nothing to clean");
            clean.IsEnabled = gain > 0 && !reading && SavesBlocked() is null;
            keep.IsEnabled = picked.Count > 0;
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
                        else if (c.Step == CleanStep.Verify) say(name + T("Checking it against the save"), 0.9 * part, true);
                        else say(name + T("Putting it in place"), 0.9 + 0.1 * part, false);
                    }), ct));
                }
                catch (Exception e) when (list.Count > 0) { error = e; break; }
            }
            return list;
        });
        FillSaves();
        if (done is null) return;
        var freed = T("{0} less", SaveSize(done.Sum(r => r.Before - r.After)));
        if (error is null) SavesSay(InfoBarSeverity.Success, done.Count == 1 ? T("Save cleaned") : T("{0} saves cleaned", done.Count), freed, T("Undo"), () => _ = UndoClean(done));
        else
        {
            App.Log(error);
            SavesSay(InfoBarSeverity.Warning, T("{0} of {1} saves cleaned", done.Count, todo.Count), error is OperationCanceledException ? freed : freed + ". " + ErrorText(error), T("Undo"), () => _ = UndoClean(done));
        }
    }

    async Task UndoClean(List<CleanResult> done)
    {
        var docs = Docs;
        var back = await SaveWork(T("Putting the saves back"), (say, ct) =>
        {
            say(T("Copying the save and checking the copy"), -1, true);
            foreach (var r in done) SaveClean.Undo(docs, r, ct);
            return done;
        });
        FillSaves();
        if (back is not null) SavesSay(InfoBarSeverity.Success, T("The saves are as they were before."));
    }

    async Task CopySaves(List<Save> saves)
    {
        var docs = Docs;
        if (await SaveWork(T("Making a copy"), (say, ct) =>
        {
            for (var i = 0; i < saves.Count; i++)
            {
                ct.ThrowIfCancellationRequested();
                say(saves[i].Name + "\n" + T("Copying the save and checking the copy"), saves.Count == 1 ? -1 : (double)i / saves.Count, true);
                Saves.Backup(saves[i], docs);
            }
            return saves;
        }) is null) return;
        SavesSay(InfoBarSeverity.Success, saves.Count == 1 ? T("Copy made") : T("{0} saves copied", saves.Count), "", T("Open folder"), () => OpenFolder(Saves.BackupDir(docs)));
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
        var now = await SaveWork(T("Rename save"), (say, _) => { say(T("Renaming the save and its copies"), -1, false); return SaveCopies.Rename(docs, s, name); });
        if (now is not null && now.Name != s.Name) SavesGalleryAgain(s.Name, now.Name);
        if (now is not null && _savePicks?.Remove(s.Dir) == true) _savePicks.Add(now.Dir);
        FillSaves();
        if (now is not null && now.Name != s.Name) SavesSay(InfoBarSeverity.Success, T("Renamed to {0}", now.Name), "", undo ? T("Undo") : null, () => _ = RenameTo(now, s.Name, undo: false));
    }

    async Task StoreSave(SaveLook l)
    {
        var docs = Docs;
        var now = await SaveWork(T("Keep out of the game"), (say, _) => { say(T("Moving the save"), -1, false); return SaveCopies.Store(docs, l.Save); });
        if (now is not null && now.Name != l.Save.Name) SavesGalleryAgain(l.Save.Name, now.Name);
        FillSaves();
        if (now is not null) SavesSay(InfoBarSeverity.Success, T("{0} is out of the game", FamilyName(l.Save, l.Info)), T("It is in Copies when you want it back."), T("Undo"), () => _ = PutBack(now));
    }

    async Task PutBack(Save s)
    {
        var docs = Docs;
        var now = await SaveWork(T("Put back in the game"), (say, _) => { say(T("Moving the save"), -1, false); return SaveCopies.Unstore(docs, s); });
        if (now is not null && now.Name != s.Name) SavesGalleryAgain(s.Name, now.Name);
        FillSaves();
        if (now is not null) SavesSay(InfoBarSeverity.Success, T("The save is back in the game."));
    }

    async Task DeleteSave(SaveLook l)
    {
        if (!await Confirm(T("Move this save to the Recycle Bin?"), T("{0} leaves the game. You can bring it back from the Recycle Bin, and its copies stay where they are.", FamilyName(l.Save, l.Info)), T("Move to the Recycle Bin"), danger: true)) return;
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
        var body = new StackPanel { Spacing = 14, Width = 420, Children = { text, bar } };
        using var cts = new CancellationTokenSource();
        var (running, canStop) = (true, true);
        var d = new ContentDialog { Title = title, Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
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
        else if (error is not null) { App.Log(error); SavesSay(InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(error)); }
        return result;
    }

    void SavesBusy(bool on) => _savesBusy = on;

    // a note at the foot of Saves, gone after a while (an error stays until it is closed)
    void SavesSay(InfoBarSeverity sev, string title, string text = "", string? action = null, Action? run = null)
    {
        if (_step != SavesScreen) return;
        var mine = ++_savesNote;
        SetInfo(SavesNote, sev, title, text, action, () => { SavesNote.IsOpen = false; run?.Invoke(); });
        if (sev != InfoBarSeverity.Error) After(action is null ? 7000 : 20000, () => { if (mine == _savesNote) SavesNote.IsOpen = false; });
    }
}
