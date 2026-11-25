using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using static ApexSetup.Strings;

namespace ApexSetup;

// The backups of Saves: those of the chosen save, the page of every backup, the page of the automatic ones, and what the
// launcher makes by itself (when the game closes, before Play, once a day).
public sealed partial class MainWindow
{
    static readonly (string Id, string Text)[] BackupWhys =
    [
        ("auto", "Automatic"), ("mine", "Made by you"), ("clean", "Before cleaning"), ("restore", "Before restoring"), ("game", "By the game"),
    ];
    static string WhyId(SavePoint p) => p.Kind == CopyKind.Game ? "game" : SaveBackups.IsAuto(p.Note) ? "auto" : p.Note == SaveCopies.BeforeClean ? "clean" : p.Note == SaveCopies.BeforeRestore ? "restore" : "mine";
    static string WhyText(SavePoint p) => p.Note switch
    {
        SaveBackups.AutoClose => T("Automatic, when the game closed"),
        SaveBackups.AutoPlay => T("Automatic, before playing"),
        SaveBackups.AutoDay => T("Automatic, once a day"),
        _ => T(BackupWhys.First(w => w.Id == WhyId(p)).Text),
    };
    string BackupTitle(SavePoint p) => p.Label ?? SaveDay(p.When, hour: true);
    string BackupSub(SavePoint p) => p.Label is null ? WhyText(p) : SaveDay(p.When, hour: true) + " · " + WhyText(p);
    string Lower(string t) => char.ToLower(t[0], LangCulture()) + t[1..]; // (inside a sentence)
    static string BackupCount(int n) => n == 1 ? T("1 backup") : T("{0} backups", n);

    // ---------- of the chosen save ----------

    const int BackupsShown = 3;
    bool _saveAllBackups; // (the list of the chosen save shown whole)

    void BackupsPart(StackPanel host, SavesNow now, SaveEntry e)
    {
        var points = e.Points;
        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        if (e.Kind == SaveKindOf.Here)
        {
            var make = Id(Button("", () => CopySaves([e.Look!.Save])), "SaveBackup");
            make.Content = Labeled("copy", T("Back up now"));
            make.IsEnabled = SavesBlocked() is null; // (the game may be writing the save)
            head.Children.Add(Dims(make));
        }
        host.Children.Add(Section(T("Backups"), head));
        var list = new StackPanel { Spacing = 4 };
        AutomationProperties.SetName(list, T("Backups"));
        if (now.FolderGone is { } gone) list.Children.Add(Warn(T("The backup folder isn't available"), gone, T("Change"), () => SavesGo("auto")));
        else if (points.Count == 0) list.Children.Add(Row("copy", T("No backups yet"), T("Make one now, or let the launcher make them."), null));
        foreach (var p in _saveAllBackups ? points : points.Take(BackupsShown)) list.Children.Add(BackupRow(e, p));
        if (points.Count > BackupsShown)
        {
            var line = new Grid();
            var text = PageSub(_saveAllBackups ? T("Show fewer") : T("Show all {0} backups", points.Count));
            text.VerticalAlignment = VerticalAlignment.Center;
            line.Children.Add(text);
            var mark = Lucide.Icon(_saveAllBackups ? "chevron-up" : "chevron-down", 16, Res("ApexSoftTextBrush"));
            mark.HorizontalAlignment = HorizontalAlignment.Right;
            line.Children.Add(mark);
            var all = new Button { Content = line, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, MinHeight = 44, Padding = new Thickness(20, 0, 22, 0), CornerRadius = new CornerRadius(8),
                Background = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderBrush = Res("ApexCardBorderBrush") };
            all.Click += (_, _) => { _saveAllBackups = !_saveAllBackups; SavesAgain(); };
            AutomationProperties.SetName(all, text.Text);
            all.BorderThickness = new Thickness(0);
            var more = new Border { Child = all };
            Soft(more);
            list.Children.Add(Join(more));
        }
        if (e.Kind == SaveKindOf.Here) list.Children.Add(AutoRow(e));
        host.Children.Add(list);
    }

    Border BackupRow(SaveEntry e, SavePoint p)
    {
        var g = new Grid { ColumnSpacing = 16, VerticalAlignment = VerticalAlignment.Center };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto, GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var (title, sub) = (BackupTitle(p), BackupSub(p));
        // why it was made, at a glance; a pinned one says that instead
        var mark = RowIcon(p.Pinned ? "pin" : WhyId(p) switch { "auto" => "calendar-clock", "clean" => "sparkles", "restore" => "undo-2", "game" => "gamepad-2", _ => "copy" });
        if (p.Pinned) { ToolTipService.SetToolTip(mark, T("Pinned: never deleted by itself")); }
        g.Children.Add(mark);
        var words = (StackPanel)Col(SaveText(title, 14, strong: true), SaveText(sub, 13, "ApexMutedBrush"));
        (words.Spacing, words.VerticalAlignment) = (2, VerticalAlignment.Center);
        ToolTipService.SetToolTip(words, title + "\n" + sub);
        var size = SaveText(SaveSize(p.Stored), 13, "ApexSoftTextBrush");
        size.VerticalAlignment = VerticalAlignment.Center;
        var (text, go) = e.Kind == SaveKindOf.Here ? (T("Restore"), (Func<Task>)(() => RestoreAsync(e, p)))
            : (T("Bring back"), () => BringBack(e.Name, (d, ct) => p.Kind == CopyKind.Game ? SaveCopies.Rescue(d, e.Look!.Save, ct) : SaveCopies.Restore(d, new Save(Path.Combine(d, "Saves", e.Save + ".sims3"), e.Save, p.When), p, ct)));
        var act = Dims(Button(text, go));
        (act.Height, act.VerticalAlignment, act.IsEnabled) = (36, VerticalAlignment.Center, SavesBlocked() is null);
        AutomationProperties.SetName(act, $"{text}, {title}, {sub}");
        foreach (var (x, at) in new (FrameworkElement, int)[] { (words, 1), (size, 2), (act, 3) }) { Grid.SetColumn(x, at); g.Children.Add(x); }
        var more = MoreButton(T("More options") + ", " + title, BackupMenu(e, p));
        Grid.SetColumn(more, 4);
        g.Children.Add(more);
        var card = new Border { Child = g, Padding = new Thickness(20, 12, 20, 12), MinHeight = 68, ContextFlyout = BackupMenu(e, p) };
        Soft(card);
        return Join(card);
    }

