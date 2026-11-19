using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// The details pane for several mods at once and for an item the game's own launcher installed, in the same layout as
// one mod's: a strip with the title over its foot, the body, and a fixed footer with the main action.
public sealed partial class MainWindow
{
    // several picked: their tiles stacked in the strip, the list with a way to drop one, what can be done to all of them
    void FillManyDetails(List<ModRow> picked)
    {
        var mine = picked.Where(Mine).ToList();
        var list = _modsGrid ? (ListViewBase)ModGrid : ModList;
        var stack = new Grid { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 0, 30) };
        var shown = picked.Take(3).ToList();
        for (var i = shown.Count - 1; i >= 0; i--)
        {
            var glyph = Lucide.Icon(shown[i].KindIcon, 28, shown[i].GlyphBrush, 1.6);
            glyph.HorizontalAlignment = HorizontalAlignment.Center;
            glyph.VerticalAlignment = VerticalAlignment.Center;
            // (the tile's own wash on an opaque base, so the tiles behind don't show through)
            stack.Children.Add(new Border
            {
                Width = 72, Height = 72, CornerRadius = new CornerRadius(14), Background = Res("ApexPillBrush"),
                BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1),
                Child = new Border { Background = shown[i].TileBrush, CornerRadius = new CornerRadius(13), Child = glyph },
                Translation = new System.Numerics.Vector3(i * 22 - (shown.Count - 1) * 11, -i * 8, 0),
            });
        }
        var kinds = picked.Select(r => r.Kind).Distinct().ToList();
        var folders = picked.Select(r => ModRow.Place(r.File)).Distinct().ToList();
        var on = mine.Count(r => r.File.Enabled);
        var sub = string.Join(" · ", new[]
        {
            kinds.Count == 1 ? kinds[0] : null,
            ModRow.SizeText(picked.Sum(r => r.File.Size)),
            mine.Count == 0 ? null : on == mine.Count ? T("all on") : on == 0 ? T("all off") : T("{0} on", on),
            folders.Count == 1 ? folders[0] : null,
        }.OfType<string>());
        ModDetailsHead.Children.Add(PlainStrip(null, stack, [], T("{0} mods selected", picked.Count), sub, T("Clear the selection"), () => list.SelectedItems.Clear()));

        var host = ModDetails;
        // the chosen mods, each with a way out of the selection
        var rows = new StackPanel();
        var take = picked.Count > 8 ? 7 : picked.Count;
        foreach (var r in picked.Take(take))
        {
            var row = new Grid { ColumnSpacing = 10, Padding = new Thickness(12, 6, 6, 6), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, rows.Children.Count == 0 ? 0 : 1, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var tile = Lucide.Icon(r.KindIcon, 13, r.GlyphBrush);
            tile.HorizontalAlignment = HorizontalAlignment.Center;
            tile.VerticalAlignment = VerticalAlignment.Center;
            row.Children.Add(new Border { Width = 26, Height = 26, CornerRadius = new CornerRadius(6), Background = r.TileBrush, Child = tile });
            var name = new TextBlock { Text = r.Name, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            ToolTipService.SetToolTip(name, r.File.Name);
            Grid.SetColumn(name, 1);
            row.Children.Add(name);
            var size = new TextBlock { Text = r.Size, FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(size, 2);
            row.Children.Add(size);
            var x = IconButton("x", T("Take {0} out of the selection", r.Name));
            x.Width = x.Height = 28;
            var it = r;
            x.Click += (_, _) => list.SelectedItems.Remove(it);
            Grid.SetColumn(x, 3);
            row.Children.Add(x);
            rows.Children.Add(row);
        }
        if (picked.Count > take)
            rows.Children.Add(new TextBlock { Text = T("and {0} more", picked.Count - take), FontSize = 12, Foreground = Res("ApexMutedBrush"), Padding = new Thickness(12, 10, 12, 10) });
        host.Children.Add(new Border { Child = rows, CornerRadius = new CornerRadius(12), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) });
        if (mine.Count < picked.Count)
            host.Children.Add(Note("lock", "ApexMutedBrush", T("{0} of them can't be changed here", picked.Count - mine.Count)));

        // what they share: findings that name more than one of them
        foreach (var f in picked.SelectMany(r => r.Findings).GroupBy(f => f).Where(g => g.Count() > 1).Select(g => g.Key))
            host.Children.Add(Warn(f, null, T("See in Health"), () => Show(HealthScreen)));
        // Sims3Packs of lots, worlds and store items, installed or removed through the game in one go
        var packs = picked.Where(r => r.File.FileType == "sims3pack").Select(r => (Row: r, Pack: Sims3Pack.Read(r.File.Path))).Where(x => x.Pack is { Extractable: false, PackageId.Length: 34 }).ToList();
        var toInstall = packs.Where(x => !PackInstalled(x.Row.File)).ToList();
        var toRemove = packs.Where(x => PackInstalled(x.Row.File)).ToList();
        if (toInstall.Count + toRemove.Count > 0 && _game is not null)
        {
            var game = Section(T("In the game"), null, out _);
            if (toInstall.Count > 0) game.Children.Add(ActionRow("package", T("Install {0} in the game", toInstall.Count), null,
                () => Safe(GameContentAsync(GameContent.Mode.Install, toInstall.Select(x => x.Row.File.Path).ToList(), toInstall.Count == 1 ? toInstall[0].Row.Name : T("{0} Sims3Packs", toInstall.Count)))));
            if (toRemove.Count > 0) game.Children.Add(ActionRow("trash-2", T("Remove {0} from the game", toRemove.Count), null,
                () => Safe(GameContentAsync(GameContent.Mode.Remove, toRemove.Select(x => x.Pack!.PackageId).ToList(), toRemove.Count == 1 ? toRemove[0].Row.Name : T("{0} Sims3Packs", toRemove.Count)))));
            host.Children.Add(game);
        }
        if (mine.Count == 0) return;

        var tags = Section(T("Tags"), null, out _);
        tags.Children.Add(TagsRow(mine));
        host.Children.Add(tags);
        var all = Section(mine.Count == picked.Count ? T("For all {0}", mine.Count) : T("For the {0} that can be changed", mine.Count), null, out _);
        if (mine.Count > 1) all.Children.Add(ActionRow("merge", T("Merge into one package"), T("faster loading"), () => Safe(MergeAsync(mine, null, null))));
        Button? moveRow = null;
        moveRow = ActionRow("folder-input", T("Move to a folder"), null, () => MoveMenu(mine).ShowAt(moveRow!));
        all.Children.Add(moveRow);
        host.Children.Add(all);

        var foot = ModDetailsFoot;
        foot.Visibility = Visibility.Visible;
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var allOn = on == mine.Count;
        foot.Children.Add(MainButton(allOn ? T("Turn all {0} off", mine.Count) : T("Turn all {0} on", mine.Count), () => SetMods(mine, !allOn)));
        var remove = IconButton("trash-2", T("Remove all {0} to the Recycle Bin", mine.Count), "ApexErrorBrush");
        remove.Width = remove.Height = 36;
        remove.BorderThickness = new Thickness(1);
        remove.BorderBrush = Res("ApexCardBorderBrush");
        remove.Click += (_, _) => RemoveMods(mine);
        Grid.SetColumn(remove, 1);
        foot.Children.Add(remove);
    }

    // a full-width row: icon, what it does, a short hint on the right
    static Button ActionRow(string icon, string text, string? hint, Action click)
    {
        var g = new Grid { ColumnSpacing = 12 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var i = Lucide.Icon(icon, 16, Res("ApexAccentBrush"));
        i.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(i);
        var t = new TextBlock { Text = text, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        if (hint is not null)
        {
            var h = new TextBlock { Text = hint, FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(h, 2);
            g.Children.Add(h);
        }
        var b = new Button { Content = g, Height = 44, Padding = new Thickness(14, 0, 14, 0), CornerRadius = new CornerRadius(10), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, text);
        b.Click += (_, _) => click();
        return b;
    }

    // the strip without a mod behind it: a picture or a centre piece, badges, a corner button, the title over its foot
    Grid PlainStrip(ImageSource? picture, UIElement? centre, IEnumerable<Border> badges, string title, string subtitle, string cornerTip, Action corner)
    {
        var g = new Grid { Height = 176, Background = Res("ApexPillBrush") };
        if (centre is not null) g.Children.Add(centre);
        if (picture is not null)
            g.Children.Add(new Image { Source = picture, Stretch = Stretch.Uniform, MaxWidth = 84, MaxHeight = 84, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 40, 0, 52) });
        var wrap = new WrapPanel { Spacing = 6, Margin = new Thickness(12, 12, 56, 12), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        foreach (var b in badges) wrap.Children.Add(b);
        g.Children.Add(wrap);
        var x = StripButton("x", cornerTip);
        x.Margin = new Thickness(12);
        x.HorizontalAlignment = HorizontalAlignment.Right;
        x.VerticalAlignment = VerticalAlignment.Top;
        x.Click += (_, _) => corner();
        g.Children.Add(x);
        var names = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Bottom, Padding = new Thickness(20, 36, 16, 12) };
        names.Background = new LinearGradientBrush
        {
            StartPoint = new Windows.Foundation.Point(0, 1), EndPoint = new Windows.Foundation.Point(0, 0),
            GradientStops = { new GradientStop { Color = Windows.UI.Color.FromArgb(255, 15, 16, 19), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(0, 15, 16, 19), Offset = 1 } },
        };
        var t = new TextBlock { Text = title, FontSize = 18, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        ToolTipService.SetToolTip(t, title);
        names.Children.Add(t);
        names.Children.Add(new TextBlock { Text = subtitle, FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        g.Children.Add(names);
        return g;
    }

    // an installed item: its picture from the game's cache, where it is kept, the lots and worlds in Downloads that use
    // it, and the way to remove it
    void FillInstalledDetails(InstalledRow r)
    {
        var glyph = Lucide.Icon(r.KindIcon, 44, Res("ApexMutedBrush"), 1.4);
        glyph.HorizontalAlignment = HorizontalAlignment.Center;
        glyph.VerticalAlignment = VerticalAlignment.Center;
        glyph.Margin = new Thickness(0, 0, 0, 30);
        ModDetailsHead.Children.Add(PlainStrip(r.Image, r.Image is null ? glyph : null, [StripBadge(T("From the game's launcher"), "package", "#E6E6EC")], r.Name, T("Store item"),
            T("Close"), () => { InstalledGrid.SelectedItem = null; if (_modsNarrow) CloseModDetails(); }));

        var host = ModDetails;
        var tiles = new Grid { ColumnSpacing = 8 };
        tiles.ColumnDefinitions.Add(new ColumnDefinition());
        tiles.ColumnDefinitions.Add(new ColumnDefinition());
        void Tile(int col, string label, string value)
        {
            var s = new StackPanel { Spacing = 3 };
            s.Children.Add(new TextBlock { Text = label, FontSize = 11, Foreground = Res("ApexMutedBrush") });
            var v = new TextBlock { Text = value, FontSize = 14, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
            ToolTipService.SetToolTip(v, value);
            s.Children.Add(v);
            var b = new Border { Child = s, Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) };
            Grid.SetColumn(b, col);
            tiles.Children.Add(b);
        }
        Tile(0, T("Type"), r.Kind);
        Tile(1, T("In the game's cache"), Path.GetFileName(r.Item.File));
        host.Children.Add(tiles);

        // (which pack it came from is not kept by the game: the lots and worlds waiting in Downloads that use it are)
        var users = _modRows.Where(x => x.File.FileType == "sims3pack" && r.Item.Id != 0 && Sims3Pack.Read(x.File.Path)?.StoreDependencies.Contains(r.Item.Id) == true).ToList();
        var used = Section(users.Count == 0 ? T("Used by") : T("Used by ({0})", users.Count), null, out _);
        if (users.Count == 0) used.Children.Add(new TextBlock { Text = T("No lot or world in Downloads"), FontSize = 13, Foreground = Res("ApexSoftTextBrush") });
        foreach (var u in users)
        {
            var row = new Grid { ColumnSpacing = 12, Padding = new Thickness(10), CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            var tile = Lucide.Icon(u.KindIcon, 15, u.GlyphBrush);
            tile.HorizontalAlignment = HorizontalAlignment.Center;
            tile.VerticalAlignment = VerticalAlignment.Center;
            row.Children.Add(new Border { Width = 32, Height = 32, CornerRadius = new CornerRadius(8), Background = u.TileBrush, Child = tile });
            var text = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(new TextBlock { Text = u.Name, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis });
            text.Children.Add(new TextBlock { Text = T("{0} · in Downloads", u.Kind), FontSize = 12, Foreground = Res("ApexMutedBrush") });
            Grid.SetColumn(text, 1);
            row.Children.Add(text);
            used.Children.Add(row);
        }
        host.Children.Add(used);

        // the store set it came with: the game removes a set as one, so its other items go with it
        var set = SetOf(r);
        if (set.Count > 0)
        {
            var same = Section(T("Same store set ({0})", set.Count + 1), null, out _);
            foreach (var s in set.Take(12))
            {
                var row = new Grid { ColumnSpacing = 10 };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.Children.Add(Lucide.Icon(s.Icon, 14, Res("ApexMutedBrush")));
                var t = new TextBlock { Text = s.Name, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis };
                Grid.SetColumn(t, 1);
                row.Children.Add(t);
                same.Children.Add(row);
            }
            if (set.Count > 12) same.Children.Add(new TextBlock { Text = T("and {0} more", set.Count - 12), FontSize = 12, Foreground = Res("ApexMutedBrush") });
            host.Children.Add(same);
        }
        host.Children.Add(Note("lock", "ApexMutedBrush", users.Count == 0
            ? T("The game keeps these in its own content cache, not in Mods. Removing one changes that cache the way the game does, and keeps a copy first so you can undo it.")
            : T("The game keeps these in its own content cache, not in Mods. Removing it would leave {0} without it.", string.Join(", ", users.Select(u => u.Name)))));

        var foot = ModDetailsFoot;
        foot.Visibility = Visibility.Visible;
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        var remove = MainButton(set.Count > 0 ? T("Remove the set from the game") : T("Remove from the game"), () => Safe(RemoveInstalledAsync(r, set)));
        remove.IsEnabled = r.Item.FullId is not null && _game is not null;
        foot.Children.Add(remove);
    }

    // the other installed items of an item's store set
    // (each by its catalog name when the Installed list has it, else the name the game stored; parts of the set that
    // are not catalog items, such as an object's food, are only in the cache's records)
    List<(string Name, string Icon)> SetOf(InstalledRow r)
    {
        if (r.Item.FullId is not { } id) return [];
        var rows = (_installed ?? []).Where(x => x.Item.FullId is not null).GroupBy(x => x.Item.FullId!).ToDictionary(g => g.Key, g => g.First());
        return DcCache.WithSets([id], _setLists).Skip(1).Where(_recordNames.ContainsKey)
            .Select(x => rows.TryGetValue(x, out var row) ? (row.Name, row.KindIcon) : (_recordNames[x] is { Length: > 0 } n ? n : x, "package")).ToList();
    }

    // removing an item of a store set takes the whole set, as the game does: said first, with what goes
    async Task RemoveInstalledAsync(InstalledRow r, List<(string Name, string Icon)> set)
    {
        if (set.Count > 0)
        {
            var body = new StackPanel { Spacing = 10, Width = 420 };
            body.Children.Add(new TextBlock { Text = T("{0} came in a store set with {1} other items. The game removes a set as one, so these go too:", r.Name, set.Count), TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush") });
            var names = new StackPanel { Spacing = 4 };
            foreach (var s in set) names.Children.Add(new TextBlock { Text = "• " + s.Name, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis });
            body.Children.Add(new ScrollViewer { Content = names, MaxHeight = 220 });
            body.Children.Add(new TextBlock { Text = T("You can undo it right after."), FontSize = 12, Foreground = Res("ApexMutedBrush") });
            var d = new ContentDialog { Title = T("Remove {0} items?", set.Count + 1), Content = body, PrimaryButtonText = T("Remove all {0}", set.Count + 1), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Close, XamlRoot = Content.XamlRoot };
            if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        }
        await GameContentAsync(GameContent.Mode.Remove, [r.Item.FullId!], set.Count > 0 ? T("{0} and its set", r.Name) : r.Name);
    }

    bool PackInstalled(ModFile m) => _installedIds is { } ids && Sims3Pack.Read(m.Path) is { } p && ids.Contains(GameContent.ShortId(p.PackageId));

    // removing: written straight into the game's content cache, the way the game itself does it (no game start); every
    // file it changes is kept first, so Undo puts it back. Installing: the game's own installer, which shows its screen
    // for the half minute it takes; closing it when it says Done is what saves the install.
    async Task GameContentAsync(GameContent.Mode mode, List<string> entries, string what)
    {
        if (_game is not { } g) return;
        if (DryRun) { SetInfo(ModsInfo, InfoBarSeverity.Informational, T("Dry run: nothing was changed"), ""); return; }
        if (Games.IsRunning()) { SetInfo(ModsInfo, InfoBarSeverity.Warning, T("Close The Sims 3 first"), ""); return; }
        if (mode == GameContent.Mode.Remove) { await RemoveContentAsync(entries, what); return; }
        using var cts = new CancellationTokenSource();
        var status = new TextBlock { Text = T("Starting the game's installer…"), FontSize = 12, Foreground = Res("ApexMutedBrush") };
        var body = new StackPanel { Spacing = 14, Width = 420 };
        body.Children.Add(new TextBlock
        {
            Text = T("The game installs it itself, the same way its own launcher does: its installer screen shows for about half a minute and closes when it is done."),
            TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush"),
        });
        body.Children.Add(new ProgressBar { IsIndeterminate = true });
        body.Children.Add(status);
        var d = new ContentDialog { Title = T("Installing {0}", what), Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
        var finished = false;
        d.Closing += (_, _) => { if (!finished) cts.Cancel(); };
        var progress = new Progress<GameContent.Step>(s => status.Text = s switch
        {
            GameContent.Step.Working => T("Installing…"),
            GameContent.Step.Finishing => T("Saving…"),
            _ => T("Starting the game's installer…"),
        });
        var shown = ShowDialog(d);
        GameContent.Result? res = null;
        Exception? error = null;
        try { res = await GameContent.InstallAsync(g, entries, progress, cts.Token); }
        catch (OperationCanceledException) { }
        catch (Exception e) { App.Log(e); error = e; }
        finished = true;
        d.Hide();
        await shown;
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
        if (error is not null) SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was installed"), ErrorText(error));
        else if (res is null) SetInfo(ModsInfo, InfoBarSeverity.Informational, T("Cancelled: nothing was changed"), "");
        else if (res.Failed.Count == 0) ModsResult(T("{0} installed in the game", what), null);
        else SetInfo(ModsInfo, InfoBarSeverity.Warning, T("{0} of {1} were not installed", res.Failed.Count, entries.Count),
            T("The game did not take them: one may need an expansion pack you don't have, or be damaged."));
    }

    async Task RemoveContentAsync(List<string> ids, string what)
    {
        var root = Path.Combine(Paths.Data, "content-backups");
        var backup = Path.Combine(root, DateTime.Now.ToString("yyyyMMdd-HHmmss"));
        SetInfo(ModsInfo, InfoBarSeverity.Informational, T("Removing {0}…", what), "");
        DcCache.RemoveResult? res = null;
        try { res = await Task.Run(() => DcCache.Remove(Docs, ids, backup)); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was removed"), ErrorText(e)); }
        // (the newest few backups are kept: each holds whole cache files)
        try { foreach (var old in Directory.GetDirectories(root).OrderDescending().Skip(3)) Directory.Delete(old, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
        if (res is null) return;
        if (res.Removed.Count == 0) { SetInfo(ModsInfo, InfoBarSeverity.Warning, T("Nothing was removed"), T("The game's cache no longer lists it.")); return; }
        ModsResult(res.NotFound.Count == 0 ? T("{0} removed from the game", what) : T("{0} of {1} removed from the game", res.Removed.Count, ids.Count), () => Safe(UndoRemoveAsync(backup, what)));
    }

    async Task UndoRemoveAsync(string backup, string what)
    {
        try { await Task.Run(() => DcCache.Restore(Docs, backup)); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("It could not be put back"), ErrorText(e)); return; }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
        ModsResult(T("{0} is back in the game", what), null);
    }
}
