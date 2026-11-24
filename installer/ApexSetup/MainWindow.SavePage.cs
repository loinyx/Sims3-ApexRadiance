using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// One save's page: its cover with the household and Play, and four parts: the memories the game kept, what takes the room
// (and the clean), the copies on a timeline, and the faces. Nothing here reads or writes a save on the window's thread;
// what changes a save goes through SaveClean and SaveCopies, behind a dialog that shows how far it is. The parts:
// MainWindow.SaveTabs.cs.
public sealed partial class MainWindow
{
    sealed class SavePageState
    {
        public required Save Save { get; init; }
        public required string Docs { get; init; }
        public required string Look { get; init; }      // what the page was built for (SpLook): built again when it changes
        public string? Blocked { get; init; }           // why nothing may change now, in words; null: it may
        public string Tab { get; set; } = "memories";
        public CancellationTokenSource Stop { get; } = new(); // the page was left: what is still loading stops
        public bool Stored { get; init; }               // kept out of the game's sight
        public SaveInfo Info { get; set; } = null!;
        public SaveContents Contents { get; set; } = null!;
        public IReadOnlyList<SaveItem> Memories { get; set; } = [];
        public List<SaveItem>? Repeated { get; set; }   // the memories that share a picture, same ones together; null: not read yet
        public HashSet<string> Stars { get; set; } = [];
        public HashSet<string> Picked { get; } = [];
        public string Filter { get; set; } = "all";
        public CleanPlan? Plan { get; set; }            // of nothing chosen: what each part would free
        public CleanParts Marked { get; set; }
        public List<SavePoint>? Points { get; set; }
        public List<SizePoint> History { get; set; } = [];
        public int Point { get; set; }
        public ImageSource? Cover { get; set; }
        public ImageSource? Family { get; set; }
        public StackPanel Body { get; set; } = null!;
        public Grid Host { get; } = new();              // the open part
        public Border Bar { get; set; } = null!;        // under the scrolling part
        public InfoBar Note { get; set; } = null!;
        public Dictionary<string, SelectorBarItem> Tabs { get; } = [];
        public Action? Repaint { get; set; }            // the memories in sight, when many are chosen at once
        public Action? ShowRepeated { get; set; }       // the Memories' third choice, once the memories were compared
        public string Title => Info.Household.Length > 0 ? T("{0} family", Info.Household) : Save.Name;
    }

    SavePageState? _sp;
    int _spNote;

    void OpenSave(Save s, string tab = "memories")
    {
        if (_game is null) return;
        if (_step != SavesScreen) Show(SavesScreen); else HideSavePage();
        var docs = Docs;
        var p = _sp = new SavePageState
        {
            Save = s, Docs = docs, Look = SpLook(), Blocked = SpBlocked(), Stored = s.Dir.StartsWith(SaveCopies.StoredDir(docs) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase),
            Tab = tab is "space" or "faces" ? tab : tab is "time" or "timeline" ? "timeline" : "memories",
        };
        if (SavePage.RowDefinitions.Count == 0)
        {
            SavePage.RowDefinitions.Add(new RowDefinition());
            SavePage.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var esc = new KeyboardAccelerator { Key = Windows.System.VirtualKey.Escape };
            esc.Invoked += (_, e) => { if (_sp is not null && _dialog is null && !_busy) { e.Handled = true; Show(SavesScreen); } };
            SavePage.KeyboardAccelerators.Add(esc);
            SavePage.KeyboardAcceleratorPlacementMode = KeyboardAcceleratorPlacementMode.Hidden;
        }
        p.Body = new StackPanel { Spacing = 16, Padding = new Thickness(24, 20, 24, 32) };
        p.Body.Children.Add(SpRing());
        var scroll = new ScrollViewer { Content = p.Body };
        p.Bar = new Border { Visibility = Visibility.Collapsed, Margin = new Thickness(24, 8, 24, 16), Padding = new Thickness(18, 10, 12, 10), CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(1),
            Background = Res("ApexRaisedBrush"), BorderBrush = Res("ApexCardBorderBrush") };
        Grid.SetRow(p.Bar, 1);
        p.Note = new InfoBar { IsOpen = false, MaxWidth = 720, VerticalAlignment = VerticalAlignment.Bottom, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(24, 0, 24, 16) };
        foreach (var e in new UIElement[] { scroll, p.Bar, p.Note }) SavePage.Children.Add(e);
        Scroller.Visibility = Visibility.Collapsed; // (the page scrolls on its own, its foot stays in sight)
        SavePage.Visibility = Visibility.Visible;
        if (AnimationsOn) Animate(SavePage);
        _ = SpLoad(p);
    }

