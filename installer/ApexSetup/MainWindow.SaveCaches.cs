using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using static ApexSetup.Strings;

namespace ApexSetup;

// Saves' page of the game's caches: what the game makes again by itself, for every save. Each group is a row that can be
// turned off, and each row opens its files to keep some of them. GameCaches moves what goes into the launcher's data, kept
// until the game closes normally; the list is read with the saves, off the window's thread.
public sealed partial class MainWindow
{
    // the group, its heading on the page, its words, its icon and whether a clean takes it unless the player says otherwise
    static readonly (CacheGroup Group, string Section, string Name, string Text, string Icon, bool On)[] CacheRows =
    [
        (CacheGroup.CreateASim, "When something looks wrong", "Create a Sim", "When hair or clothes are missing or look wrong", "shirt", true),
        (CacheGroup.Scripts, "When something looks wrong", "Scripts", "When you changed script mods", "code-xml", true),
        (CacheGroup.Worlds, "When something looks wrong", "Worlds", "When a world takes long to load", "globe", true),
        (CacheGroup.Thumbnails, "When something looks wrong", "Thumbnails", "When Buy or Build mode shows wrong pictures", "layout-grid", false),
        (CacheGroup.Downloads, "When something looks wrong", "Downloads index", "When downloaded content or Sims don't show up", "download", true),
        (CacheGroup.Online, "When something looks wrong", "Online", "When store pictures or online features don't load", "cloud", false),
        (CacheGroup.Leftovers, "Disk space", "Unfinished content files", "Left half written when the game closed; the game never reads them", "file-down", true),
        (CacheGroup.ContentBackups, "Disk space", "Backups of installed content", "Copies the content installer keeps; the game plays without them", "copy", false),
        (CacheGroup.ErrorLogs, "Disk space", "Old error logs", "Script errors Health has already read, and the game's own logs", "list", true),
    ];

    readonly Dictionary<CacheGroup, bool> _cacheOn = new();                                // a group turned on or off here
    readonly HashSet<string> _cacheKeep = new(StringComparer.OrdinalIgnoreCase);           // the files unticked: they stay
    readonly HashSet<CacheGroup> _cacheOpen = new();                                       // the rows showing their files

    void DrawCaches(SavesNow now)
    {
        var files = now.Caches;
        bool On(CacheGroup g) => _cacheOn.TryGetValue(g, out var on) ? on : CacheRows.First(r => r.Group == g).On;
        List<CacheFile> Chosen() => files.Where(f => On(f.Group) && !_cacheKeep.Contains(f.Path)).ToList();

        var free = Id(MainButton("", () => Safe(ClearCaches(now.Docs, Chosen()))), "CachesFree");
        // (the height of the other buttons in Saves' heads)
        (free.HorizontalAlignment, free.Height, free.Padding) = (HorizontalAlignment.Right, Design.Size.Control, new Thickness(Design.Space.X4, 0, Design.Space.X4, 0));
        var page = new StackPanel { Spacing = Design.Space.X1, MaxWidth = Design.Size.Page };
        page.Children.Add(SavesSubHead(T("Game caches"), T("Files the game makes again by itself, for every save"), free));
        if (SavesBlocked() is { } why) page.Children.Add(Warn(why, null));
        var rows = new StackPanel { Spacing = Design.Space.X1, Margin = new Thickness(0, Design.Space.HeadToBody, 0, 0) };
        AutomationProperties.SetName(rows, T("Game caches"));
        var (meter, setMeter) = FreeMeter();
        rows.Children.Add(meter);
        var says = new List<Action>();
        var section = "";
        foreach (var (group, of, name, text, icon, _) in CacheRows)
        {
            var mine = files.Where(f => f.Group == group).OrderByDescending(f => f.Size).ToList();
            if (mine.Count == 0 && group is CacheGroup.Online or CacheGroup.Leftovers or CacheGroup.ContentBackups) continue; // (most players have none)
            if (of != section) rows.Children.Add(RowGroup(T(section = of)));
            var size = RowSize(mine.Count == 0 ? T("Empty") : "");
            var sw = RowSwitch(mine.Count > 0 && On(group));
            sw.IsEnabled = mine.Count > 0;
            AutomationProperties.SetName(sw, T(name) + ". " + T(text));
            sw.Toggled += (_, _) => { if (sw.IsOn != On(group)) { _cacheOn[group] = sw.IsOn; Say(); } };

            // the files, one checkbox each: one unticked stays
            HyperlinkButton? link = null;
            Border? list = null;
            if (mine.Count > 0)
            {
                var label = new TextBlock { Text = mine.Count == 1 ? T("1 file") : T("{0} files", mine.Count) };
                var chevron = (LucideIcon)Lucide.Icon(_cacheOpen.Contains(group) ? "chevron-up" : "chevron-down", Design.Size.IconSmall, Res("ApexAccentBrush"));
                chevron.VerticalAlignment = VerticalAlignment.Center;
                link = new HyperlinkButton { Padding = new Thickness(0), Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X1, Children = { label, chevron } } };
                AutomationProperties.SetName(link, label.Text + ", " + T(name));
                list = FileList(mine, Say);
                list.Visibility = _cacheOpen.Contains(group) ? Visibility.Visible : Visibility.Collapsed;
                var (l, c) = (list, chevron);
                link.Click += (_, _) =>
                {
                    var open = !_cacheOpen.Remove(group);
                    if (open) _cacheOpen.Add(group);
                    l.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
                    c.Kind = open ? "chevron-up" : "chevron-down";
                    AutomationProperties.SetItemStatus(link, open ? T("Expanded") : T("Collapsed"));
                };
                AutomationProperties.SetItemStatus(link, _cacheOpen.Contains(group) ? T("Expanded") : T("Collapsed"));
            }
            rows.Children.Add(Row(icon, T(name), T(text), RowEnd(size, sw), link));
            if (list is not null) rows.Children.Add(list);
            if (mine.Count > 0)
                says.Add(() =>
                {
                    var going = On(group) ? mine.Where(f => !_cacheKeep.Contains(f.Path)).ToList() : [];
                    size.Text = SaveSize(going.Count > 0 ? going.Sum(f => f.Size) : mine.Sum(f => f.Size));
                    size.Foreground = Res(going.Count > 0 ? "ApexSuccessBrush" : "ApexMutedBrush");
                });
        }
        page.Children.Add(rows);

