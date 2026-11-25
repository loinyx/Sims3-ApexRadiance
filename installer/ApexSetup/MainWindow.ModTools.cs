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

    // one package to write: its file's name, the work, and where its log is once it started
    sealed record MergeJob(string File, Func<IProgress<MergeProgress>, CancellationToken, MergeResult> Run, Func<string?> Log);

    // the run, one package after the other: a step list with a bar and the time, the log as it is written, Cancel until the
    // originals start moving. The packages already done stay done when a later one fails or is stopped.
    async Task<(List<MergeResult> Done, Exception? Error)> MergeRunDialog(List<MergeJob> jobs, Func<List<MergeResult>, string>? title = null)
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
        var time = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = HorizontalAlignment.Right, TextWrapping = TextWrapping.Wrap };
        var log = new TextBox { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.NoWrap, FontFamily = new FontFamily("Consolas"), FontSize = 11, Height = 120 };
        ScrollViewer.SetVerticalScrollBarVisibility(log, ScrollBarVisibility.Auto);
        var body = new StackPanel { Spacing = 14, Width = 480 };
        body.Children.Add(bar);
        body.Children.Add(list);
        body.Children.Add(time);
        body.Children.Add(log);
        var cts = new CancellationTokenSource();
        var running = true; // (a failed run leaves the bar short of the end: Close then closes, it does not stop)
        var d = new ContentDialog { Title = T("Merging into {0}", jobs[0].File), Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
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
        var job = 0;
        Progress<MergeProgress> Follow(int mine) => new(p =>
        {
            if (mine != job) return; // (said late, by the package before)
            if (p.Step != current) { foreach (var (s, _) in steps.TakeWhile(x => x.Item1 != p.Step)) Mark(s, "done"); current = p.Step; Mark(p.Step, "now"); }
            var (_, _, det) = rows[p.Step];
            det.Text = p.Total <= 0 ? "" : p.Step == MergeStep.Move ? $"{p.Done} / {p.Total}" : $"{ModRow.SizeText(p.Done)} / {ModRow.SizeText(p.Total)}";
            var share = 1.0 / steps.Length;
            bar.Value = (job + Array.FindIndex(steps, x => x.Item1 == p.Step) * share + (p.Total > 0 ? share * p.Done / p.Total : 0)) / jobs.Count;
            time.Text = clock.Elapsed.ToString(@"mm\:ss");
        });
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(400);
        var (before, shownLog) = ("", ""); // (the logs of the packages already done)
        void ReadLog()
        {
            if (jobs[job].Log() is { } lp && File.Exists(lp))
                try { using var fs = new FileStream(lp, FileMode.Open, FileAccess.Read, FileShare.ReadWrite); using var sr = new StreamReader(fs); var all = before + sr.ReadToEnd(); if (all.Length != shownLog.Length) { log.Text = shownLog = all; log.SelectionStart = all.Length; } }
                catch (IOException) { }
        }
        timer.Tick += (_, _) => { time.Text = clock.Elapsed.ToString(@"mm\:ss"); ReadLog(); };
        timer.Start();
        var shown = ShowDialog(d);
        var done = new List<MergeResult>();
        Exception? error = null;
        try
        {
            for (; job < jobs.Count; job++)
            {
                cts.Token.ThrowIfCancellationRequested();
                if (jobs.Count > 1) d.Title = T("Merging into {0}, {1} of {2}", jobs[job].File, job + 1, jobs.Count);
                foreach (var (s, _) in steps) { Mark(s, "wait"); rows[s].Detail.Text = ""; }
                current = MergeStep.Check;
                Mark(MergeStep.Check, "now");
                var (now, progress) = (jobs[job], Follow(job));
                done.Add(await ModWork(() => now.Run(progress, cts.Token)));
                ReadLog(); // (a merge quicker than the timer still shows its whole log)
                before = shownLog.Length > 0 ? shownLog + "\r\n" : "";
            }
            job = jobs.Count - 1;
        }
        catch (Exception e) { error = e; App.Log(e); }
        running = false;
        timer.Stop();
        clock.Stop();
        if (error is not null) ReadLog();
        // (several logs: the folder they are in)
        if ((error is null ? done[^1].LogPath : jobs[job].Log() ?? done.LastOrDefault()?.LogPath) is { } path)
        {
            d.PrimaryButtonText = jobs.Count == 1 ? T("Open log") : T("Open log folder");
            d.PrimaryButtonClick += (_, e) => { e.Cancel = true; if (jobs.Count == 1) OpenLog(path); else OpenFolder(Path.GetDirectoryName(path)!); };
        }
        d.CloseButtonText = T("Close");
        if (error is null)
        {
            foreach (var (s, _) in steps) Mark(s, "done");
            bar.Value = 1;
            d.Title = title?.Invoke(done) ?? T("Merged {0} mods", done.Sum(r => r.Mods));
            time.Text = T("{0} resources · {1} · verified · {2:0.0} s", done.Sum(r => r.Resources), ModRow.SizeText(done.Sum(r => r.Bytes)), done.Sum(r => r.Elapsed.TotalSeconds));
        }
        else
        {
            Mark(current, "fail");
            d.Title = error is OperationCanceledException ? T("Merge stopped") : T("The merge did not finish");
            // (a SetupException from Run says what is out of place itself, such as originals left in the merge folder)
            time.Text = (done.Count > 0 ? T("Done before it: {0}.", string.Join(", ", done.Select(r => r.Merged.Name))) + " " : "")
                + (error is OperationCanceledException ? T(done.Count > 0 ? "The rest was not merged." : "Nothing was changed.")
                : ErrorText(error) + (error is SetupException || done.Count > 0 ? "" : " " + T("Your mods are as they were.")));
        }
        await shown;
        return (done, error);
    }

    async Task ClearCachesAfterMerge()
    {
        try { await ModWork(() => GameCaches.Clean(Docs)); ModsResult(T("Caches cleared. The game rebuilds them when it starts."), null); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
    }

    // mods a merge that is gone still keeps: said on the Mods screen, with the way back
    void OfferPutBack(List<ModMerge.Orphan> gone)
    {
        if (gone.Count == 0 || ModsInfo.IsOpen) return;
        var n = gone.Sum(o => o.Files.Count);
        SetInfo(ModsInfo, InfoBarSeverity.Warning, n == 1 ? T("1 mod is out of the game") : T("{0} mods are out of the game", n), T("The merge they were in is gone."), T("Put back"), async () =>
        {
            try { var back = await ModWork(() => gone.Sum(o => ModMerge.PutBack(Docs, o))); ModsResult(T("{0} mods put back", back), null); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Some mods were not put back"), ErrorText(e)); }
            await ModsAgain();
        });
    }

    static void OpenLog(string? path)
    {
        if (path is null || !File.Exists(path)) return;
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true }); }
        catch (Exception e) { App.Log(e); }
    }

    async Task SplitAsync(ModRow r)
    {
        // (a save makes the rows again)
        if (AskLeaveMerge(() => { if (_modRows.FirstOrDefault(x => x.File.Rel.Equals(r.File.Rel, StringComparison.OrdinalIgnoreCase) && ModMerge.IsMerged(x.File)) is { } now) Safe(SplitAsync(now)); })) return;
        var info = ModMerge.Info(r.File);
        if (!await Confirm(T("Split {0}?", r.Name), T("Its {0} original mods go back where they were, checked against the saved copies. The merged package goes to the Recycle Bin.", info?.Originals.Count ?? 0), T("Split"))) return;
        List<ModFile>? back = null;
        try { back = await ModWork(() => ModMerge.Split(Docs, r.File)); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The split did not finish"), ErrorText(e)); }
        if (back is not null) ModsResult(T("{0} mods put back", back.Count), null);
        await ModsAgain();
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
                acts.Children.Add(Button(T("Turn it off"), () => { if (ModsBusy()) return; SetMods([row], false); ModHunt.Forget(); HuntBanner(); }, accent: true));
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
