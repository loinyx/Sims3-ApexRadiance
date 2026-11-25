using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Adding files: each one read and shown first (what it is, where it goes, what is already there), then added with the
// crystal turning over a progress bar, then what went where and what was left out and why. Files come from Add mods, a drop
// from Explorer, or "Open with" (a second launcher started that way hands its files to this one).
public sealed partial class MainWindow
{
    List<string>? _openWhenReady;

    // (the game's own packs too: the list says how to install them)
    public static List<string> ModFilesIn(IEnumerable<string> args) =>
        args.Where(a => ModAdd.Extensions.Concat(ModAdd.Packs).Any(e => a.EndsWith(e, StringComparison.OrdinalIgnoreCase)) && File.Exists(a)).ToList();

    // from App: files given on the command line, opened once the game and the mods are read
    public void OpenWhenReady(List<string> files) { if (files.Count > 0) _openWhenReady = files; }

    // from App, when a second launcher handed files over; before this one is ready they wait for OpenPendingAsync
    public void TakeHandedFiles()
    {
        if (_started && HandedFiles() is { Count: > 0 } files) Safe(InstallFilesAsync(files));
    }

    // what second launchers left in Paths.Ops\open-*.txt, taken
    static List<string> HandedFiles()
    {
        var files = new List<string>();
        try
        {
            if (Directory.Exists(Paths.Ops))
                foreach (var f in Directory.EnumerateFiles(Paths.Ops, "open-*.txt"))
                {
                    files.AddRange(ModFilesIn(File.ReadAllLines(f)));
                    File.Delete(f);
                }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); }
        return files;
    }

    // a task started from a click or a menu: awaited here, so a failure is logged and said on the Mods bar
    async void Safe(Task t)
    {
        try { await t; }
        catch (Exception e)
        {
            App.Log(e);
            try { SetInfo(ModsInfo, InfoBarSeverity.Error, T("The mods were not added"), ErrorText(e)); } catch (Exception e2) { App.Log(e2); }
        }
    }

    async Task OpenPendingAsync()
    {
        List<string> files = [.. _openWhenReady ?? [], .. Maintenance ? [] : HandedFiles()];
        _openWhenReady = null;
        if (files.Count > 0) await InstallFilesAsync(files);
    }

    // (every way in goes through here: a failure is logged and said, never lost in a task nobody awaits; files that come while
    // others are being added wait for them, as a newer dialog would close theirs)
    readonly List<string> _addNext = [];
    bool _adding;
    async Task InstallFilesAsync(List<string> paths)
    {
        if (paths.Count == 0)
        {
            SetInfo(ModsInfo, InfoBarSeverity.Warning, T("The files were not added"), T("Files inside an archive can't be dropped here. Add the archive, or extract it first."));
            return;
        }
        _addNext.AddRange(paths);
        if (_adding) return;
        _adding = true;
        try
        {
            while (_addNext.Count > 0)
            {
                // (an install or other work under way keeps its screen: the files are shown once it ends)
                while (_busy) await Task.Delay(200);
                var now = _addNext.ToList();
                _addNext.Clear();
                await InstallFilesCoreAsync(now);
            }
        }
        catch (Exception e) { App.Log(e); _addNext.Clear(); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The files were not added"), ErrorText(e)); }
        finally { _adding = false; }
    }

    async Task InstallFilesCoreAsync(List<string> paths)
    {
        if (_game is null) { await Message(T("The files were not added"), T("Choose the game first.")); return; }
        if (_step != ModsScreen) Show(ModsScreen);
        if (!_modsLoaded) await LoadModsAsync();
        // (a read already under way, as when the launcher starts from "Open with": the files are compared with its list and
        // go to the folder it finds, never to Mods itself)
        while (_modsLoading) await Task.Delay(50);
        ModAdd? add;
        try { add = await ReadFilesAsync(paths); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The files could not be read"), ErrorText(e)); return; }
        if (add is null) return;
        try
        {
            if (!await AddPlanDialog(add, paths)) return;
            try { Engine.CheckWritable(); }
            catch (InvalidOperationException e) { SetInfo(ModsInfo, InfoBarSeverity.Warning, T("The files were not added"), ErrorText(e)); return; }
            await AddRunDialog(add);
        }
        finally { await Task.Run(add.Dispose); } // (the temporary copies taken out of archives)
    }

    // the files opened and compared with what is installed; null when the player cancels
    async Task<ModAdd?> ReadFilesAsync(List<string> paths)
    {
        var (game, docs, dir, installed) = (_game!, Docs, IsFolder(_modPlace) ? _modPlace : NewFolderParent(), _mods.ToList());
        var cts = new CancellationTokenSource();
        var name = Muted("");
        name.TextTrimming = TextTrimming.CharacterEllipsis;
        name.TextWrapping = TextWrapping.NoWrap;
        var progress = new Progress<string>(n => name.Text = n);
        var reading = Task.Run(() => ModAdd.Open(game, docs, dir, paths, installed, progress, cts.Token));
        // (a few loose files are read before a dialog could open)
        if (await Task.WhenAny(reading, Task.Delay(400)) != reading)
        {
            var bar = new ProgressBar { IsIndeterminate = true };
            AutomationProperties.SetName(bar, T("Reading the files"));
            var body = new StackPanel { Spacing = 14, Width = AddWidth(440) };
            body.Children.Add(bar);
            body.Children.Add(name);
            var d = new ContentDialog { Tag = "package", Title = T("Reading the files"), Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
            var shown = ShowDialog(d);
            await Task.WhenAny(reading, shown);
            if (!reading.IsCompleted)
            {
                // (an archive being opened ends on its own; what it left is deleted then)
                cts.Cancel();
                _ = reading.ContinueWith(t => { if (t.IsCompletedSuccessfully) t.Result.Dispose(); else App.Log(t.Exception); });
                return null;
            }
            d.Hide();
            await shown;
        }
        return await reading;
    }

    double AddWidth(double wide) => Math.Max(240, Math.Min(wide, Content.XamlRoot.Size.Width - 110));

    // ---------- the list ----------

    string AddPlaceText(AddPlace place, string folder) => place switch
    {
        AddPlace.SavedSims => T("Saved Sims"),
        AddPlace.Library => T("Library"),
        AddPlace.Worlds => T("Game worlds"),
        _ => folder.Length == 0 ? "Mods" : folder.Replace(Path.DirectorySeparatorChar.ToString(), " › "),
    };

    // what a file is, or what stands in its way
    string AddInfo(AddItem i)
    {
        if (!i.Ok) return T(i.Problem!);
        if (i.SameAs is { } same) return i.SameInList ? T("The same file as {0}", same) : i.SameInMerge ? T("Already inside the merge {0}", same) : T("Already installed as {0}", same) + (i.OtherMod?.Enabled == false ? " (" + T("Turned off") + ")" : "");
        if (i is { Doubt: true, Warning: { } doubt })
            return T(doubt) + " " + T("Tick it to add it anyway.") + (i.Also is { } also ? " " + T(also) : "") + (i.OtherMod is { } there ? " " + T("Another version is installed: {0}", ModRow.Place(there)) : "");
        if (i.Place == AddPlace.Worlds && i.Other is not null) return T("Another world has this name. Your saves keep using that one.");
        if (i.OtherMod is { } o) return T("Another version is installed: {0}", ModRow.Place(o)) + (o.Enabled ? "" : " (" + T("Turned off") + ")");
        if (i.Other is not null) return T("Another file with this name is there");
        if (i.OtherMerge is { } merge) return T("Another version is inside the merge {0}", merge);
        if (i.Warning is { } w) return T(w);
        var kind = i.Place switch
        {
            AddPlace.SavedSims => T("Sim"),
            AddPlace.Library => i.Household ? T("Household") : T("Lot"),
            AddPlace.Worlds => T("World"),
            _ => ModRow.KindName(i.Kind),
        };
        return kind + " · " + (i.Place == AddPlace.Worlds && ModAdd.WorldsNeedAdmin(_game!) ? T("Windows asks for permission") : ModRow.SizeText(i.Size));
    }

    FrameworkElement AddRow(AddItem i, ModAdd add, bool narrow, Action changed)
    {
        var info = AddInfo(i);
        var quiet = i is { SameAs: null, Other: null, OtherMerge: null, Warning: null };
        var g = new Grid { ColumnSpacing = 12, RowSpacing = 6, Padding = new Thickness(12, 8, 12, 8), MinHeight = 56 };
        foreach (var w in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        g.RowDefinitions.Add(new RowDefinition());
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        var check = new CheckBox { MinWidth = 0, IsChecked = i.Add, VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetName(check, $"{i.Name}. {info}. {AddPlaceText(i.Place, add.Folder)}");
        check.Click += (_, _) => { i.Add = check.IsChecked == true; changed(); };
        g.Children.Add(check);

        var icon = i.Place switch { AddPlace.SavedSims => "person-standing", AddPlace.Library => i.Household ? "users" : "house", AddPlace.Worlds => "globe", _ => ModRow.IconOf(i.Kind) };
        var tile = new Border { Width = 36, Height = 36, CornerRadius = new CornerRadius(8), Background = Res("ApexPillBrush"),
            VerticalAlignment = VerticalAlignment.Center, Child = Lucide.Icon(icon, 18, Res("ApexSoftTextBrush")) };
        Grid.SetColumn(tile, 1);
        g.Children.Add(tile);

        var text = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
        var name = new TextBlock { Text = i.Name, FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        ToolTipService.SetToolTip(name, i.Name);
        text.Children.Add(name);
        text.Children.Add(new TextBlock { Text = info, FontSize = 12, Foreground = Res(quiet ? "ApexMutedBrush" : "ApexWarningBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(text, 2);
        g.Children.Add(text);

        // where it goes, or the choice between the version there and this one
        FrameworkElement to;
        if (i.CanReplace)
        {
            var pick = new ComboBox { ItemsSource = new[] { T("Replace it"), T("Keep both") }, SelectedIndex = i.Replace ? 0 : 1, FontSize = 12, MinWidth = 130 };
            AutomationProperties.SetName(pick, i.Name);
            pick.SelectionChanged += (_, _) => i.Replace = pick.SelectedIndex == 0;
            to = pick;
        }
        else to = new TextBlock { Text = AddPlaceText(i.Place, add.Folder), FontSize = 12, Foreground = Res("ApexSoftTextBrush"), MaxWidth = 170, TextWrapping = TextWrapping.Wrap };
        to.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(to, narrow ? 2 : 3);
        Grid.SetRow(to, narrow ? 1 : 0);
        g.Children.Add(to);
        return new Border { Child = g, CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), BorderBrush = Res("ApexHairlineBrush"), Background = new SolidColorBrush(Pick(0x0AFFFFFF, 0x06000000)) };
    }

    static StackPanel AddLeftOut(string title, List<(string Name, string Why)> out_)
    {
        var s = new StackPanel { Spacing = 4 };
        s.Children.Add(new TextBlock { Text = title, FontSize = 13, FontWeight = FontWeights.SemiBold });
        foreach (var (name, why) in out_.Take(40))
            s.Children.Add(new TextBlock { Text = $"{name}: {why}", FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        if (out_.Count > 40) s.Children.Add(Muted(T("and {0} more", out_.Count - 40)));
        return s;
    }

    // before anything is written: every file with its tick and its place, then the ones that can't be added and why
    async Task<bool> AddPlanDialog(ModAdd add, List<string> paths)
    {
        // (the ones with a choice to make first, so each has its row; one past the last row replaces nothing unseen)
        var ok = add.Items.Where(i => i.Ok).OrderByDescending(i => i.Doubt).ThenByDescending(i => i.CanReplace).ToList();
        foreach (var i in ok.Skip(200)) i.Replace = false;
        var width = AddWidth(560);
        var d = new ContentDialog
        {
            Tag = "package", Title = ok.Count == 0 ? T("The files can't be added") : ok.Count == 1 ? T("Add {0}", ok[0].Name) : T("Add {0} files", ok.Count),
            PrimaryButtonText = ok.Count > 0 ? T("Add") : "", CloseButtonText = ok.Count > 0 ? T("Cancel") : T("Close"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
        };
        void Changed() => d.IsPrimaryButtonEnabled = ok.Any(i => i.Add);
        var list = new StackPanel { Spacing = 8, Padding = new Thickness(0, 0, 14, 0) };
        // shortcut: the first 200 have a row, the rest keep their default choice; a virtual list if players add that many often
        foreach (var i in ok.Take(200)) list.Children.Add(AddRow(i, add, width < 440, Changed));
        if (ok.Count > 200) list.Children.Add(Muted(T("and {0} more, added the same way. Files you already have are left out.", ok.Count - 200)));
        if (add.Items.Where(i => !i.Ok).Select(i => (i.Name, AddInfo(i))).ToList() is { Count: > 0 } bad)
        {
            var left = AddLeftOut(T("Left out"), bad);
            left.Margin = new Thickness(0, ok.Count > 0 ? 8 : 0, 0, 0);
            list.Children.Add(left);
        }
        var body = new StackPanel { Spacing = 16, Width = width };
        if (ok.Count > 0)
            body.Children.Add(Muted((ok.All(i => i.Archive is not null) && paths.Count == 1 ? T("From {0}.", Path.GetFileName(paths[0])) + " " : "") + T("Each file goes to the right folder.")));
        if (ok.Any(i => i.Place == AddPlace.Mods && !i.CanReplace) && !FolderLoaded(add.Folder)) body.Children.Add(Warn(T("The game does not load this folder"), null));
        if (ok.Any(i => i.Place == AddPlace.Mods) && !File.Exists(Path.Combine(Docs, "Mods", "Resource.cfg")))
            body.Children.Add(Muted(T("The game isn't set up to load mods yet. The launcher adds the Resource.cfg file it needs.")));
        body.Children.Add(new ScrollViewer { Content = list, Margin = new Thickness(0, 0, -14, 0), MaxHeight = 400 });
        d.Content = body;
        Changed();
        return await ShowDialog(d) == ContentDialogResult.Primary;
    }

    // ---------- adding ----------

    async Task AddRunDialog(ModAdd add)
    {
        var chosen = add.Items.Where(i => i.Ok && i.Add).ToList();
        var width = AddWidth(440);
        var crystal = new Crystal(110);
        crystal.SetState(HealthState.Ready, animate: false);
        crystal.HorizontalAlignment = HorizontalAlignment.Center;
        var bar = new ProgressBar { Maximum = 1 };
        AutomationProperties.SetName(bar, T("Adding"));
        var now = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = HorizontalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        var run = new StackPanel { Spacing = 16, Width = width, Padding = new Thickness(0, 8, 0, 4) };
        run.Children.Add(crystal);
        run.Children.Add(bar);
        run.Children.Add(now);
        bool running = true, stop = false;
        var d = new ContentDialog { Tag = "package", Title = T("Adding"), Content = new ScrollViewer { Content = run, MaxHeight = 460 }, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
        // (the file being copied is finished, the rest are not added)
        d.CloseButtonClick += (_, e) => { if (running) { e.Cancel = true; stop = true; d.CloseButtonText = T("Stopping…"); } };
        var shown = ShowDialog(d);
        var failed = new Dictionary<AddItem, string>();
        var clock = System.Diagnostics.Stopwatch.StartNew();
        var worlds = chosen.Where(i => i.Place == AddPlace.Worlds).ToList();
        var done = 0;
        foreach (var i in chosen.Except(worlds))
        {
            if (stop) break;
            now.Text = i.Name;
            bar.Value = (double)done++ / chosen.Count;
            try { await ModWork(() => { add.Add(i); return 0; }); }
            catch (Exception e) { App.Log(e); failed[i] = ErrorText(e); }
        }
        if (worlds.Count > 0 && !stop)
        {
            bar.Value = (double)done / chosen.Count;
            try { await AddWorldsAsync(add, worlds, now); }
            catch (Exception e) { App.Log(e); foreach (var w in worlds.Where(w => w.Added is null)) failed[w] = ErrorText(e); }
        }
        bar.Value = 1;
        // (a short run still shows the crystal turn once)
        if (clock.ElapsedMilliseconds < 900) await Task.Delay(900 - (int)clock.ElapsedMilliseconds);
        running = false;

        var added = chosen.Where(i => i.Added is not null).ToList();
        var replaced = chosen.Count(i => i.Replaced is not null);
        var title = added.Count == 1 ? T("1 file added") : T("{0} files added", added.Count);
        run.Children.Clear();
        crystal = new Crystal(80);
        crystal.SetState(added.Count == chosen.Count ? HealthState.Ready : HealthState.NeedsYou, animate: false);
        crystal.HorizontalAlignment = HorizontalAlignment.Center;
        run.Children.Add(crystal);
        var head = new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center };
        AutomationProperties.SetLiveSetting(head, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        run.Children.Add(head);
        foreach (var place in added.GroupBy(i => Path.GetDirectoryName(i.Added!)!, StringComparer.OrdinalIgnoreCase))
        {
            var mods = Path.Combine(Docs, "Mods");
            var i = place.First();
            run.Children.Add(AddedPlace(AddPlaceText(i.Place, i.Place == AddPlace.Mods && Path.GetRelativePath(mods, place.Key) is var r and not "." ? r : ""),
                place.Select(x => Path.GetFileName(x.Added!)).ToList(), place.Key));
        }
        if (replaced > 0) run.Children.Add(Muted(replaced == 1 ? T("The file it replaced is in the Recycle Bin.") : T("The {0} files replaced are in the Recycle Bin.", replaced)));
        var left = add.Items.Where(i => i.Added is null)
            .Select(i => (i.Name, !i.Ok || !i.Add && i.SameAs is not null ? AddInfo(i) : failed.TryGetValue(i, out var why) ? why : i.Add ? T("Cancelled") : T("Not selected"))).ToList();
        if (left.Count > 0) run.Children.Add(AddLeftOut(T("Left out"), left));
        d.Title = T("Done");
        d.CloseButtonText = T("Close");
        await shown;

        // (a replaced file comes back from the Recycle Bin, and a world only with an administrator: no Undo that does half)
        var undo = added.Count == 0 || replaced > 0 || added.Any(i => i.Place == AddPlace.Worlds) ? (Action?)null : () => Safe(UndoAddAsync(added));
        if (added.Count > 0) ModsResult(title, undo, failed.Count == 0 ? "" : T("{0} left out", failed.Count), failed.Count == 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning);
        else SetInfo(ModsInfo, InfoBarSeverity.Warning, T("The files were not added"), failed.Values.FirstOrDefault() ?? "");
        await ModsAgain();
    }

    // one place in the result: its files, and its folder in Explorer
    static Grid AddedPlace(string place, List<string> names, string dir)
    {
        var g = new Grid { ColumnSpacing = 10 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(Lucide.Icon("circle-check", 16, Res("ApexSuccessBrush")));
        var text = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = place, FontSize = 13, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        text.Children.Add(new TextBlock { Text = string.Join(", ", names.Take(5)) + (names.Count > 5 ? " " + T("and {0} more", names.Count - 5) : ""), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        var open = IconButton("folder-open", T("Open folder"));
        open.Click += (_, _) => OpenFolder(dir);
        AutomationProperties.SetHelpText(open, place);
        Grid.SetColumn(open, 2);
        g.Children.Add(open);
        return g;
    }

    async Task UndoAddAsync(List<AddItem> added)
    {
        try { await ModWork(() => { ModAdd.Undo(added); return 0; }); ModsResult(added.Count == 1 ? T("The file you added is in the Recycle Bin") : T("The files you added are in the Recycle Bin"), null); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some files were not removed"), ErrorText(e)); }
        await ModsAgain();
    }

    // ---------- worlds ----------

    const string WorldsPlan = "add-worlds";

    // into the game's folder: here when this copy may write there, else in a copy run as administrator, without a window
    async Task AddWorldsAsync(ModAdd add, List<AddItem> worlds, TextBlock now)
    {
        var game = _game!;
        var plan = add.WorldPlan(worlds);
        var admin = ModAdd.WorldsNeedAdmin(game);
        now.Text = admin ? T("Windows asks for permission") : worlds[0].Name;
        try
        {
            await ModWork(() =>
            {
                if (!admin) { ModAdd.AddWorlds(game, plan); return 0; }
                var file = Elevation.SaveChoices(plan);
                try
                {
                    using var p = Elevation.StartMaintenance(Environment.ProcessPath ?? "", game.Bin, WorldsPlan, file) ?? throw new SetupException("Windows did not get your permission. Try again and choose Yes when it asks.");
                    p.WaitForExit();
                }
                finally { File.Delete(file); } // (still there when Windows was told no)
                return 0;
            });
        }
        finally { await Task.Run(() => add.WorldsDone(worlds, plan)); }
        // (the administrator copy writes why in its log)
        if (worlds.Any(w => w.Added is null)) throw new SetupException("The world could not be added. See the log for details.");
    }

    // the administrator copy: the plan is checked again there, as any program could have changed it
    public static int? WorldsElevated(string? bin, string? screen, IReadOnlyList<string> args)
    {
        if (screen != WorldsPlan || bin is null || !Elevation.IsElevated) return null;
        try
        {
            if (Games.FromFolder(bin) is not { } g) return 1;
            ModAdd.AddWorlds(g, Elevation.Choices(args));
            return 0;
        }
        catch (Exception e) { App.Log(e); return 1; }
    }
}