        // what a clean leaves behind, and the way back
        var foot = new Grid { ColumnSpacing = Design.Space.X4, Margin = new Thickness(0, Design.Space.X3, 0, 0) };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var kept = PageSub(T("A copy is kept until the game closes normally."));
        (kept.TextWrapping, kept.VerticalAlignment) = (TextWrapping.Wrap, VerticalAlignment.Center);
        foot.Children.Add(kept);
        if (now.CachesKept)
        {
            var back = Id(Dims(Button(T("Put the caches back"), () => Safe(PutCachesBackAsync(last: false)))), "CachesBack");
            back.IsEnabled = SavesBlocked() is null;
            Grid.SetColumn(back, 1);
            foot.Children.Add(back);
        }
        page.Children.Add(foot);
        SavesHost.Children.Add(page);
        Say();

        void Say()
        {
            foreach (var s in says) s();
            var chosen = Chosen();
            var (gain, total) = (chosen.Sum(f => f.Size), files.Sum(f => f.Size));
            setMeter(chosen.Count > 0 ? T("{0} can be freed", SaveSize(gain)) : T("Nothing to free with these choices"), T("{0} in caches", SaveSize(total)), gain, total);
            free.Content = chosen.Count > 0 ? T("Free up {0}", SaveSize(gain)) : T("Nothing to clean");
            free.IsEnabled = chosen.Count > 0 && SavesBlocked() is null;
        }
    }

    // a row's files under it: a checkbox for each, with its size and day; Select all and Clear all over them
    Border FileList(List<CacheFile> files, Action changed)
    {
        var boxes = new List<(CheckBox Box, CacheFile File)>();
        var list = new StackPanel();
        foreach (var f in files)
        {
            var g = new Grid { ColumnSpacing = Design.Space.X4 };
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var file = SaveText(f.Rel, Design.TextSize.Small);
            file.VerticalAlignment = VerticalAlignment.Center;
            ToolTipService.SetToolTip(file, f.Path);
            g.Children.Add(file);
            var about = RowSize(SaveSize(f.Size) + " · " + SaveDay(f.When));
            about.FontSize = Design.TextSize.Caption;
            Grid.SetColumn(about, 1);
            g.Children.Add(about);
            // (the box's own 32 high with the words centred on it, two apart: one control height a file)
            var box = new CheckBox { Content = g, IsChecked = !_cacheKeep.Contains(f.Path), MinWidth = 0, MinHeight = 0, Padding = new Thickness(Design.Space.X2, 0, 0, 0), Margin = new Thickness(0, 2, 0, 2), VerticalContentAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            box.Resources["CheckBoxCheckBackgroundStrokeUnchecked"] = QuietBox;
            AutomationProperties.SetName(box, $"{f.Rel}, {SaveSize(f.Size)}");
            box.Click += (_, _) => { if (box.IsChecked == true) _cacheKeep.Remove(f.Path); else _cacheKeep.Add(f.Path); changed(); };
            boxes.Add((box, f));
            list.Children.Add(box);
        }
        void All(bool on)
        {
            foreach (var (box, f) in boxes) { box.IsChecked = on; if (on) _cacheKeep.Remove(f.Path); else _cacheKeep.Add(f.Path); }
            changed();
        }
        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X4, Margin = new Thickness(0, 0, 0, Design.Space.X1), Children = { Link(T("Select all"), () => All(true)), Link(T("Clear all"), () => All(false)) } };
        // (under the row's words: as far in as its icon and the gap after it)
        return Join(new Border
        {
            Padding = new Thickness(Design.Space.RowPad + Design.Size.Tile + Design.Space.IconToText, Design.Space.X2, Design.Space.RowPad, Design.Space.X3),
            Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1),
            Child = new StackPanel { Children = { acts, new ScrollViewer { Content = list, MaxHeight = Design.Size.Control * 9 } } }, // (nine files, then it scrolls)
        });
    }

    async Task ClearCaches(string docs, List<CacheFile> chosen)
    {
        if (chosen.Count == 0) return;
        try
        {
            var bytes = await ModWork(() => GameCaches.Clean(docs, chosen));
            SavesSay(InfoBarSeverity.Success, chosen.All(f => f.Group == CacheGroup.ErrorLogs) ? T("Old error logs cleared") : T("Caches cleared. The game rebuilds them when it starts."),
                T("{0} freed", SaveSize(bytes)), T("Undo"), () => Safe(PutCachesBackAsync(last: true)), stays: true);
        }
        catch (Exception e) { App.Log(e); SavesSay(InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        FillSaves();
    }

    // last: what the last clear moved, else every copy still kept
    async Task PutCachesBackAsync(bool last)
    {
        try
        {
            var back = await ModWork(() => GameCaches.Restore(last));
            SavesSay(back ? InfoBarSeverity.Success : InfoBarSeverity.Informational, back ? T("Caches put back") : T("There is no copy to put back"));
        }
        catch (Exception e) { App.Log(e); SavesSay(InfoBarSeverity.Error, T("Some caches were not put back"), ErrorText(e)); }
        FillSaves();
    }
}
