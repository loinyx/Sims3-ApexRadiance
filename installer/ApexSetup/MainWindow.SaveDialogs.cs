using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// The work of Saves that goes back to a backup or brings a save back, and the dialog of the pictures that stay whatever is
// cleaned.
public sealed partial class MainWindow
{
    // ---------- backups ----------

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
        if (done is not null) SavesSay(InfoBarSeverity.Success, undo ? T("{0} restored", name) : T("The save is back to how it was"), "", undo && done.Item2 is not null ? T("Undo") : null, () => _ = RestoreSave(done.Item1, done.Item2!, name, undo: false), stays: undo);
    }

    // a save that is gone; undone, it leaves again (its copy never left its place)
    async Task BringBack(string name, Func<string, CancellationToken, Save> bring)
    {
        var docs = Docs;
        var back = await SaveWork(T("Bringing the save back"), (say, ct) => { say(T("Copying the save and checking the copy"), -1, true); return bring(docs, ct); });
        if (back is not null) _saveWant = back.Dir;
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
        AutomationProperties.SetName(pick, T("Which save"));
        foreach (var l in saves) pick.Items.Add(FamilyName(l.Save, l.Info) + " · " + SavePlace(l.Save, l.Info));
        if (saves.Count > 1) body.Children.Add(pick);
        var host = new Grid { Height = 380 };
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
            if (filling || !read.TryGetValue(at, out var mine)) return;
            foreach (var p in e.AddedItems.OfType<KeepPicture>().Where(mine.Contains)) p.Kept = true;
            foreach (var p in e.RemovedItems.OfType<KeepPicture>().Where(mine.Contains)) p.Kept = false;
            Count();
        };
        all.Click += (_, _) =>
        {
            if (!read.TryGetValue(at, out var shown)) return; // (still being read)
            if (shown.All(p => p.Kept)) grid.DeselectRange(new Microsoft.UI.Xaml.Data.ItemIndexRange(0, (uint)shown.Count)); else grid.SelectAll();
        };
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
                Count();
                if (!read.ContainsKey(i))
                {
                    host.Children.Clear();
                    host.Children.Add(new ProgressRing { IsActive = true, Width = 28, Height = 28, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
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
            catch (Exception e) { App.Log(e); if (at == i) { host.Children.Clear(); host.Children.Add(Muted(ErrorText(e))); } } // (a save that is not read is not written)
        }
        pick.SelectionChanged += (_, _) => _ = Open(pick.SelectedIndex);
        Count();
        pick.SelectedIndex = 0;
        if (saves.Count == 1 && at < 0) _ = Open(0);
        var answer = await ShowDialog(d);
        stop.Cancel();
        if (answer == ContentDialogResult.None || DryRun) return;
        var chosen = read.ToList(); // (a save still being read may end meanwhile)
        try
        {
            foreach (var (i, pictures) in chosen)
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
            var items = chosen.SelectMany(r => r.Value.Where(p => p.Kept).Select(p => (saves[r.Key].Save.Name, Item: p.Items[0]))).ToList();
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
