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
        host.Children.Add(Note("lock", "ApexMutedBrush", users.Count == 0
            ? T("The game's own launcher installs and removes these. Apex Launcher only reads its cache, so the store content your saves use stays safe.")
            : T("The game's own launcher installs and removes these. Apex Launcher only reads its cache, so the store content your saves use stays safe. Removing it would leave {0} without it.", string.Join(", ", users.Select(u => u.Name)))));

        var foot = ModDetailsFoot;
        foot.Visibility = Visibility.Visible;
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        var exe = _game is { } g ? Path.Combine(g.Bin, "Sims3Launcher.exe") : null;
        var open = MainButton(T("Remove in the game's launcher"), () =>
        {
            if (Games.IsRunning()) { SetInfo(ModsInfo, InfoBarSeverity.Warning, T("Close The Sims 3 first"), ""); return; }
            try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(exe!) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe)! }); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The game's launcher did not open"), ErrorText(e)); }
        });
        open.IsEnabled = exe is not null && File.Exists(exe);
        foot.Children.Add(open);
    }
}