    void HideSavePage()
    {
        if (_sp is null) return;
        _sp.Stop.Cancel();
        _sp = null;
        SavePage.Visibility = Visibility.Collapsed;
        SavePage.Children.Clear();
        Scroller.Visibility = Visibility.Visible;
    }

    // the game started or closed, or the language or the theme changed: the page is read and built again
    void RefreshSavePage()
    {
        if (_sp is { } p && !_busy && p.Look != SpLook()) OpenSave(p.Save, p.Tab);
    }

    string SpLook() => $"{SpBlocked()}|{Lang}|{_light}|{Docs}";
    string? SpBlocked() => DryRun ? T("Test run: the launcher changes nothing.") : _running || Games.IsRunning() ? T("Close The Sims 3 to change this save. You can still look around and make copies.") : null;

    static FrameworkElement SpRing() => new ProgressRing { IsActive = true, Width = 32, Height = 32, Margin = new Thickness(0, 80, 0, 80), HorizontalAlignment = HorizontalAlignment.Center };

    // a size as this page says it: whole MB from 10 on
    string SpSize(long bytes)
    {
        var c = LangCulture();
        var mb = Math.Max(0, bytes) / (1024.0 * 1024);
        return mb >= 1024 ? (mb / 1024).ToString("0.#", c) + " GB" : mb >= 10 ? mb.ToString("0", c) + " MB" : mb >= 0.05 || bytes <= 0 ? mb.ToString("0.#", c) + " MB" : Math.Max(1, bytes / 1024).ToString(c) + " KB";
    }

    async Task SpLoad(SavePageState p)
    {
        var ct = p.Stop.Token;
        try
        {
            var s = p.Save;
            if (!Directory.Exists(s.Dir)) { Show(SavesScreen); return; }
            var (info, photo, contents, stars) = await Task.Run(() =>
            {
                HashSet<string> kept = [];
                // (stars that can't be read: the clean refuses on its own, the pictures still show)
                try { kept = SaveStars.Of(s.Name); } catch (SetupException e) { App.Log(e); }
                return (SaveInfo.Of(s), SaveInfo.Photo(s.Dir), SaveContents.Read(s.Dir), kept);
            }, ct);
            if (_sp != p) return;
            (p.Info, p.Contents, p.Stars, p.Memories) = (info, contents, stars, contents.Items(SaveKind.Memory));
            p.Family = await SaveCard.SimsOf(photo);
            p.Cover = await Task.Run(() => SpCover(p, ct), ct) is { } cover ? await SpImage(cover, 0, ct) : null;
            if (_sp != p) return;
            SpBuild(p);

            var (plan, groups, points, history) = await Task.Run(() => (SaveClean.Plan(p.Docs, s, CleanParts.None, null, contents, ct), contents.MemoryGroups(ct), SaveCopies.Timeline(p.Docs, s), SaveHealth.History(s.Name)), ct);
            if (_sp != p) return;
            (p.Plan, p.Points, p.History) = (plan, points, history);
            p.Repeated = groups.Where(g => g.Count > 1).SelectMany(g => g).ToList();
            p.Point = Math.Max(0, points.Count - 2);
            p.Tabs["timeline"].Text = SpTab(p, "timeline");
            SpFillWaiting(p);
        }
        catch (OperationCanceledException) { }
        catch (Exception e)
        {
            App.Log(e);
            if (_sp != p) return;
            p.Body.Children.Clear();
            p.Body.Children.Add(SpBack());
            p.Body.Children.Add(Warn(T("This save can't be read"), ErrorText(e), T("Try again"), () => OpenSave(p.Save, p.Tab)));
        }
    }

