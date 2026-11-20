using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Adding mods: every file checked first and shown (what goes where, what is not added and why), then installed with the
// crystal turning over a progress bar, then what happened. Files come from Add mods, a drop from Explorer, or "Open with"
// on a .package or zip (a second launcher started that way hands its files to this one).
public sealed partial class MainWindow
{
    static readonly string[] ModExtensions = [".package", ".zip"];
    List<string>? _openWhenReady;

    public static List<string> ModFilesIn(IEnumerable<string> args) =>
        args.Where(a => ModExtensions.Any(e => a.EndsWith(e, StringComparison.OrdinalIgnoreCase)) && File.Exists(a)).ToList();

    // from App: files given on the command line, opened once the game and the mods are read
    public void OpenWhenReady(List<string> files) { if (files.Count > 0) _openWhenReady = files; }

    // from App, when a second launcher handed files over (Paths.Ops\open-*.txt)
    public void TakeHandedFiles()
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
        if (files.Count > 0) Safe(InstallFilesAsync(files));
    }

    // a task started from a click or a menu: awaited here, so a failure is logged and said on the Mods bar
    async void Safe(Task t)
    {
        try { await t; }
        catch (Exception e)
        {
            App.Log(e);
            try { SetInfo(ModsInfo, InfoBarSeverity.Error, T("Something went wrong"), ErrorText(e)); } catch (Exception e2) { App.Log(e2); }
        }
    }

    async Task OpenPendingAsync()
    {
        if (_openWhenReady is not { } files) return;
        _openWhenReady = null;
        await InstallFilesAsync(files);
    }

    // (every way in goes through here: a failure is logged and said, never lost in a task nobody awaits)
    async Task InstallFilesAsync(List<string> paths)
    {
        try { await InstallFilesCoreAsync(paths); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The files were not added"), ErrorText(e)); }
    }

    async Task InstallFilesCoreAsync(List<string> paths)
    {
        if (_game is null || paths.Count == 0) return;
        if (_step != ModsScreen) Show(ModsScreen);
        if (!_modsLoaded) await LoadModsAsync();
        var docs = Docs;
        var dir = IsFolder(_modPlace) ? _modPlace : NewFolderParent();
        var installed = _mods.ToList();
        List<ModInstall.Item> items;
        try { items = await Task.Run(() => ModInstall.Inspect(paths, installed)); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The files could not be read"), ErrorText(e)); return; }

        // 1. what will happen to each file
        var ok = items.Where(i => i.Ok).ToList();
        var dup = ok.Count(i => i.SameAs is not null);
        var list = new StackPanel { Spacing = 8 };
        foreach (var i in items)
        {
            var g = new Grid { ColumnSpacing = 10 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            var good = i.Ok && i.SameAs is null;
            g.Children.Add(Lucide.Icon(!i.Ok ? "x" : i.SameAs is not null ? "copy" : "circle-check", 16, Res(!i.Ok ? "ApexErrorBrush" : i.SameAs is not null ? "ApexWarningBrush" : "ApexSuccessBrush")));
            var text = new StackPanel { Spacing = 1 };
            text.Children.Add(new TextBlock { Text = i.Name, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis });
            var why = !i.Ok ? T(i.Problems[0])
                : i.SameAs is { } s ? T("Already installed as {0}", s)
                : i.Warnings.FirstOrDefault() is { } w ? T(w) : ModRow.SizeText(i.Size);
            text.Children.Add(new TextBlock { Text = why, FontSize = 12, Foreground = Res(!i.Ok ? "ApexErrorBrush" : good ? "ApexMutedBrush" : "ApexWarningBrush"), TextWrapping = TextWrapping.Wrap });
            Grid.SetColumn(text, 1);
            g.Children.Add(text);
            list.Children.Add(g);
        }
        var again = new CheckBox { Content = T("Add the ones already installed again (as a copy)"), Visibility = dup > 0 ? Visibility.Visible : Visibility.Collapsed };
        var body = new StackPanel { Spacing = 14, Width = 480 };
        body.Children.Add(Muted(T("Into Mods › {0}. Every file was checked: only what the game can read goes in.", PlaceTitle(dir).Title)));
        body.Children.Add(new ScrollViewer { Content = list, MaxHeight = 320 });
        body.Children.Add(again);
        var installable = ok.Count - dup;
        var d = new ContentDialog
        {
            Title = items.Count == 1 ? T("Add {0}", items[0].Name) : T("Add {0} files", items.Count), Content = body,
            PrimaryButtonText = installable > 0 || dup > 0 ? T("Install") : "", CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
        };
        if (installable == 0 && dup == 0) d.CloseButtonText = T("Close");
        if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        var withDuplicates = again.IsChecked == true;
        if (installable == 0 && !withDuplicates) return;

        // 2. installing: the crystal turns over the bar, one file at a time
        var crystal = new Crystal(110);
        crystal.SetState(HealthState.Ready, animate: false);
        crystal.HorizontalAlignment = HorizontalAlignment.Center;
        var bar = new ProgressBar { Maximum = 1, Width = 360 };
        var now = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = HorizontalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = 360 };
        var run = new StackPanel { Spacing = 16, Width = 400, Padding = new Thickness(0, 8, 0, 4) };
        run.Children.Add(crystal);
        run.Children.Add(bar);
        run.Children.Add(now);
        var pd = new ContentDialog { Title = T("Installing"), Content = run, XamlRoot = Content.XamlRoot };
        var shown = ShowDialog(pd);
        var sources = ok.Where(i => withDuplicates || i.SameAs is null).Select(i => i.Source).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        var added = new List<string>();
        var skipped = items.Where(i => !i.Ok).Select(i => $"{i.Name}: {T(i.Problems[0])}").ToList();
        var clock = System.Diagnostics.Stopwatch.StartNew();
        for (int n = 0; n < sources.Count; n++)
        {
            now.Text = Path.GetFileName(sources[n]);
            bar.Value = (double)n / sources.Count;
            try
            {
                var src = sources[n];
                var res = await Task.Run(() => ModLibrary.AddAny(docs, dir, [src], installed, withDuplicates));
                added.AddRange(res.Added);
                // (a later file of this drop with the same bytes is a duplicate of this one)
                installed.AddRange(res.Added.Select(p => new ModFile(p, Path.Combine("Mods", Path.GetRelativePath(Path.Combine(docs, "Mods"), p)), new FileInfo(p).Length, ModKind.Package, true, false)));
                skipped.AddRange(res.Skipped);
            }
            catch (Exception e) { App.Log(e); skipped.Add($"{Path.GetFileName(sources[n])}: {ErrorText(e)}"); }
        }
        bar.Value = 1;
        // (a short install still shows the crystal turn once)
        if (clock.ElapsedMilliseconds < 900) await Task.Delay(900 - (int)clock.ElapsedMilliseconds);

        // 3. what happened
        run.Children.Clear();
        crystal = new Crystal(80);
        crystal.SetState(skipped.Count == 0 ? HealthState.Ready : HealthState.NeedsYou, animate: false);
        crystal.HorizontalAlignment = HorizontalAlignment.Center;
        run.Children.Add(crystal);
        run.Children.Add(new TextBlock { Text = added.Count == 1 ? T("1 file installed") : T("{0} files installed", added.Count), FontSize = 16, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center });
        if (skipped.Count > 0)
        {
            var sk = new StackPanel { Spacing = 4 };
            sk.Children.Add(new TextBlock { Text = T("Not installed"), FontSize = 13, FontWeight = FontWeights.SemiBold });
            foreach (var s in skipped.Take(10)) sk.Children.Add(new TextBlock { Text = s, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            if (skipped.Count > 10) sk.Children.Add(Muted(T("and {0} more", skipped.Count - 10)));
            run.Children.Add(new ScrollViewer { Content = sk, MaxHeight = 200 });
        }
        pd.Title = T("Done");
        pd.CloseButtonText = T("Close");
        await shown;
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
    }
}
