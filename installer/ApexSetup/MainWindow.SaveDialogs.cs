using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// The two dialogs of Saves: the copies (with the saves that are gone and the ones kept out of the game), and the pictures
// that stay whatever is cleaned.
public sealed partial class MainWindow
{
    // ---------- copies ----------

    sealed record CopyLine(string Name, string Sub, string Act, DateTime When, Func<Task> Go);

    List<CopyLine> ReadCopies(string docs)
    {
        var lines = new List<CopyLine>();
        string Why(SavePoint p) => p.Kind == CopyKind.Game ? T("copy by the game") : p.Note == SaveCopies.BeforeClean ? T("before cleaning") : p.Note == SaveCopies.BeforeRestore ? T("before restoring") : T("copy by Apex");
        string Sub(params string[] parts) => string.Join(" · ", parts.Where(p => p.Length > 0));
        foreach (var s in Saves.List(docs))
        {
            var info = SaveInfo.Of(s);
            foreach (var p in SaveCopies.Timeline(docs, s).Where(p => p.Kind != CopyKind.Now))
                lines.Add(new(FamilyName(s, info), Sub(SavePlace(s, info), SaveDay(p.When, hour: true), Why(p), SaveSize(p.Bytes)), T("Restore"), p.When, () => RestoreSave(s, p, FamilyName(s, info))));
        }
        foreach (var s in SaveCopies.Lost(docs))
        {
            var info = SaveInfo.Of(s);
            lines.Add(new(FamilyName(s, info), Sub(SavePlace(s, info), T("the save is gone"), T("copy by the game"), SaveSize(info.Bytes)), T("Bring back"), s.LastPlayed, () => BringBack(FamilyName(s, info), (d, ct) => SaveCopies.Rescue(d, s, ct))));
        }
        foreach (var (s, p) in SaveCopies.Gone(docs))
            lines.Add(new(s.Name, Sub(SaveDay(p.When, hour: true), T("the save is gone"), Why(p), SaveSize(p.Bytes)), T("Bring back"), p.When, () => BringBack(s.Name, (d, ct) => SaveCopies.Restore(d, s, p, ct))));
        foreach (var s in SaveCopies.Stored(docs))
        {
            var info = SaveInfo.Of(s);
            lines.Add(new(FamilyName(s, info), Sub(SavePlace(s, info), T("out of the game"), SaveSize(info.Bytes)), T("Put back"), s.LastPlayed, () => PutBack(s)));
        }
        return lines.OrderByDescending(l => l.When).ToList();
    }