    // the picture behind the header, as on the Saves home: a memory of a place, else one from the middle of them, else a house
    static SaveItem? SpCover(SavePageState p, CancellationToken ct) =>
        SaveCover.Best(p.Contents, ct) ?? (p.Memories.Count > 0 ? p.Memories[p.Memories.Count / 2] : SaveCover.Lots(p.Contents, 1).Cast<SaveItem?>().FirstOrDefault());

    // a few pictures at a time, off the window's thread; null when the page was left, the picture is no longer wanted, or
    // it can't be read
    readonly SemaphoreSlim _spGate = new(4);
    async Task<ImageSource?> SpImage(SaveItem i, int side, CancellationToken ct, Func<bool>? wanted = null)
    {
        try
        {
            await _spGate.WaitAsync(ct);
            try
            {
                if (wanted?.Invoke() == false) return null;
                if (i.Kind is not (SaveKind.Memory or SaveKind.Generated)) return await SaveCard.ImageOf(await Task.Run(() => SaveContents.Png(i), ct));
                if (await Task.Run(() => SaveContents.Picture(i, side), ct) is not { } pic) return null;
                var bmp = new WriteableBitmap(pic.Width, pic.Height);
                using (var o = bmp.PixelBuffer.AsStream()) o.Write(pic.Bgra);
                bmp.Invalidate();
                return bmp;
            }
            finally { _spGate.Release(); }
        }
        catch (OperationCanceledException) { return null; }
        catch (Exception e) { App.Log(e); return null; }
    }

    // ---------- the frame ----------

    Button SpBack(bool glass = false)
    {
        var b = new Button { Content = Lucide.Icon("chevron-left", 18), Width = 44, Height = 44, CornerRadius = new CornerRadius(22), Padding = new Thickness(0), VerticalAlignment = VerticalAlignment.Center };
        if (glass) b.Background = Glass(0.4);
        AutomationProperties.SetName(b, T("Back to Saves"));
        AutomationProperties.SetAutomationId(b, "SaveBack");
        ToolTipService.SetToolTip(b, T("Back to Saves"));
        b.Click += (_, _) => Show(SavesScreen);
        b.Loaded += (_, _) => b.Focus(FocusState.Programmatic); // (a keyboard goes on from here)
        return b;
    }

    void SpBuild(SavePageState p)
    {
        p.Body.Children.Clear();
        p.Body.Children.Add(SpHeader(p));
        if (p.Blocked is { } why) p.Body.Children.Add(Warn(why, null));
        var bar = new SelectorBar { Margin = new Thickness(0, -8, 0, 0) };
        AutomationProperties.SetName(bar, T("Parts of the save"));
        foreach (var key in new[] { "memories", "space", "timeline", "faces" })
        {
            var t = Tab(SpTab(p, key), key == "memories");
            (t.Tag, t.IsSelected, p.Tabs[key]) = (key, key == p.Tab, t);
            bar.Items.Add(t);
        }
        bar.SelectionChanged += (b, _) => { if (b.SelectedItem?.Tag is string k && k != p.Tab) { p.Tab = k; SpFill(p); } };
        p.Body.Children.Add(bar);
        p.Body.Children.Add(new Border { Height = 1, Background = Res("ApexHairlineBrush"), Margin = new Thickness(0, -16, 0, 0) });
        p.Body.Children.Add(p.Host);
        SpFill(p);
    }

    string SpTab(SavePageState p, string key) => key switch
    {
        "memories" => T("Memories") + (p.Memories.Count > 0 ? "  " + p.Memories.Count.ToString("N0", LangCulture()) : ""),
        "space" => T("Space") + "  " + SpSize(p.Contents.Bytes),
        "timeline" => T("Timeline") + (p.Points?.Count(x => x.Kind != CopyKind.Now) is > 0 and var n ? "  " + n : ""),
        _ => T("Faces"),
    };