    static Button MoreButton(string name, MenuFlyout menu)
    {
        var b = new Button { Width = 36, Height = 36, Padding = new Thickness(0), Content = Lucide.Icon("ellipsis", 16), Flyout = menu, VerticalAlignment = VerticalAlignment.Center,
            Background = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0) };
        AutomationProperties.SetName(b, name);
        ToolTipService.SetToolTip(b, T("More options"));
        return b;
    }

    MenuFlyout BackupMenu(SaveEntry e, SavePoint p)
    {
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
        var mine = p.Kind == CopyKind.Launcher;
        void Item(string text, Func<Task> run, string icon, bool on = true, bool danger = false)
        {
            var i = MenuItem(text, () => Safe(run()), icon, danger: danger);
            i.IsEnabled = on && !DryRun;
            menu.Items.Add(i);
        }
        if (e.Kind == SaveKindOf.Here) Item(T("Open as another save"), () => OpenAsAnother(e, p), "layers", SavesBlocked() is null);
        if (mine)
        {
            Item(T("Give it a name"), () => NameBackup(p), "pencil");
            if (p.Label is not null) Item(T("Remove the name"), () => MarkBackups([p], q => (null, q.Pinned)), "x");
            Item(p.Pinned ? T("Unpin") : T("Pin, never delete"), () => MarkBackups([p], q => (q.Label, !q.Pinned)), p.Pinned ? "pin-off" : "pin");
            Item(T("Check the file"), () => CheckBackup(p), "check");
        }
        Item(T("Export"), () => ExportBackups([(e, p)]), "upload", mine || SavesBlocked() is null);
        if (!SaveBackups.CanDelete(p)) return menu;
        menu.Items.Add(new MenuFlyoutSeparator());
        Item(BackupGone(p.Path), () => DeleteBackups([p]), "trash-2", mine || SavesBlocked() is null, danger: true);
        return menu;
    }
    static string BackupGone(string path) => SaveBackups.HasBin(path) ? T("Move to the Recycle Bin") : T("Delete");

    // the save's own automatic backups
    Border AutoRow(SaveEntry e)
    {
        var name = e.Save;
        var of = BackupChoices.Load().Of(name);
        var on = of.OnClose || of.BeforePlay || of.Daily;
        var when = new[] { of.OnClose ? T("When the game closes") : null, of.BeforePlay ? T("Before playing") : null, of.Daily ? T("Once a day") : null }.OfType<string>();
        var text = !on ? T("Off") : string.Join(", ", when.Select((w, i) => i == 0 ? w : Lower(w))) + " · " + (of.Keep == 1 ? T("keeps the newest one") : T("keeps the newest {0}", of.Keep));
        if (_autoBackup is { IsCompleted: false }) text = T("Backing up…");
        else if (on && _autoError is { } bad) text = T("The last one did not work: {0}", bad);
        var sw = new ToggleSwitch { IsOn = on, OnContent = "", OffContent = "", MinWidth = 0, Width = 40, IsEnabled = !DryRun };
        AutomationProperties.SetName(sw, T("Automatic backups of this save"));
        AutomationProperties.SetAutomationId(sw, "SaveAuto");
        sw.Toggled += (_, _) => ChangeBackups(c =>
        {
            var own = c.Saves.TryGetValue(name, out var o) ? o : c.Saves[name] = new();
            own.Off = !sw.IsOn;
            if (sw.IsOn && c.Of(name) is (false, false, false, _)) own.OnClose = true;
            if (own is { Off: false, OnClose: null, BeforePlay: null, Daily: null, Keep: null }) c.Saves.Remove(name);
        });
        var change = Link(T("Adjust"), () => Safe(AutoOfSave(e)));
        (change.VerticalAlignment, change.IsEnabled) = (VerticalAlignment.Center, !DryRun);
        AutomationProperties.SetName(change, T("Adjust") + ", " + T("Automatic backups of this save"));
        var row = Row("shield-check", T("Automatic backups of this save"), text, new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, Children = { change, sw } });
        row.Padding = new Thickness(20, 10, 20, 10);
        return row;
    }

    void ChangeBackups(Action<BackupChoices> edit)
    {
        if (DryRun) return;
        try { BackupChoices.Change(edit); }
        catch (Exception x) { App.Log(x); SavesSay(InfoBarSeverity.Error, T("That did not work"), ErrorText(x)); }
        _nextDaily = Math.Min(_nextDaily, Environment.TickCount64 + 5000); // (once a day, just turned on, does not wait)
        SavesAgain();
    }

    // when this save gets one by itself and how many stay; as every save, or its own
    async Task AutoOfSave(SaveEntry e)
    {
        var choices = BackupChoices.Load();
        var of = choices.Of(e.Save);
        var own = choices.Saves.ContainsKey(e.Save);
        var boxes = new StackPanel { Spacing = 4, Margin = new Thickness(0, 4, 0, 0) };
        CheckBox Box(string text, bool on) { var b = new CheckBox { Content = text, IsChecked = on }; boxes.Children.Add(b); return b; }
        var (close, play, day) = (Box(T("When the game closes"), of.OnClose), Box(T("Before playing"), of.BeforePlay), Box(T("Once a day"), of.Daily));
        var keep = KeepBox(of.Keep);
        keep.Header = T("Backups to keep");
        (keep.HorizontalAlignment, keep.Margin) = (HorizontalAlignment.Stretch, new Thickness(0, 12, 0, 0));
        boxes.Children.Add(keep);
        var (same, mine) = (new RadioButton { Content = T("As for every save"), IsChecked = !own }, new RadioButton { Content = T("Only for this save"), IsChecked = own });
        void Say() => boxes.Visibility = mine.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        same.Checked += (_, _) => Say();
        mine.Checked += (_, _) => Say();
        Say();
        boxes.Margin = new Thickness(28, 0, 0, 0);
        var d = new ContentDialog { Title = T("Automatic backups of {0}", e.Name), Content = new StackPanel { Spacing = 4, Width = 400, Children = { same, mine, boxes } }, PrimaryButtonText = T("Save"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        ChangeBackups(c =>
        {
            if (same.IsChecked == true) c.Saves.Remove(e.Save);
            else c.Saves[e.Save] = new() { OnClose = close.IsChecked, BeforePlay = play.IsChecked, Daily = day.IsChecked, Keep = (int)((ComboBoxItem)keep.SelectedItem).Tag };
        });
    }

    static ComboBox Numbers(int now, IEnumerable<int> of, Func<int, string> text)
    {
        var box = new ComboBox { MinWidth = 112 };
        foreach (var n in of.Append(now).Distinct().OrderBy(n => n == 0 ? int.MaxValue : n))
        {
            var i = new ComboBoxItem { Content = text(n), Tag = n };
            box.Items.Add(i);
            if (n == now) box.SelectedItem = i;
        }
        return box;
    }
    ComboBox KeepBox(int now) => Numbers(now, [1, 3, 5, 10, 20], n => n.ToString(LangCulture()));

    // ---------- what is done with one ----------

    async Task NameBackup(SavePoint p)
    {
        if (await AskName(T("Give it a name"), p.Label ?? "", T("Save")) is { } name) await MarkBackups([p], q => (name, q.Pinned));
    }

    async Task MarkBackups(List<SavePoint> points, Func<SavePoint, (string? Label, bool Pinned)> to)
    {
        try { await Task.Run(() => { foreach (var p in points) { var (label, pin) = to(p); SaveBackups.Mark(p, label, pin); } }); }
        catch (Exception x) { App.Log(x); SavesSay(InfoBarSeverity.Error, T("That did not work"), ErrorText(x)); }
        FillSaves();
    }

    async Task CheckBackup(SavePoint p)
    {
        if (await SaveWork(T("Checking the file"), (say, ct) => { say(BackupTitle(p), -1, true); SaveBackups.Verify(p, ct); return p; }) is not null)
            SavesSay(InfoBarSeverity.Success, T("The file is whole"), BackupTitle(p));
    }

    async Task DeleteBackups(List<SavePoint> points)
    {
        var bin = SaveBackups.HasBin(points[0].Path);
        if (!bin && !await Confirm(points.Count == 1 ? T("Delete this backup?") : T("Delete {0} backups?", points.Count), T("This drive has no Recycle Bin, so you can't bring them back."), T("Delete"), danger: true)) return;
        if (await SaveWork(T("Removing backups"), (say, _) => { say(BackupCount(points.Count), -1, false); SaveBackups.Delete(points); return points; }) is not null)
            SavesSay(InfoBarSeverity.Success, bin ? points.Count == 1 ? T("Backup moved to the Recycle Bin") : T("{0} backups moved to the Recycle Bin", points.Count) : points.Count == 1 ? T("Backup deleted") : T("{0} backups deleted", points.Count));
        FillSaves();
    }

    // a save that is gone, with everything that is left of it; or one kept out of the game
    async Task DeleteGone(SaveEntry e)
    {
        var bin = e.Kind == SaveKindOf.Stored || e.Points.All(p => SaveBackups.HasBin(p.Path));
        var text = e.Kind == SaveKindOf.Stored ? T("The save goes to the Recycle Bin, and its backups stay where they are.")
            : e.Points.Count == 1 ? T("Its last backup goes to the Recycle Bin.") : T("Its {0} backups go to the Recycle Bin.", e.Points.Count);
        text = bin ? text + " " + T("Empty the Recycle Bin to delete it for good.") : T("This drive has no Recycle Bin, so you can't bring it back.");
        // (the next of the same kind is chosen after it, to delete several in a row)
        var same = _savesNow?.Entries.Where(x => x.Kind == e.Kind).ToList() ?? [];
        var at = same.FindIndex(x => x.Key == e.Key);
        var next = same.ElementAtOrDefault(at + 1) ?? same.ElementAtOrDefault(at - 1);
        if (!await Confirm(T("Delete {0}?", e.Name), text, T("Delete"), danger: true)) return;
        if (await SaveWork(T("Delete"), (say, _) =>
        {
            say(T("Moving the save"), -1, false);
            if (e.Kind == SaveKindOf.Stored) SaveCopies.Delete(e.Look!.Save); else SaveBackups.Delete(e.Points);
            return e;
        }) is null) { FillSaves(); return; }
        // (before the list is read again: what it shows still has the one that went)
        if (next is not null) SavePicked(next.Key);
        FillSaves();
        SavesSay(InfoBarSeverity.Success, T("{0} deleted", e.Name));
    }

    async Task ExportBackups(List<(SaveEntry Of, SavePoint Point)> what)
    {
        string FileName(SaveEntry e, SavePoint p) => p.Kind == CopyKind.Launcher ? Path.GetFileName(p.Path) : $"{e.Save} {p.When:yyyy-MM-dd HH.mm.ss}.zip";
        string? folder, single = null;
        if (what.Count == 1)
        {
            var picker = new Microsoft.Windows.Storage.Pickers.FileSavePicker(AppWindow.Id) { SuggestedFileName = Path.GetFileNameWithoutExtension(FileName(what[0].Of, what[0].Point)), DefaultFileExtension = ".zip" };
            picker.FileTypeChoices.Add("Zip", new List<string> { ".zip" });
            if (await picker.PickSaveFileAsync() is not { } file) return;
            (single, folder) = (file.Path, Path.GetDirectoryName(file.Path)!);
        }
        else if (await new Microsoft.Windows.Storage.Pickers.FolderPicker(AppWindow.Id).PickSingleFolderAsync() is { } f) folder = f.Path;
        else return;
        var done = await SaveWork(T("Exporting"), (say, ct) =>
        {
            for (var i = 0; i < what.Count; i++)
            {
                ct.ThrowIfCancellationRequested();
                say(what[i].Of.Name + " · " + BackupTitle(what[i].Point), what.Count == 1 ? -1 : (double)i / what.Count, true);
                // (a folder may hold an older export of the same name: it is the same backup, or takes the next name)
                var to = single ?? Path.Combine(folder, FileName(what[i].Of, what[i].Point));
                for (var n = 2; single is null && File.Exists(to); n++) to = Path.Combine(folder, Path.GetFileNameWithoutExtension(FileName(what[i].Of, what[i].Point)) + $" ({n}).zip");
                SaveBackups.Export(what[i].Point, to);
            }
            return what;
        });
        if (done is not null) SavesSay(InfoBarSeverity.Success, what.Count == 1 ? T("Backup exported") : T("{0} backups exported", what.Count), "", T("Open folder"), () => OpenFolder(folder));
    }

    async Task ImportSave()
    {
        var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
        picker.FileTypeFilter.Add(".zip");
        if (await picker.PickSingleFileAsync() is not { } file) return;
        var docs = Docs;
        var made = await SaveWork(T("Importing a save"), (say, ct) => { say(T("Copying the save and checking the copy"), -1, true); return SaveBackups.Import(docs, file.Path, ct); });
        if (made is not null) _saveWant = made.Dir;
        FillSaves();
        if (made is not null) SavesSay(InfoBarSeverity.Success, T("The new save is called {0}", made.Name));
    }

    // ---------- going back to one ----------

    async Task RestoreAsync(SaveEntry e, SavePoint p)
    {
        var (docs, look) = (Docs, e.Look!);
        var missing = await Task.Run(() => SaveBackups.MissingMods(docs, p));
        RadioButton Choice(string title, string sub, bool on)
        {
            var b = new RadioButton { Content = new StackPanel { Spacing = 2, Children = { SaveText(title, 14, strong: true), SaveText(sub, 12, "ApexMutedBrush") } }, IsChecked = on, GroupName = "restore", MinHeight = 0, VerticalContentAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Stretch, Padding = new Thickness(12, 0, 0, 0) };
            AutomationProperties.SetName(b, title + ". " + sub);
            return b;
        }
        var swap = Choice(T("Replace the current save"), T("It becomes one more backup"), true);
        var beside = Choice(T("Open as another save"), T("Both stay in the game"), false);
        var body = new StackPanel { Spacing = 6, Width = 432 };
        body.Children.Add(new TextBlock { Text = string.Join(" · ", new[] { e.Name, p.Label is null ? WhyText(p) : SaveDay(p.When, hour: true), SaveSize(p.Stored) }), FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 10) });
        foreach (var b in new[] { swap, beside })
        {
            var card = new Border { Child = b, CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), Padding = new Thickness(16, 11, 16, 11) };
            void Paint() => (card.Background, card.BorderBrush) = (Res(b.IsChecked == true ? "ApexAccentSoftBrush" : "ApexCardBrush"), Res(b.IsChecked == true ? "ApexAccentLineBrush" : "ApexCardBorderBrush"));
            b.Checked += (_, _) => Paint();
            b.Unchecked += (_, _) => Paint();
            Paint();
            body.Children.Add(card);
        }
        if (missing.Count > 0)
        {
            var names = new TextBlock { Text = string.Join('\n', missing), FontSize = 12, Foreground = Res("ApexWarningTextBrush"), TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
            var more = new ScrollViewer { Content = names, MaxHeight = 140, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 8, 0, 0) };
            var few = new TextBlock { Text = string.Join(", ", missing.Take(2)) + (missing.Count > 2 ? " " + T("and {0} more", missing.Count - 2) : ""), FontSize = 12, Foreground = Res("ApexWarningTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis };
            var see = Link(T("See"), () => { });
            see.VerticalAlignment = VerticalAlignment.Center;
            AutomationProperties.SetName(see, T("See the mods"));
            see.Click += (_, _) => { (more.Visibility, few.Visibility) = (Visibility.Visible, Visibility.Collapsed); see.Visibility = Visibility.Collapsed; };
            var line = new Grid { ColumnSpacing = 12 };
            foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) line.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
            line.Children.Add(Lucide.Icon("triangle-alert", 16, Res("ApexWarningBrush")));
            var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center, Children =
            {
                new TextBlock { Text = missing.Count == 1 ? T("1 mod from that day isn't on now") : T("{0} mods from that day aren't on now", missing.Count), FontSize = 13, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = Res("ApexWarningBrush"), TextWrapping = TextWrapping.Wrap }, few, more,
            } };
            Grid.SetColumn(words, 1);
            line.Children.Add(words);
            Grid.SetColumn(see, 2);
            line.Children.Add(see);
            body.Children.Add(new Border { Child = line, Padding = new Thickness(16, 10, 14, 10), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), Margin = new Thickness(0, 6, 0, 0), Background = Res("ApexWarningSoftBrush"), BorderBrush = Res("ApexWarningLineBrush") });
        }
        var d = new ContentDialog { Title = p.Label is { } l ? T("Restore “{0}”", l) : T("Restore {0}", Lower(SaveDay(p.When, hour: true))), Content = body, PrimaryButtonText = T("Restore"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        if (beside.IsChecked == true) await OpenAsAnother(e, p); else await RestoreSave(look.Save, p, e.Name);
    }

    async Task OpenAsAnother(SaveEntry e, SavePoint p)
    {
        var docs = Docs;
        var made = await SaveWork(T("Open as another save"), (say, ct) => { say(T("Copying the save and checking the copy"), -1, true); return SaveCopies.RestoreAs(docs, e.Look!.Save, p, ct); });
        if (made is not null) _saveWant = made.Dir;
        FillSaves();
        if (made is not null) SavesSay(InfoBarSeverity.Success, T("The new save is called {0}", made.Name));
    }

    // ---------- several saves cleaned at once ----------

    async Task CleanSeveral(SavesNow now)
    {
        var boxes = new List<(CheckBox Box, SaveLook Look)>();
        var rows = new StackPanel();
        var d = new ContentDialog { Title = T("Clean several saves"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        foreach (var l in now.Here)
        {
            var g = new Grid { ColumnSpacing = 14 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(SavePhoto(l, 36));
            var name = SaveText(EntryName(now, now.Entries.First(e => e.Look == l)), 14, strong: true);
            name.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(name, 1);
            g.Children.Add(name);
            var size = SaveText(T("reading…"), 13, "ApexMutedBrush");
            size.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(size, 2);
            g.Children.Add(size);
            var box = new CheckBox { Content = g, MinHeight = 52, MinWidth = 0, Padding = new Thickness(12, 0, 0, 0), IsEnabled = false, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Center };
            box.Resources["CheckBoxCheckBackgroundStrokeUnchecked"] = QuietBox;
            AutomationProperties.SetName(box, name.Text);
            box.Click += (_, _) => Sum();
            boxes.Add((box, l));
            rows.Children.Add(new Border { Child = box, Padding = new Thickness(16, 0, 16, 0), BorderThickness = new Thickness(0, rows.Children.Count > 0 ? 1 : 0, 0, 0), BorderBrush = Res("ApexHairlineBrush") });
            _ = Read();

            async Task Read()
            {
                try { await CleanRead(l); } catch (Exception x) { App.Log(x); }
                var gain = Known(l) is { } r ? Gain(r, _saveParts) : 0;
                size.Text = gain > 0 ? SaveSize(gain) : T("nothing to clean");
                size.Foreground = Res(gain > 0 ? "ApexSuccessBrush" : "ApexMutedBrush");
                (box.IsEnabled, box.IsChecked) = (gain > 0, gain > 0);
                Sum();
            }
        }
        List<SaveLook> Picked() => boxes.Where(b => b.Box.IsChecked == true).Select(b => b.Look).ToList();
        void Sum()
        {
            var gain = Picked().Sum(l => Known(l) is { } r ? Gain(r, _saveParts) : 0);
            d.PrimaryButtonText = gain > 0 ? T("Clean {0}", SaveSize(gain)) : T("Nothing to clean");
            d.IsPrimaryButtonEnabled = gain > 0 && SavesBlocked() is null;
        }
        var what = CleanOptions.Where(o => (_saveParts & o.Parts) == o.Parts).Select(o => T(o.Name)).ToList();
        d.Content = new StackPanel
        {
            Width = 440, Spacing = 14, Children =
            {
                new TextBlock { Text = what.Count > 0 ? string.Join(", ", what) : T("Nothing is turned on in Clean"), FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap },
                new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(0), Child = new ScrollViewer { Content = rows, MaxHeight = 340 } },
                PageSub(T("Each save gets a backup first.")),
            },
        };
        Sum();
        if (await ShowDialog(d) == ContentDialogResult.Primary) await CleanSaves(Picked());
    }

    // ---------- the page of the automatic ones ----------

    StackPanel SavesSubHead(string title, string? sub, FrameworkElement? act = null)
    {
        var back = BackButtonTo(T("Saves"), () => SavesGo(""), "SavesBack");
        return new StackPanel { Spacing = 4, Children = { back, PageHead(title, sub, act) } };
    }

    void SavesGo(string page)
    {
        (_savesPage, _savesSig) = (page, "");
        _allPicks.Clear();
        SavesNote.IsOpen = false;
        Scroller.ChangeView(null, 0, null, true);
        FillSaves();
    }

    void DrawAutomatic(SavesNow now)
    {
        var (c, docs) = (BackupChoices.Load(), now.Docs);
        var page = new StackPanel { Spacing = 4, MaxWidth = 920 };
        page.Children.Add(SavesSubHead(T("Automatic backups"), null));
        page.Children.Add(Section(T("When")));
        page.Children.Add(Toggle("shield-check", T("When the game closes"), T("Only the saves that changed"), c.OnClose, on => ChangeBackups(x => x.OnClose = on), !DryRun));
        page.Children.Add(Toggle("play", T("Before playing"), T("Takes a few seconds for each save"), c.BeforePlay, on => ChangeBackups(x => x.BeforePlay = on), !DryRun));
        page.Children.Add(Toggle("calendar-clock", T("Once a day"), T("While the launcher is open and the game is closed"), c.Daily, on => ChangeBackups(x => x.Daily = on), !DryRun));

        page.Children.Add(Section(T("How much to keep")));
        var keep = KeepBox(c.Keep);
        AutomationProperties.SetName(keep, T("Backups of each save"));
        keep.SelectionChanged += (_, _) => ChangeBackups(x => x.Keep = (int)((ComboBoxItem)keep.SelectedItem).Tag);
        page.Children.Add(Row("layers", T("Backups of each save"), T("Older automatic ones go to the Recycle Bin."), keep));
        var limit = Numbers(c.LimitGb, [2, 5, 10, 20, 50, 0], n => n == 0 ? T("No limit") : n + " GB");
        AutomationProperties.SetName(limit, T("Space limit"));
        limit.SelectionChanged += (_, _) => ChangeBackups(x => x.LimitGb = (int)((ComboBoxItem)limit.SelectedItem).Tag);
        var all = Id(Button(T("See all"), () => SavesGo("all")), "SavesAll");
        var used = now.Points.Values.SelectMany(p => p).Where(SaveBackups.IsOwn).Sum(p => p.Stored);
        page.Children.Add(Row("hard-drive", T("Space limit"), T("{0} used", SaveSize(used)), new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { limit, all } }));

        page.Children.Add(Section(T("Where and how")));
        var change = new Button { Content = T("Change"), MinHeight = 36, Padding = new Thickness(14, 0, 14, 0), IsEnabled = !DryRun };
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
        menu.Items.Add(MenuItem(T("Choose a folder"), () => Safe(PickBackupFolder()), "folder"));
        var usual = MenuItem(T("Use the Documents folder"), () => Safe(MoveBackups(null)), "undo-2");
        usual.IsEnabled = c.FolderOf(docs) is not null;
        menu.Items.Add(usual);
        change.Flyout = menu;
        AutomationProperties.SetName(change, T("Change") + ", " + T("Folder"));
        var dir = Saves.BackupDir(docs);
        page.Children.Add(Row("folder", T("Folder"), now.FolderGone is null ? now.Free is { } free ? T("{0} · {1} free", dir, SaveSize(free)) : dir : T("{0} isn't available", dir),
            new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { Dims(Open()), Dims(change) } }));
        page.Children.Add(Toggle("check", T("Check each backup"), T("Reads everything again after copying"), c.Check, on => ChangeBackups(x => x.Check = on), !DryRun));
        page.Children.Add(Toggle("blocks", T("Remember the mods that are on"), T("Tells you which are missing when you restore"), c.RememberMods, on => ChangeBackups(x => x.RememberMods = on), !DryRun));
        SavesHost.Children.Add(page);

        Button Open() { var b = Button(T("Open"), () => OpenFolder(dir)); b.IsEnabled = now.FolderGone is null; AutomationProperties.SetName(b, T("Open folder")); return b; }
    }

    async Task PickBackupFolder()
    {
        if (await new Microsoft.Windows.Storage.Pickers.FolderPicker(AppWindow.Id).PickSingleFolderAsync() is { } f) await MoveBackups(f.Path);
    }

    async Task MoveBackups(string? folder)
    {
        var docs = Docs;
        var gone = _savesNow?.FolderGone is { } f && !Directory.Exists(f); // (what was there can't be reached: only the choice changes)
        if (await SaveWork(T("Moving the backups"), (say, ct) => { say(T("Reading the backups…"), -1, true); SaveBackups.MoveTo(docs, folder, (i, n) => say(T("{0} of {1}", i + 1, n), (double)i / n, true), ct, gone); return docs; }) is not null)
            SavesSay(InfoBarSeverity.Success, T("The backups are in the new folder"), "", T("Open folder"), () => OpenFolder(Saves.BackupDir(docs)));
        _savesSig = "";
        FillSaves();
    }

    // ---------- the page of every backup ----------

    string _allSave = "", _allWhy = "";
    readonly HashSet<string> _allPicks = new(StringComparer.OrdinalIgnoreCase);

    void DrawAllBackups(SavesNow now)
    {
        var every = now.Entries.SelectMany(e => e.Points.Select(p => (Of: e, Point: p))).OrderByDescending(x => x.Point.When).ToList();
        _allPicks.IntersectWith(every.Select(x => x.Point.Path));
        var bySave = new ComboBox { MinWidth = 150, MaxWidth = 220 };
        AutomationProperties.SetName(bySave, T("Which save"));
        void Pick(ComboBox box, string id, string text, string at) { var i = new ComboBoxItem { Content = text, Tag = id }; box.Items.Add(i); if (id == at) box.SelectedItem = i; }
        Pick(bySave, "", T("All saves"), _allSave);
        foreach (var e in now.Entries.Where(e => e.Points.Count > 0)) Pick(bySave, e.Key, EntryName(now, e), _allSave);
        if (now.Entries.Any(e => e.Kind == SaveKindOf.Gone)) Pick(bySave, "-gone", T("Gone from the game"), _allSave);
        if (bySave.SelectedIndex < 0) (_allSave, bySave.SelectedIndex) = ("", 0);
        var byWhy = new ComboBox { MinWidth = 150 };
        AutomationProperties.SetName(byWhy, T("Why it was made"));
        Pick(byWhy, "", T("All reasons"), _allWhy);
        foreach (var w in BackupWhys) Pick(byWhy, w.Id, T(w.Text), _allWhy);
        bySave.SelectionChanged += (_, _) => { _allSave = (string)((ComboBoxItem)bySave.SelectedItem).Tag; _allPicks.Clear(); SavesAgain(); };
        byWhy.SelectionChanged += (_, _) => { _allWhy = (string)((ComboBoxItem)byWhy.SelectedItem).Tag; _allPicks.Clear(); SavesAgain(); };
        var used = SaveSize(every.Where(x => x.Point.Kind == CopyKind.Launcher).Sum(x => x.Point.Stored));
        var page = new StackPanel { Spacing = 14, Margin = new Thickness(0, 0, 0, 72) };
        page.Children.Add(SavesSubHead(T("All backups"), BackupCount(every.Count) + ", " + used, new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { bySave, byWhy } }));
        var shown = every.Where(x => (_allSave == "" || x.Of.Key == _allSave || _allSave == "-gone" && x.Of.Kind == SaveKindOf.Gone) && (_allWhy == "" || WhyId(x.Point) == _allWhy)).ToList();
        SavesHost.Children.Add(page);
        if (shown.Count == 0) { page.Children.Add(EmptyPlace("copy", T("No backups here yet."))); return; }

        var sync = new List<Action>();
        var mine = shown.Where(x => SaveBackups.CanDelete(x.Point)).ToList();
        var all = new CheckBox { MinWidth = 0, MinHeight = 32, IsThreeState = false };
        AutomationProperties.SetName(all, T("Choose all"));
        all.Resources["CheckBoxCheckBackgroundStrokeUnchecked"] = QuietBox;
        all.Click += (_, _) => { if (_allPicks.Count == mine.Count) _allPicks.Clear(); else foreach (var x in mine) _allPicks.Add(x.Point.Path); Say(); };
        var head = Columns(all, null, SaveText(T("Save file"), 12, strong: true), SaveText(T("When"), 12, "ApexMutedBrush"), SaveText(T("Reason"), 12, "ApexMutedBrush"), SaveText(T("Size"), 12, "ApexMutedBrush"), null);
        var rows = new StackPanel();
        rows.Children.Add(new Border { Child = head, MinHeight = 40, Padding = new Thickness(12, 0, 4, 0), BorderThickness = new Thickness(0, 0, 0, 1), BorderBrush = Res("ApexHairlineBrush") });
        foreach (var (of, p) in shown)
        {
            var box = new CheckBox { MinWidth = 0, MinHeight = 32, IsEnabled = SaveBackups.CanDelete(p) };
            box.Resources["CheckBoxCheckBackgroundStrokeUnchecked"] = QuietBox;
            AutomationProperties.SetName(box, $"{of.Name}, {BackupTitle(p)}, {WhyText(p)}, {SaveSize(p.Stored)}");
            if (!box.IsEnabled) ToolTipService.SetToolTip(box, T("The game keeps this one"));
            box.Click += (_, _) => { if (box.IsChecked == true) _allPicks.Add(p.Path); else _allPicks.Remove(p.Path); Say(); };
            var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            var title = SaveText(p.Label ?? EntryName(now, of), 14, strong: true);
            name.Children.Add(title);
            if (p.Pinned) { var pin = Lucide.Icon("pin", 14, Res("ApexAccentBrush")); ToolTipService.SetToolTip(pin, T("Pinned: never deleted by itself")); name.Children.Add(pin); }
            ToolTipService.SetToolTip(name, p.Label is null ? of.Name : p.Label + "\n" + of.Name);
            var row = new Border { MinHeight = 56, Padding = new Thickness(12, 0, 4, 0), BorderThickness = new Thickness(0, 0, 0, 1), BorderBrush = Res("ApexHairlineBrush"), ContextFlyout = BackupMenu(of, p),
                Child = Columns(box, SavePhoto(of.Look, 40), name, SaveText(SaveDay(p.When, hour: true), 13, "ApexSoftTextBrush"), SaveText(T(BackupWhys.First(w => w.Id == WhyId(p)).Text), 13, "ApexSoftTextBrush"), SaveText(SaveSize(p.Stored), 13, "ApexSoftTextBrush"), MoreButton(T("More options") + ", " + of.Name + ", " + BackupTitle(p), BackupMenu(of, p))) };
            sync.Add(() => { box.IsChecked = _allPicks.Contains(p.Path); row.Background = box.IsChecked == true ? Res("ApexRaisedBrush") : null; });
            rows.Children.Add(row);
        }
        page.Children.Add(rows);
        Say();

        // box, picture, name, day, why, size, menu
        Grid Columns(params FrameworkElement?[] cells)
        {
            var g = new Grid { ColumnSpacing = 16, VerticalAlignment = VerticalAlignment.Center };
            var tight = Scroller.ActualWidth < 860;
            foreach (var w in new double[] { 20, 40, -1, tight ? 152 : 160, tight ? 0 : 150, 72, 36 }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w < 0 ? new GridLength(1, GridUnitType.Star) : new GridLength(w) });
            for (var i = 0; i < cells.Length; i++)
                if (cells[i] is { } x)
                {
                    x.VerticalAlignment = VerticalAlignment.Center;
                    if (i == 5) x.HorizontalAlignment = HorizontalAlignment.Right;
                    Grid.SetColumn(x, i);
                    g.Children.Add(x);
                }
            return g;
        }
        void Say()
        {
            foreach (var s in sync) s();
            all.IsChecked = mine.Count > 0 && _allPicks.Count == mine.Count;
            AllBar(mine.Where(x => _allPicks.Contains(x.Point.Path)).ToList());
        }
    }

    // what can be done with the chosen ones, at the foot of the window
    void AllBar(List<(SaveEntry Of, SavePoint Point)> picked)
    {
        SavesBar.Visibility = picked.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        SavesNote.Margin = new Thickness(24, 0, 24, picked.Count > 0 ? 76 : 16);
        if (picked.Count == 0) { SavesBar.Child = null; return; }
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        var clear = IconButton("x", T("Clear the selection"));
        clear.Click += (_, _) => { _allPicks.Clear(); SavesAgain(); };
        s.Children.Add(clear);
        var count = new TextBlock { Text = picked.Count == 1 ? T("1 chosen") : T("{0} chosen", picked.Count), FontSize = 13, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(6, 0, 8, 0) };
        AutomationProperties.SetLiveSetting(count, AutomationLiveSetting.Polite);
        s.Children.Add(count);
        s.Children.Add(new TextBlock { Text = SaveSize(picked.Sum(x => x.Point.Stored)), FontSize = 13, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
        s.Children.Add(new Border { Width = 1, Height = 20, Background = Res("ApexCardBorderBrush"), Margin = new Thickness(12, 0, 8, 0) });
        var points = picked.Select(x => x.Point).ToList();
        var pins = points.Where(p => p.Kind == CopyKind.Launcher).ToList();
        var unpin = pins.All(p => p.Pinned);
        void Act(string icon, string text, Func<Task> run, string? brush = null)
        {
            var b = new Button { Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { Lucide.Icon(icon, 16, Res(brush ?? "ApexSoftTextBrush")), new TextBlock { Text = text, FontSize = 13 } } }, Height = 36, Padding = new Thickness(12, 0, 12, 0), BorderThickness = new Thickness(0), IsEnabled = !DryRun,
                Background = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            AutomationProperties.SetName(b, text);
            b.Click += (_, _) => Safe(run());
            s.Children.Add(Dims(b));
        }
        if (pins.Count > 0) Act(unpin ? "pin-off" : "pin", unpin ? T("Unpin") : T("Pin"), () => MarkBackups(pins, p => (p.Label, !unpin)));
        Act("upload", T("Export"), () => ExportBackups(picked));
        Act("trash-2", SaveBackups.HasBin(points[0].Path) ? T("Recycle Bin") : T("Delete"), () => DeleteBackups(points), "ApexErrorBrush");
        SavesBar.Child = s;
    }

    // ---------- by itself ----------

    Task? _autoBackup;
    CancellationTokenSource? _autoStop;
    string? _autoError;
    long _nextDaily = Environment.TickCount64 + 60_000;

    // quietly, off the window's thread; one at a time
    void AutoBackup(BackupMoment moment)
    {
        if (DryRun || Maintenance || _game is null || _autoBackup is { IsCompleted: false }) return;
        _autoStop = new CancellationTokenSource();
        _autoBackup = Go(Docs, _autoStop.Token);

        async Task Go(string docs, CancellationToken ct)
        {
            try
            {
                var (made, error) = await Task.Run(() => SaveBackups.Run(docs, moment, null, ct));
                if (made.Count > 0 || error is not null) _autoError = error is null ? null : ErrorText(error);
            }
            catch (Exception e) { App.Log(e); }
            // (after this task has ended, so the row does not still say it is being made)
            DispatcherQueue.TryEnqueue(() => { if (_step == SavesScreen) { _savesSig = ""; FillSaves(); } });
        }
    }

    // each tick of the window's timer, with no game it follows; none while Play is under way
    long _closeDue; // (a game gone for a moment may be starting again)
    void AutoBackupTick(bool closed)
    {
        if (_running || _starting) { if (_running) _autoStop?.Cancel(); _closeDue = 0; return; }
        if (closed) _closeDue = Environment.TickCount64 + 10_000;
        if (_closeDue != 0 && Environment.TickCount64 >= _closeDue) { _closeDue = 0; AutoBackup(BackupMoment.Close); }
        if (Environment.TickCount64 < _nextDaily) return;
        _nextDaily = Environment.TickCount64 + 600_000;
        AutoBackup(BackupMoment.Day);
    }

    // Before the game starts, with what waits said: the backup being made, then the ones due. The player may go on without.
    async Task BackupsBeforePlay()
    {
        if (DryRun) return;
        var docs = Docs;
        var busy = _autoBackup is { IsCompleted: false };
        try { if (!busy && (await Task.Run(() => SaveBackups.Due(docs, BackupMoment.Play))).Count == 0) return; }
        catch (Exception e) { App.Log(e); return; }
        var bar = new ProgressBar { IsIndeterminate = true };
        var text = Body(T("Backing up…"));
        var d = new ContentDialog { Title = T("Backing up your saves"), Content = new StackPanel { Spacing = 14, Width = 420, Children = { text, bar } }, CloseButtonText = T("Play without waiting"), XamlRoot = Content.XamlRoot };
        var stop = new CancellationTokenSource(); // (the work may outlive this)
        var running = true;
        d.CloseButtonClick += (_, _) => { d.CloseButtonText = T("Stopping…"); stop.Cancel(); _autoStop?.Cancel(); };
        d.Closing += (_, e) => e.Cancel = running;
        var opened = new TaskCompletionSource();
        d.Opened += (_, _) => opened.TrySetResult();
        var shown = ShowDialog(d);
        // (Play without waiting: the copy ends at its next piece, and what is left is only tidying)
        var skip = new TaskCompletionSource();
        stop.Token.Register(() => skip.TrySetResult());
        try { await Task.WhenAny(Work(), Skipped()); }
        catch (Exception e) { App.Log(e); } // (the game starts either way)
        running = false;
        await Task.WhenAny(opened.Task, shown);
        d.Hide();
        try { await shown; } catch (Exception e) { App.Log(e); }

        async Task Skipped() { await skip.Task; await Task.Delay(1500); }
        async Task Work()
        {
            if (_autoBackup is { } t) await t;
            if (stop.IsCancellationRequested) return;
            var (made, error) = await Task.Run(() => SaveBackups.Run(docs, BackupMoment.Play, (s, i, n) => DispatcherQueue.TryEnqueue(() => { text.Text = s.Name; (bar.IsIndeterminate, bar.Maximum, bar.Value) = (n == 1, n, i); }), stop.Token));
            if (made.Count > 0 || error is not null) _autoError = error is null ? null : ErrorText(error);
        }
    }
}
