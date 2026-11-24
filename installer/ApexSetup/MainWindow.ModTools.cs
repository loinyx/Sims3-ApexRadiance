using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// The Mods screen's two tools: merging packages into one (planned, shown, written, verified, with a log and a way back)
// and finding a problem mod by halves.
public sealed partial class MainWindow
{
    // ---------- merge ----------

    async Task MergeAsync(List<ModRow> rows, string? name, string? folder)
    {
        if (_game is null) return;
        var docs = Docs;
        var include = false;
        var mods = rows.Select(r => r.File).ToList();
        folder ??= rows.Select(r => DirOf(r.File)).GroupBy(d => d).OrderByDescending(g => g.Count()).First().Key;
        name ??= (Path.GetFileName(folder) is { Length: > 0 } f ? f : "Mods") + "-merged";
        MergePlan plan;
        while (true)
        {
            try { plan = await Task.Run(() => ModMerge.Plan(docs, mods, name, folder, include)); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("These mods can't be merged"), ErrorText(e)); return; }
            var answer = await MergePlanDialog(plan, name, folder, include);
            if (answer is null) return;
            // (a changed name or script choice, or a plan that could not run, is planned again)
            var changed = answer.Value.Name != name || answer.Value.Scripts != include;
            name = answer.Value.Name;
            include = answer.Value.Scripts;
            if (!changed && plan.CanRun) break;
        }
        if (!plan.CanRun) return;
        await MergeRunDialog(plan);
    }

    // the plan, before anything is written: what goes in, what is left out and why, the keys in more than one mod
    async Task<(string Name, bool Scripts)?> MergePlanDialog(MergePlan plan, string name, string folder, bool scripts)
    {
        var body = new StackPanel { Spacing = 12, MinWidth = 440, MaxWidth = 520 };
        var box = new TextBox { Header = T("Name of the merged package"), Text = name };
        body.Children.Add(box);
        body.Children.Add(Muted(T("Into {0} · {1} mods · {2} resources · {3}", PlaceTitle(folder).Title, plan.Inputs.Count, plan.Resources, ModRow.SizeText(plan.OutputBytes))));
        if (plan.Duplicates.Count > 0)
            body.Children.Add(Warn(plan.Duplicates.Count == 1 ? T("1 resource is in more than one of these mods") : T("{0} resources are in more than one of these mods", plan.Duplicates.Count),
                T("The merged package keeps the copy the game loads now. The others are listed in the log.")));
        if (plan.Shifts.Count > 0)
            body.Children.Add(Warn(T("{0} resources would change which mod the game uses", plan.Shifts.Count),
                T("A mod outside this merge has them too, and the merged package loads in a different order: {0}", string.Join(", ", plan.Shifts.Select(x => Path.GetFileNameWithoutExtension(x.Other)).Distinct().Take(3)))));
        if (plan.Skipped.Count > 0)
        {
            var sk = new StackPanel { Spacing = 4 };
            sk.Children.Add(new TextBlock { Text = T("Left out"), FontSize = 13, FontWeight = FontWeights.SemiBold });
            foreach (var s in plan.Skipped.Take(8))
                sk.Children.Add(new TextBlock { Text = $"{Path.GetFileNameWithoutExtension(s.Mod.Name)}: {T(s.Reason.Key, s.Reason.Args)}", FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            if (plan.Skipped.Count > 8) sk.Children.Add(Muted(T("and {0} more", plan.Skipped.Count - 8)));
            body.Children.Add(sk);
        }
        var scriptBox = new CheckBox { Content = T("Include script mods (their makers usually ask not to merge them)"), IsChecked = scripts };
        if (scripts || plan.Skipped.Any(s => s.Reason.Key.StartsWith("A script mod"))) body.Children.Add(scriptBox);
        foreach (var r in plan.Refusals) body.Children.Add(Warn(T(r.Key, r.Args), null));
        body.Children.Add(Muted(T("The originals are kept in Mods\\{0}. Split puts them back as they were.", ModMerge.FolderName)));
        var d = new ContentDialog
        {
            Title = plan.Inputs.Count == plan.Inputs.Count + plan.Skipped.Count ? T("Merge {0} mods", plan.Inputs.Count) : T("Merge {0} of the {1} mods chosen", plan.Inputs.Count, plan.Inputs.Count + plan.Skipped.Count), Content = new ScrollViewer { Content = body, MaxHeight = 460 },
            PrimaryButtonText = plan.CanRun ? T("Merge") : T("Check again"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
        };
        if (await ShowDialog(d) != ContentDialogResult.Primary) return null;
        var n = box.Text.Trim();
        var s2 = scriptBox.IsChecked == true;
        return (n.Length > 0 ? n : name, s2);
    }

    // the run: a step list with a bar and the time, the log as it is written, Cancel until the originals start moving
    async Task MergeRunDialog(MergePlan plan)
    {
        var steps = new[] { (MergeStep.Check, T("Check the files")), (MergeStep.Write, T("Write the merged package")), (MergeStep.Verify, T("Check every resource against the originals")),
            (MergeStep.Move, T("Keep the originals aside")), (MergeStep.Finish, T("Put the merged package in place")) };
        var rows = new Dictionary<MergeStep, (FrameworkElement Icon, TextBlock Text, TextBlock Detail)>();
        var list = new StackPanel { Spacing = 8 };
        var icons = new Dictionary<MergeStep, Grid>();
        foreach (var (step, label) in steps)
        {
            var g = new Grid { ColumnSpacing = 10 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(18) });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var iconHost = new Grid();
            iconHost.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 14, Height = 14, Stroke = Res("ApexMutedBrush"), StrokeThickness = 1.5 });
            g.Children.Add(iconHost);
            var t = new TextBlock { Text = label, FontSize = 13, Foreground = Res("ApexMutedBrush") };
            Grid.SetColumn(t, 1);
            g.Children.Add(t);
            var det = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush") };
            Grid.SetColumn(det, 2);
            g.Children.Add(det);
            list.Children.Add(g);
            rows[step] = (iconHost, t, det);
            icons[step] = iconHost;
        }
        var bar = new ProgressBar { Maximum = 1 };
        var time = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = HorizontalAlignment.Right };
        var log = new TextBox { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.NoWrap, FontFamily = new FontFamily("Consolas"), FontSize = 11, Height = 120 };
        ScrollViewer.SetVerticalScrollBarVisibility(log, ScrollBarVisibility.Auto);
        var body = new StackPanel { Spacing = 14, Width = 480 };
        body.Children.Add(bar);
        body.Children.Add(list);
        body.Children.Add(time);
        body.Children.Add(log);
        var cts = new CancellationTokenSource();
        var running = true; // (a failed run leaves the bar short of the end: Close then closes, it does not stop)
        var d = new ContentDialog { Title = T("Merging into {0}", Path.GetFileName(plan.TargetRel)), Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
        d.CloseButtonClick += (_, e) => { if (running && !cts.IsCancellationRequested) { e.Cancel = true; cts.Cancel(); d.CloseButtonText = T("Stopping…"); } };
        var clock = System.Diagnostics.Stopwatch.StartNew();
        var current = MergeStep.Check;
        void Mark(MergeStep step, string state)
        {
            var (host, text, _) = rows[step];
            var g = (Grid)host;
            g.Children.Clear();
            g.Children.Add(state switch
            {
                "done" => Lucide.Icon("circle-check", 16, Res("ApexSuccessBrush")),
                "now" => new ProgressRing { Width = 14, Height = 14, IsActive = true },
                "fail" => Lucide.Icon("x", 16, Res("ApexErrorBrush")),
                _ => new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 14, Height = 14, Stroke = Res("ApexMutedBrush"), StrokeThickness = 1.5 },
            });
            text.Foreground = Res(state == "wait" ? "ApexMutedBrush" : "ApexSoftTextBrush");
        }
        Mark(MergeStep.Check, "now");
        var progress = new Progress<MergeProgress>(p =>
        {
            if (p.Step != current) { foreach (var (s, _) in steps.TakeWhile(x => x.Item1 != p.Step)) Mark(s, "done"); current = p.Step; Mark(p.Step, "now"); }
            var (_, _, det) = rows[p.Step];
            det.Text = p.Total <= 0 ? "" : p.Step == MergeStep.Move ? $"{p.Done} / {p.Total}" : $"{ModRow.SizeText(p.Done)} / {ModRow.SizeText(p.Total)}";
            var share = 1.0 / steps.Length;
            bar.Value = Array.FindIndex(steps, x => x.Item1 == p.Step) * share + (p.Total > 0 ? share * p.Done / p.Total : 0);
            time.Text = clock.Elapsed.ToString(@"mm\:ss");
        });
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(400);
        void ReadLog()
        {
            if (plan.LogPath is { } lp && File.Exists(lp))
                try { using var fs = new FileStream(lp, FileMode.Open, FileAccess.Read, FileShare.ReadWrite); using var sr = new StreamReader(fs); var all = sr.ReadToEnd(); if (all.Length != log.Text.Length) { log.Text = all; log.SelectionStart = all.Length; } }
                catch (IOException) { }
        }
        timer.Tick += (_, _) => { time.Text = clock.Elapsed.ToString(@"mm\:ss"); ReadLog(); };
        timer.Start();
        var shown = ShowDialog(d);
        MergeResult? result = null;
        Exception? error = null;
        try { result = await ModWork(() => ModMerge.Run(plan, progress, cts.Token)); }
        catch (Exception e) { error = e; App.Log(e); }
        running = false;
        timer.Stop();
        clock.Stop();
        ReadLog(); // (a merge quicker than the timer still shows its whole log)
        if (result is not null)
        {
            foreach (var (s, _) in steps) Mark(s, "done");
            bar.Value = 1;
            d.Title = T("Merged {0} mods", result.Mods);
            time.Text = T("{0} resources · {1} · verified · {2:0.0} s", result.Resources, ModRow.SizeText(result.Bytes), result.Elapsed.TotalSeconds);
            d.CloseButtonText = T("Close");
            d.PrimaryButtonText = T("Open log");
            d.PrimaryButtonClick += (_, e) => { e.Cancel = true; OpenLog(result.LogPath); };
        }
        else
        {
            Mark(current, "fail");
            d.Title = error is OperationCanceledException ? T("Merge stopped") : T("The merge did not finish");
            // (a SetupException from Run says what is out of place itself, such as originals left in the merge folder)
            time.Text = error is OperationCanceledException ? T("Nothing was changed.") : ErrorText(error!) + (error is SetupException ? "" : " " + T("Your mods are as they were."));
            d.CloseButtonText = T("Close");
            if (plan.LogPath is { } lp) { d.PrimaryButtonText = T("Open log"); d.PrimaryButtonClick += (_, e) => { e.Cancel = true; OpenLog(lp); }; }
        }
        await shown;
        if (result is not null) ModsResult(T("{0} mods merged into {1}", result.Mods, result.Merged.Name), null);
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }

    static void OpenLog(string? path)
    {
        if (path is null || !File.Exists(path)) return;
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true }); }
        catch (Exception e) { App.Log(e); }
    }

    async Task SplitAsync(ModRow r)
    {
        var info = ModMerge.Info(r.File);
        if (!await Confirm(T("Split {0}?", r.Name), T("Its {0} original mods go back where they were, checked against the saved copies. The merged package goes to the Recycle Bin.", info?.Originals.Count ?? 0), T("Split"))) return;
        List<ModFile>? back = null;
        try { back = await ModWork(() => ModMerge.Split(Docs, r.File)); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The split did not finish"), ErrorText(e)); }
        if (back is not null) ModsResult(T("{0} mods put back", back.Count), null);
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }

    // ---------- find a problem mod ----------

    void OnModsHunt(object sender, RoutedEventArgs e) => Safe(HuntAsync());

    async Task HuntAsync()
    {
        if (ModHunt.Current() is not null) return;
        var suspects = _mods.Count(m => m.FileType == "package" && !m.Locked && m.Enabled);
        var rounds = (int)Math.Ceiling(Math.Log2(Math.Max(2, suspects)));
        if (!await Confirm(T("Find a problem mod"),
                T("Each round, the launcher turns off half of your {0} active mods. You play, then say if the problem is still there. About {1} rounds find the mod causing it. You can stop at any time, and every mod goes back as it was.", suspects, rounds)
                + "\n\n" + T("While you hunt, don't overwrite your save. It would lose objects and outfits from the mods that are off. Use Save As, or quit without saving."),
                T("Start"))) return;
        try { ModHunt.Start(Mods.List(_game!, Docs, kinds: false), Docs, _modLooks.Select(p => _mods.FirstOrDefault(m => m.Path == p)?.Rel).OfType<string>()); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The hunt did not start"), ErrorText(e)); return; }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }

    async Task HuntAnswer(bool still)
    {
        try { ModHunt.Answer(still, Mods.List(_game!, Docs, kinds: false), Docs); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }

    async Task HuntStop()
    {
        try { ModHunt.Stop(Mods.List(_game!, Docs, kinds: false), Docs); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some mods were not put back"), ErrorText(e)); }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }

    // the hunt's bar over the list: the round, a bar, the two answers and Stop; at the end, the mod it found
    void HuntBanner()
    {
        var st = ModHunt.Current();
        ModHuntBar.Visibility = st is null ? Visibility.Collapsed : Visibility.Visible;
        ModsHunt.IsEnabled = st is null;
        if (st is null) return;
        var host = ModHuntContent;
        host.Children.Clear();
        var done = st.Suspects.Count <= 1;
        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        head.Children.Add(Lucide.Icon("bug", 18, Res("ApexAccentBrush")));
        head.Children.Add(new TextBlock { Text = done ? T("Hunt finished") : T("Finding a problem mod: round {0}", st.Round), FontSize = 14, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
        host.Children.Add(head);
        if (done)
        {
            var found = ModHunt.Found(st);
            host.Children.Add(Body(found is null ? T("The answers didn't point to a single mod. The problem may come from two mods together, or from the game itself. Every mod is back as it was.")
                : T("The problem comes from {0}. Every other mod is back as it was.", Path.GetFileNameWithoutExtension(found))));
            var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            if (found is not null && _modRows.FirstOrDefault(r => r.File.Rel.Equals(found, StringComparison.OrdinalIgnoreCase)) is { } row)
            {
                acts.Children.Add(Button(T("Turn it off"), () => { SetMods([row], false); ModHunt.Forget(); HuntBanner(); }, accent: true));
                acts.Children.Add(Button(T("Show it"), () => { ModHunt.Forget(); _modPlace = AllMods; ModSearch.Text = row.Name; HuntBanner(); }));
            }
            acts.Children.Add(Button(T("Close"), () => { ModHunt.Forget(); HuntBanner(); }));
            host.Children.Add(acts);
            return;
        }
        var left = ModHunt.RoundsLeft(st);
        host.Children.Add(new ProgressBar { Maximum = st.Round + left - 1, Value = st.Round - 1 });
        host.Children.Add(Body(T("{0} of the {1} suspect mods are off this round. Play until the problem usually shows up, then come back.", st.Off.Count, st.Suspects.Count)));
        host.Children.Add(Warn(T("Don't overwrite your save while hunting"), T("It would lose objects and outfits from the mods that are off. Use Save As, or quit without saving.")));
        var a = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        // (one answer per round: the buttons stay off until the list is read again and the banner rebuilt)
        void Once(Func<Task> act) { foreach (var b in a.Children.OfType<Control>()) b.IsEnabled = false; _ = act(); }
        a.Children.Add(Button(T("The problem is still there"), () => Once(() => HuntAnswer(true)), accent: true));
        a.Children.Add(Button(T("It is gone"), () => Once(() => HuntAnswer(false))));
        a.Children.Add(Button(T("Stop and put every mod back"), () => Once(HuntStop)));
        host.Children.Add(a);
    }
}