    void SpFill(SavePageState p)
    {
        p.Host.Children.Clear();
        p.Bar.Visibility = Visibility.Collapsed;
        p.Bar.Child = null;
        switch (p.Tab)
        {
            case "space": SpSpace(p); break;
            case "timeline": SpTimeline(p); break;
            case "faces": SpFaces(p); break;
            default: SpMemories(p); break;
        }
    }

    // what was still being read is there now
    void SpFillWaiting(SavePageState p)
    {
        if (p.Tab is "space" or "timeline") SpFill(p);
        else if (p.Tab == "memories") p.ShowRepeated?.Invoke();
    }

    UIElement SpHeader(SavePageState p)
    {
        var onCover = p.Cover is not null;
        Brush text = onCover ? new SolidColorBrush(Microsoft.UI.Colors.White) : Res("TextFillColorPrimaryBrush");
        Brush soft = onCover ? new SolidColorBrush(Windows.UI.Color.FromArgb(0xD8, 255, 255, 255)) : Res("ApexSoftTextBrush");
        var g = new Grid { Height = 168 };
        if (onCover)
        {
            g.Children.Add(new Image { Source = p.Cover, Stretch = Stretch.UniformToFill, VerticalAlignment = VerticalAlignment.Center });
            g.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(Windows.UI.Color.FromArgb(255, 10, 11, 12), [(0, 0xF2), (0.46, 0xC7), (1, 0x80)], across: true) });
        }
        else g.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(((SolidColorBrush)Res("ApexAccentBrush")).Color, [(0, 0), (0.55, 0), (1, 0x1C)], across: true) });
        // (over the cover the buttons are the dark theme's: the picture is dark under its shade in both)
        var row = new Grid { ColumnSpacing = 18, Padding = new Thickness(22, 0, 22, 0), RequestedTheme = onCover ? ElementTheme.Dark : ElementTheme.Default };
        foreach (var w in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) row.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        row.RowDefinitions.Add(new RowDefinition());
        row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        row.Children.Add(SpBack(onCover));
        var family = new Image { Source = p.Family, Height = 146, MaxWidth = 230, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Bottom, Visibility = p.Family is null ? Visibility.Collapsed : Visibility.Visible };
        Grid.SetColumn(family, 1);
        row.Children.Add(family);
        var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Spacing = 2 };
        var title = new TextBlock { Text = p.Title, FontFamily = new FontFamily("Segoe UI Variable Display"), FontSize = 34, FontWeight = FontWeights.SemiBold, Foreground = text, TextTrimming = TextTrimming.CharacterEllipsis };
        AutomationProperties.SetHeadingLevel(title, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level1);
        ToolTipService.SetToolTip(title, p.Title);
        words.Children.Add(title);
        // (the world being played first; a save that also has a university or a holiday names both)
        var worlds = new[] { p.Info.World }.Concat(p.Info.Worlds).Where(w => w.Length > 0).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        words.Children.Add(new TextBlock { Text = worlds.Count == 2 ? T("{0} and {1}", worlds[0], worlds[1]) : string.Join(" · ", worlds), FontSize = 14, Foreground = soft, TextTrimming = TextTrimming.CharacterEllipsis });
        words.Children.Add(new TextBlock { Text = SavedWhen(p.Info.LastPlayed) + " · " + Path.GetFileName(p.Save.Dir), FontSize = 12, Foreground = soft, TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(words, 2);
        row.Children.Add(words);

        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        Button Big(Button b, double pad) { (b.Height, b.CornerRadius, b.Padding) = (48, new CornerRadius(8), new Thickness(pad, 0, pad, 0)); acts.Children.Add(b); return b; }
        var play = Big(Button(T("Play"), () => PlayAsync(p.Save.Name, update: _settings.InstallUpdatesOnPlay), accent: true), 26);
        play.IsEnabled = !_busy && !Maintenance && !_running && !p.Stored;
        AutomationProperties.SetName(play, T("Play {0}", p.Title));
        AutomationProperties.SetAutomationId(play, "SavePlay");
        var copy = Big(Button(T("Make a copy"), () => SpCopy(p)), 18);
        copy.IsEnabled = !DryRun;
        if (onCover) copy.Background = Glass(0.4);
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
        MenuFlyoutItem Item(string t, Func<Task> run, string icon, bool changes = true, bool danger = false)
        {
            var i = MenuItem(t, () => _ = run(), icon, danger: danger);
            i.IsEnabled = !changes || p.Blocked is null;
            menu.Items.Add(i);
            return i;
        }
        if (!p.Stored)
        {
            Item(T("Duplicate save"), () => SpDuplicate(p), "copy");
            Item(T("Rename save"), () => SpRename(p), "pencil");
        }
        Item(p.Stored ? T("Put back in the game") : T("Keep out of the game"), () => SpStore(p, !p.Stored), "folder-input");
        Item(T("Open folder"), () => { OpenFolder(p.Save.Dir); return Task.CompletedTask; }, "folder", changes: false);
        menu.Items.Add(new MenuFlyoutSeparator());
        Item(T("Move to the Recycle Bin"), () => SpDelete(p), "trash-2", danger: true);
        var more = Big(new Button { Width = 48, Flyout = menu, Content = Lucide.Icon("ellipsis", 18) }, 0);
        if (onCover) more.Background = Glass(0.4);
        AutomationProperties.SetName(more, T("More options"));
        ToolTipService.SetToolTip(more, T("More options"));
        row.Children.Add(acts);
        g.Children.Add(row);
        g.SizeChanged += (_, e) =>
        {
            // a narrow window: the buttons under the words
            var narrow = e.NewSize.Width < 780;
            g.Height = narrow ? 228 : 168;
            Grid.SetRow(acts, narrow ? 1 : 0);
            Grid.SetColumn(acts, narrow ? 0 : 3);
            Grid.SetColumnSpan(acts, narrow ? 4 : 1);
            acts.Margin = new Thickness(0, narrow ? 10 : 0, 0, narrow ? 16 : 0);
        };
        return new Border { Child = g, CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexCardBrush") };
    }

    // a note at the foot of the page, gone after a while (an error stays until it is closed)
    void SpSay(InfoBarSeverity sev, string title, string text = "", string? action = null, Action? run = null)
    {
        if (_sp is not { } p) return;
        var mine = ++_spNote;
        SetInfo(p.Note, sev, title, text, action, () => { p.Note.IsOpen = false; run?.Invoke(); });
        if (sev != InfoBarSeverity.Error) After(action is null ? 7000 : 14000, () => { if (mine == _spNote) p.Note.IsOpen = false; });
    }
    void SpError(Exception e) { App.Log(e); SpSay(InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }

    // ---------- work on the save ----------

    // Work off the window's thread behind a dialog that says how far it is (say: the words, its share or -1, and whether it
    // can still be stopped). Null when it was stopped or failed: then the page says so.
    async Task<TR?> SpWork<TR>(string title, Func<Action<string, double, bool>, CancellationToken, TR> work) where TR : class
    {
        if (_busy) return null;
        var bar = new ProgressBar { IsIndeterminate = true, Maximum = 1 };
        var text = Body(T("Reading the save…"));
        AutomationProperties.SetLiveSetting(text, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        var body = new StackPanel { Spacing = 14, Width = 420, Children = { text, bar } };
        using var cts = new CancellationTokenSource();
        var (running, canStop) = (true, true);
        var d = new ContentDialog { Title = title, Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
        d.CloseButtonClick += (_, _) => { if (running && canStop && !cts.IsCancellationRequested) { cts.Cancel(); d.CloseButtonText = T("Stopping…"); } };
        d.Closing += (_, e) => e.Cancel = running; // (it closes when the work has ended)
        var opened = new TaskCompletionSource();
        d.Opened += (_, _) => opened.TrySetResult();
        var shown = ShowDialog(d);
        TR? result = null;
        Exception? error = null;
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
        running = false;
        await Task.WhenAny(opened.Task, shown);
        d.Hide();
        await shown;
        if (error is OperationCanceledException) SpSay(InfoBarSeverity.Informational, T("Stopped"), T("Nothing was changed."));
        else if (error is not null) SpError(error);
        return result;
    }

    // the Gallery's own file was changed for it (its undo steps hold the old one)
    void SpGalleryAgain(string? from = null, string? to = null)
    {
        if (_gal is not null) _gal = Gallery.Load();
        if (from is not null && to is not null && _galAlbum == from) _galAlbum = to;
        _lastKept = null;
        _undoSteps.Clear();
        _redoSteps.Clear();
    }

    async Task SpCopy(SavePageState p)
    {
        if (await SpWork(T("Making a copy"), (say, _) => { say(T("Copying the save and checking the copy"), -1, false); return Saves.Backup(p.Save, p.Docs); }) is null) return;
        try
        {
            var points = await Task.Run(() => SaveCopies.Timeline(p.Docs, p.Save));
            if (_sp != p) return;
            (p.Points, p.Point) = (points, Math.Max(0, points.Count - 2));
            p.Tabs["timeline"].Text = SpTab(p, "timeline");
            if (p.Tab == "timeline") SpFill(p);
        }
        catch (Exception e) { App.Log(e); } // (the copy is made; the timeline reads it the next time)
        SpSay(InfoBarSeverity.Success, T("Copy made. It shows on the timeline."), "", T("Open folder"), () => OpenFolder(Saves.BackupDir(p.Docs)));
    }

    async Task SpDuplicate(SavePageState p)
    {
        if (await SpWork(T("Duplicating the save"), (say, ct) => { say(T("Copying the save and checking the copy"), -1, true); return SaveCopies.Duplicate(p.Docs, p.Save, null, ct); }) is not { } made) return;
        SpSay(InfoBarSeverity.Success, T("The copy is called {0}", made.Name), "", T("Open"), () => OpenSave(made));
    }

    async Task SpRename(SavePageState p)
    {
        if (await AskName(T("Rename save"), p.Save.Name, T("Rename")) is { } name) await SpRenameTo(p, name, undo: true);
    }

    async Task SpRenameTo(SavePageState p, string name, bool undo)
    {
        var old = p.Save.Name;
        if (await SpWork(T("Rename save"), (say, _) => { say(T("Renaming the save and its copies"), -1, false); return SaveCopies.Rename(p.Docs, p.Save, name); }) is not { } s || s.Name == old) return;
        SpGalleryAgain(old, s.Name);
        OpenSave(s, p.Tab);
        SpSay(InfoBarSeverity.Success, T("Renamed to {0}", s.Name), "", undo ? T("Undo") : null, () => { if (_sp is { } now) _ = SpRenameTo(now, old, undo: false); });
    }

    async Task SpStore(SavePageState p, bool store)
    {
        if (await SpWork(store ? T("Keep out of the game") : T("Put back in the game"), (say, _) => { say(T("Moving the save"), -1, false); return store ? SaveCopies.Store(p.Docs, p.Save) : SaveCopies.Unstore(p.Docs, p.Save); }) is not { } s) return;
        if (s.Name != p.Save.Name) SpGalleryAgain(p.Save.Name, s.Name);
        OpenSave(s, p.Tab);
        SpSay(InfoBarSeverity.Success, store ? T("The game no longer sees this save. It stays here.") : T("The save is back in the game."), "", store ? T("Undo") : null, () => { if (_sp is { } now) _ = SpStore(now, false); });
    }

    async Task SpDelete(SavePageState p)
    {
        if (!await Confirm(T("Move this save to the Recycle Bin?"), T("{0} leaves the game. You can bring it back from the Recycle Bin, and its copies stay where they are.", p.Title), T("Move to the Recycle Bin"), danger: true)) return;
        if (await SpWork(T("Move to the Recycle Bin"), (say, _) => { say(T("Moving the save"), -1, false); SaveCopies.Delete(p.Save); return p.Save; }) is not null) Show(SavesScreen);
    }
}
