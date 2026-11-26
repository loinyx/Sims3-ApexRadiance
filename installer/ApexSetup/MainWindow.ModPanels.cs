using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// The details pane for several mods at once, in the same layout as one mod's: a strip with the title over its foot, the body, and a fixed footer with the main action.
public sealed partial class MainWindow
{
    // several picked: their tiles stacked in the strip, the list with a way to drop one, what can be done to all of them
    void FillManyDetails(List<ModRow> picked)
    {
        var mine = picked.Where(Mine).ToList();
        var list = _modsGrid ? (ListViewBase)ModGrid : ModList;
        // (their pictures side by side in the place of the one mod's, and how many more there are)
        var stack = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X2 };
        var shown = picked.Take(picked.Count > 4 ? 3 : 4).ToList();
        foreach (var one in shown)
        {
            FrameworkElement glyph = one.Image is { } own ? new Image { Source = own, Stretch = Stretch.Uniform, Margin = new Thickness(6) } : Lucide.Icon(one.KindIcon, 24, Res("ApexMutedBrush"));
            (glyph.HorizontalAlignment, glyph.VerticalAlignment) = (HorizontalAlignment.Center, VerticalAlignment.Center);
            stack.Children.Add(new Border { Width = 60, Height = 60, CornerRadius = new CornerRadius(Design.Radius.Card), Background = Res("ApexTileBrush"), Child = glyph });
        }
        if (picked.Count > shown.Count)
            stack.Children.Add(new Border { Width = 60, Height = 60, CornerRadius = new CornerRadius(Design.Radius.Card), Background = Res("ApexTileBrush"),
                Child = new TextBlock { Text = "+" + (picked.Count - shown.Count), FontSize = 16, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexSoftTextBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } });
        var kinds = picked.Select(r => r.Kind).Distinct().ToList();
        var on = mine.Count(r => r.File.Enabled);
        var sub = string.Join(" · ", new[]
        {
            kinds.Count == 1 ? kinds[0] : null,
            ModRow.SizeText(picked.Sum(r => r.File.Size)),
            mine.Count == 0 ? null : on == mine.Count ? T("all on") : on == 0 ? T("all off") : T("{0} on", on),
        }.OfType<string>());
        var title = T("{0} mods selected", picked.Count);
        ModDetailsHead.Children.Add(HeadPanel(null, stack, null, NameText(title, title), sub, [], T("Clear the selection")));
        ModDetailsHead.Children.Add(new Border { Height = 1, Background = Res("ApexCardBorderBrush") });

        var host = ModDetails;
        // the chosen mods, each with a way out of the selection
        var rows = new StackPanel();
        var take = picked.Count > 8 ? 7 : picked.Count;
        foreach (var r in picked.Take(take))
        {
            var row = new Grid { ColumnSpacing = 10, MinHeight = 44, Padding = new Thickness(8, 6, 6, 6), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, rows.Children.Count == 0 ? 0 : 1, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            FrameworkElement tile = r.Image is { } own ? new Image { Source = own, Stretch = Stretch.Uniform } : Lucide.Icon(r.KindIcon, 14, r.GlyphBrush);
            tile.HorizontalAlignment = HorizontalAlignment.Center;
            tile.VerticalAlignment = VerticalAlignment.Center;
            row.Children.Add(new Border { Width = 32, Height = 32, CornerRadius = new CornerRadius(6), Background = r.TileBrush, Child = tile, VerticalAlignment = VerticalAlignment.Center });
            var name = new TextBlock { Text = r.Name, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            ToolTipService.SetToolTip(name, r.File.Name);
            Grid.SetColumn(name, 1);
            row.Children.Add(name);
            var size = new TextBlock { Text = r.Size, FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(size, 2);
            row.Children.Add(size);
            var x = IconButton("x", T("Take {0} out of the selection", r.Name));
            x.Width = x.Height = 28;
            x.VerticalAlignment = VerticalAlignment.Center;
            ((FrameworkElement)x.Content).Opacity = 0.7;
            var it = r;
            x.Click += (_, _) => list.SelectedItems.Remove(it);
            Grid.SetColumn(x, 3);
            row.Children.Add(x);
            rows.Children.Add(row);
        }
        if (picked.Count > take)
            rows.Children.Add(new TextBlock { Text = T("and {0} more", picked.Count - take), FontSize = 12, Foreground = Res("ApexMutedBrush"), Padding = new Thickness(12, 10, 12, 10) });
        host.Children.Add(PaneCard(rows));
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
        Button? moveRow = null;
        moveRow = ActionRow("folder-input", T("Move to…"), null, () => MoveMenu(mine).ShowAt(moveRow!));
        all.Children.Add(Actions(mine.Count > 1 ? ActionRow("merge", T("Merge into one package…"), T("faster loading"), () => Safe(MergeAsync(mine, null, null))) : null, moveRow));
        host.Children.Add(all);

        var foot = ModDetailsFoot;
        foot.Visibility = Visibility.Visible;
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var allOn = on == mine.Count;
        foot.Children.Add(MainButton(allOn ? T("Turn all {0} off", mine.Count) : T("Turn all {0} on", mine.Count), () => SetMods(mine, !allOn), accent: !allOn));
        var remove = Dims(Design.Parts.IconBoxed("trash-2", T("Move all {0} to the Recycle Bin", mine.Count), ink: Res("ApexErrorBrush")));
        remove.Click += (_, _) => RemoveMods(mine);
        Grid.SetColumn(remove, 1);
        foot.Children.Add(remove);
    }

    // rows of actions on one card, a line between them
    static Border Actions(params Button?[] rows)
    {
        var s = new StackPanel();
        foreach (var b in rows.OfType<Button>())
        {
            if (s.Children.Count > 0) s.Children.Add(new Border { Height = 1, Background = Res("ApexHairlineBrush") });
            s.Children.Add(b);
        }
        return PaneCard(s);
    }

    // a full-width row: icon, what it does, a short hint under it
    // (on: what it does is done already: its icon in the accent and a check at the end)
    static Button ActionRow(string icon, string text, string? hint, Action click, bool on = false)
    {
        var g = new Grid { ColumnSpacing = 12 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var i = Lucide.Icon(icon, 16, Res(on ? "ApexAccentBrush" : "ApexSoftTextBrush"));
        i.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(i);
        var t = new TextBlock { Text = text, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        if (hint is not null)
        {
            // (under the text: beside it, the pane has no room for both in Portuguese or German)
            g.Children.Remove(t);
            var both = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            both.Children.Add(t);
            both.Children.Add(new TextBlock { Text = hint, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            Grid.SetColumn(both, 1);
            g.Children.Add(both);
        }
        if (on)
        {
            var check = Lucide.Icon("check", 16, Res("ApexAccentBrush"));
            check.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(check, 2);
            g.Children.Add(check);
        }
        var b = new Button { Content = g, MinHeight = 48, Padding = new Thickness(16, 6, 16, 7), CornerRadius = new CornerRadius(11), Background = Res("ApexClearBrush"), BorderThickness = new Thickness(0),
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, text);
        b.Click += (_, _) => click();
        return b;
    }
}