    async Task CopiesAsync()
    {
        var docs = Docs;
        var host = new Grid { MinHeight = 160 };
        host.Children.Add(new ProgressRing { IsActive = true, Width = 28, Height = 28, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
        var about = Body(T("When you restore, today's save becomes one more copy."));
        var body = new StackPanel { Spacing = 16, Width = 620, Children = { about, host } };
        var d = new ContentDialog { Title = T("Copies"), Content = body, PrimaryButtonText = T("Copy all now"), SecondaryButtonText = T("Open folder"), CloseButtonText = T("Close"), XamlRoot = Content.XamlRoot };
        d.Resources["ContentDialogMaxWidth"] = 700.0;
        d.IsPrimaryButtonEnabled = !DryRun && _savesNow?.Here.Count > 0;
        d.IsSecondaryButtonEnabled = Directory.Exists(Saves.BackupDir(docs));
        Func<Task>? go = null;
        _ = Fill();
        var answer = await ShowDialog(d);
        if (go is not null) await go();
        else if (answer == ContentDialogResult.Primary) await CopySaves(_savesNow!.Here.Select(l => l.Save).ToList());
        else if (answer == ContentDialogResult.Secondary) OpenFolder(Saves.BackupDir(docs));

        async Task Fill()
        {
            try
            {
                var lines = await Task.Run(() => ReadCopies(docs));
                host.Children.Clear();
                host.MinHeight = 0;
                if (lines.Count == 0) { about.Text = T("No copies yet. Each clean makes one by itself."); return; }
                var rows = new StackPanel();
                foreach (var l in lines)
                {
                    var row = new Grid { ColumnSpacing = 14, MinHeight = 60, Padding = new Thickness(16, 8, 12, 8), BorderThickness = new Thickness(0, rows.Children.Count > 0 ? 1 : 0, 0, 0), BorderBrush = Res("ApexHairlineBrush") };
                    row.ColumnDefinitions.Add(new ColumnDefinition());
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    var words = (StackPanel)Col(SaveText(l.Name, 14, strong: true), SaveText(l.Sub, 12, "ApexMutedBrush"));
                    (words.Spacing, words.VerticalAlignment) = (1, VerticalAlignment.Center);
                    ToolTipService.SetToolTip(words, l.Sub);
                    row.Children.Add(words);
                    var act = Dims(Button(l.Act, () => { go = l.Go; d.Hide(); }));
                    (act.Height, act.VerticalAlignment, act.IsEnabled) = (36, VerticalAlignment.Center, SavesBlocked() is null);
                    AutomationProperties.SetName(act, $"{l.Act}, {l.Name}, {l.Sub}");
                    Grid.SetColumn(act, 1);
                    row.Children.Add(act);
                    rows.Children.Add(row);
                }
                host.Children.Add(new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(0), Child = new ScrollViewer { Content = rows, MaxHeight = 380 } });
            }
            catch (Exception e) { App.Log(e); host.Children.Clear(); about.Text = ErrorText(e); }
        }
    }

    async Task RestoreSave(Save s, SavePoint to, string name, bool undo = true)
    {
        var docs = Docs;
        var done = await SaveWork(T("Restoring the save"), (say, ct) =>
        {
            say(T("Copying the save and checking the copy"), -1, true);
            var now = SaveCopies.Restore(docs, s, to, ct);
            return Tuple.Create(now, SaveCopies.Timeline(docs, now).LastOrDefault(x => x.Note == SaveCopies.BeforeRestore));
        });
        FillSaves();
        if (done is not null) SavesSay(InfoBarSeverity.Success, T("{0} restored", name), "", undo && done.Item2 is not null ? T("Undo") : null, () => _ = RestoreSave(done.Item1, done.Item2!, name, undo: false));
    }

    // a save that is gone; undone, it leaves again (its copy never left its place)
    async Task BringBack(string name, Func<string, CancellationToken, Save> bring)
    {
        var docs = Docs;
        var back = await SaveWork(T("Bringing the save back"), (say, ct) => { say(T("Copying the save and checking the copy"), -1, true); return bring(docs, ct); });
        FillSaves();
        if (back is null) return;
        SavesSay(InfoBarSeverity.Success, T("{0} is back in the game", name), "", T("Undo"), () => _ = Undo());

        async Task Undo()
        {
            await SaveWork(T("Undo"), (say, _) =>
            {
                say(T("Moving the save"), -1, false);
                if (SaveInfo.Of(back).LastPlayed != back.LastPlayed) throw new SetupException("This save was played since it came back, so it stays.");
                SaveCopies.Delete(back);
                return back;
            });
            FillSaves();
        }
    }

    // ---------- the pictures that stay ----------

    // one of a save's pictures: the copies of one memory are one
    sealed partial class KeepPicture
    {
        public required IReadOnlyList<SaveItem> Items { get; init; }
        public bool Kept { get; set; }
    }

    async Task KeepAsync(List<SaveLook> saves)
    {
        if (saves.Count == 0 || _busy || _savesBusy) return;
        var tile = (DataTemplate)XamlReader.Load("<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><Grid Width='128' Height='128' CornerRadius='6' Background='#18808080'><Image Stretch='UniformToFill' HorizontalAlignment='Center' VerticalAlignment='Center'/></Grid></DataTemplate>");
        var grid = new GridView { SelectionMode = ListViewSelectionMode.Multiple, ItemTemplate = tile, Height = 380, IsItemClickEnabled = false };
        AutomationProperties.SetName(grid, T("Pictures that stay"));
        var count = Muted("");
        count.VerticalAlignment = VerticalAlignment.Center;
        var all = Link(T("Choose all"), () => { });
        all.VerticalAlignment = VerticalAlignment.Center;
        var foot = new Grid { ColumnSpacing = 12, MinHeight = 32 };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.Children.Add(count);
        Grid.SetColumn(all, 1);
        foot.Children.Add(all);
        var body = new StackPanel { Spacing = 12, Width = 660 };
        body.Children.Add(Body(T("The ones you choose never leave in a clean.")));
        var pick = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
        AutomationProperties.SetName(pick, T("Save"));
        foreach (var l in saves) pick.Items.Add(FamilyName(l.Save, l.Info) + " · " + SavePlace(l.Save, l.Info));
        if (saves.Count > 1) body.Children.Add(pick);
        var host = new Grid { Height = 380, Children = { new ProgressRing { IsActive = true, Width = 28, Height = 28, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } } };
        body.Children.Add(host);
        body.Children.Add(foot);
        var d = new ContentDialog { Title = T("Choose the pictures that stay"), Content = body, PrimaryButtonText = T("Done"), SecondaryButtonText = T("Save to a folder"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        d.Resources["ContentDialogMaxWidth"] = 740.0;
        using var stop = new CancellationTokenSource();
        var read = new Dictionary<int, List<KeepPicture>>();
        var (at, filling) = (-1, false);

        grid.ContainerContentChanging += (s, e) =>
        {
            if (e.ItemContainer.ContentTemplateRoot is not Grid { Children: [Image img] }) return;
            if (e.InRecycleQueue) { (img.Source, img.Tag) = (null, null); return; }
            if (e.Item is not KeepPicture p || ReferenceEquals(img.Tag, p)) return;
            (img.Source, img.Tag) = (null, p);
            AutomationProperties.SetName(e.ItemContainer, T("Picture {0}", e.ItemIndex + 1));
            _ = Show(img, p);
        };
        async Task Show(Image img, KeepPicture p)
        {
            try { if (await SavePicture(p.Items[0], 224, () => ReferenceEquals(img.Tag, p), stop.Token) is { } src && ReferenceEquals(img.Tag, p)) img.Source = src; }
            catch (Exception e) when (e is not OperationCanceledException) { App.Log(e); }
            catch (OperationCanceledException) { }
        }
        grid.SelectionChanged += (_, e) =>
        {
            if (filling) return;
            foreach (var p in e.AddedItems.OfType<KeepPicture>()) p.Kept = true;
            foreach (var p in e.RemovedItems.OfType<KeepPicture>()) p.Kept = false;
            Count();
        };
        all.Click += (_, _) => { if (read[at].All(p => p.Kept)) grid.DeselectRange(new Microsoft.UI.Xaml.Data.ItemIndexRange(0, (uint)read[at].Count)); else grid.SelectAll(); };
        void Count()
        {
            var shown = read.GetValueOrDefault(at) ?? [];
            var n = shown.Count(p => p.Kept);
            count.Text = n == 1 ? T("1 of {0} stays", shown.Count) : T("{0} of {1} stay", n, shown.Count);
            foot.Visibility = shown.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            all.Content = shown.Count > 0 && n == shown.Count ? T("Choose none") : T("Choose all");
            d.IsSecondaryButtonEnabled = read.Values.Any(l => l.Any(p => p.Kept));
        }
        async Task Open(int i)
        {
            try
            {
                at = i;
                if (!read.ContainsKey(i))
                {
                    var l = saves[i];
                    var pictures = await Task.Run(() =>
                    {
                        var kept = SaveStars.Of(l.Save.Name);
                        var c = SaveContents.Read(l.Save.Dir);
                        return c.MemoryGroups(stop.Token).Concat(c.Items(SaveKind.Generated).Select(x => (IReadOnlyList<SaveItem>)[x]))
                            .Select(g => new KeepPicture { Items = g, Kept = g.Any(x => kept.Contains(x.Key)) }).ToList();
                    }, stop.Token);
                    read[i] = pictures;
                }
                if (at != i) return;
                host.Children.Clear();
                if (read[i].Count == 0) { var none = Muted(T("This save has no memories or photos.")); (none.HorizontalAlignment, none.VerticalAlignment) = (HorizontalAlignment.Center, VerticalAlignment.Center); host.Children.Add(none); }
                else
                {
                    filling = true;
                    grid.ItemsSource = read[i].Cast<object>().ToList();
                    foreach (var p in read[i].Where(p => p.Kept)) grid.SelectedItems.Add(p);
                    filling = false;
                    host.Children.Add(grid);
                }
                Count();
            }
            catch (OperationCanceledException) { }
            catch (Exception e) { App.Log(e); host.Children.Clear(); host.Children.Add(Muted(ErrorText(e))); d.IsPrimaryButtonEnabled = d.IsSecondaryButtonEnabled = false; }
        }
        pick.SelectionChanged += (_, _) => _ = Open(pick.SelectedIndex);
        Count();
        pick.SelectedIndex = 0;
        if (saves.Count == 1 && at < 0) _ = Open(0);
        var answer = await ShowDialog(d);
        stop.Cancel();
        if (answer == ContentDialogResult.None) return;
        try
        {
            foreach (var (i, pictures) in read)
            {
                var name = saves[i].Save.Name;
                await Task.Run(() =>
                {
                    var was = SaveStars.Of(name);
                    // (of a memory stored more than once, the copy kept before, else the first)
                    SaveStars.Keep(name, pictures.SelectMany(p => p.Items).Select(x => x.Key).ToList(),
                        pictures.Where(p => p.Kept).SelectMany(p => p.Items.Where(x => was.Contains(x.Key)).DefaultIfEmpty(p.Items[0])).Select(x => x.Key).ToList());
                });
                _saveReads.Remove(ReadKey(saves[i], "clean")); // (what a clean would free counts them)
            }
            _savesSig = "";
            FillSaves();
            if (answer != ContentDialogResult.Secondary) return;
            var picker = new Microsoft.Windows.Storage.Pickers.FolderPicker(AppWindow.Id);
            if (await picker.PickSingleFolderAsync() is not { } f) return;
            var items = read.SelectMany(r => r.Value.Where(p => p.Kept).Select(p => (saves[r.Key].Save.Name, Item: p.Items[0]))).ToList();
            if (await SaveWork(T("Saving the pictures"), (say, ct) =>
            {
                var made = 0;
                foreach (var ((name, item), i) in items.Select((x, i) => (x, i)))
                {
                    say(T("{0} of {1}", i + 1, items.Count), (double)i / items.Count, true);
                    made += SaveContents.Export(name, [item], f.Path, ct).Count;
                }
                return new[] { made };
            }) is not [var n]) return;
            SavesSay(InfoBarSeverity.Success, n == 1 ? T("1 picture saved.") : T("{0} pictures saved.", n), "", T("Open folder"), () => OpenFolder(f.Path));
        }
        catch (Exception e) { App.Log(e); SavesSay(InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
    }

    // a few pictures at a time, off the window's thread; null when it is no longer wanted or can't be read
    readonly SemaphoreSlim _savePictures = new(4);
    async Task<WriteableBitmap?> SavePicture(SaveItem i, int side, Func<bool> wanted, CancellationToken ct)
    {
        await _savePictures.WaitAsync(ct);
        try
        {
            if (!wanted() || await Task.Run(() => SaveContents.Picture(i, side), ct) is not { } pic) return null;
            var bmp = new WriteableBitmap(pic.Width, pic.Height);
            using (var o = bmp.PixelBuffer.AsStream()) o.Write(pic.Bgra);
            bmp.Invalidate();
            return bmp;
        }
        finally { _savePictures.Release(); }
    }
}
