using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// One mod's details: a head with its picture, name, badges and switch; tabs that only show when they hold something
// (Mods for a merge's mods, see MainWindow.MergePanel.cs, Content for what a package holds, Conflicts for the mods that share its game data, Replaces
// for a default replacement, Problems when something needs a look, Notes); facts as tiles; and one main action for the
// case with icon buttons beside it.
public sealed partial class MainWindow
{
    string _detailsTab = "overview";
    string? _detailsFor;

    void FillOneModDetails(ModRow r)
    {
        var m = r.File;
        var mine = Mine(r);
        var merge = m.FileType == "package" ? ModMerge.Info(m) : null;
        if (_detailsFor != m.Rel) { _detailsFor = m.Rel; _detailsTab = merge is not null ? "inside" : "overview"; _mergeInner = null; }
        if (merge is not null)
        {
            DraftFor(r, merge);
            if (merge.Originals.FirstOrDefault(o => o.Rel.Equals(_mergeInner, StringComparison.OrdinalIgnoreCase)) is { } inner) { FillInnerDetails(r, merge, inner, mine); return; }
            _mergeInner = null;
        }

        // what needs a look
        var problems = new List<(string Title, string? Text, string? Action, Action? Run)>();
        if (m.Enabled && !Loaded(m))
            problems.Add((T("The game does not load this folder"), T("Mods\\Resource.cfg doesn't list this folder, so the game skips this mod. Move it into Packages, or a folder inside it."), null, null));
        if (r.DuplicateOf is { } dup) problems.Add((T("Same as {0}", dup), T("The same file is in two places. Keep one and remove the other."), null, null));
        foreach (var f in r.Findings) problems.Add((f, null, T("See in Health"), () => Show(HealthScreen)));

        var tabs = new List<(string Key, string Label)> { ("overview", T("Overview")) };
        if (merge is not null) tabs.Insert(0, ("inside", T("Mods ({0})", merge.Originals.Count)));
        else if (m.FileType == "package") tabs.Add(("content", T("Content")));
        if (merge is null && m.Enabled && (m.FileType == "package" || ModInspect.OnlyInHealth(_conflicts?.Found ?? [], m, _mods).Count > 0)) tabs.Add(("conflicts", T("Conflicts")));
        if (r.Defaults.IsDefault && _gameIndex is not null) tabs.Add(("replaces", T("Replaces")));
        if (problems.Count > 0) tabs.Add(("problems", T("Problems ({0})", problems.Count)));
        if (mine) tabs.Add(("notes", T("Notes")));
        if (!tabs.Any(t => t.Key == _detailsTab)) _detailsTab = "overview";

        ModDetailsHead.Children.Add(DetailsHead(r, merge, problems.Count > 0, mine));
        if (tabs.Count > 1) DetailsTabs(tabs, m);
        else ModDetailsHead.Children.Add(new Border { Height = 1, Margin = new Thickness(0, 8, 0, 0), Background = Res("ApexCardBorderBrush") });

        var host = ModDetails;
        switch (_detailsTab)
        {
            case "inside": host.Children.Add(MergeTab(r, merge!, mine)); break;
            case "content": host.Children.Add(ContentTab(m.Path)); break;
            case "conflicts": host.Children.Add(ConflictsTab(m)); break;
            case "replaces": host.Children.Add(ReplacesTab(r)); break;
            case "problems": foreach (var p in problems) host.Children.Add(Warn(p.Title, p.Text, p.Action, p.Run)); break;
            case "notes": host.Children.Add(NotesBox(ModNotes.Get(m.Rel)?.Notes, 200, text => { ModNotes.SetNotes(r.File.Rel, text); r.Refresh(); })); break;
            default: Overview(r, merge, mine); break;
        }
        DetailsFoot(r, merge, mine);
    }

    // (count: the mod whose conflicts the Conflicts tab counts)
    void DetailsTabs(List<(string Key, string Label)> tabs, ModFile? count)
    {
        var bar = new SelectorBar();
        var side = tabs.Count > 4 ? 6 : 8; // (five tabs, as a default replacement has, still fit the pane)
        foreach (var (key, label) in tabs)
        {
            var tab = new SelectorBarItem { Text = label, Tag = key, IsSelected = key == _detailsTab, FontSize = 14, Padding = new Thickness(side, 10, side, 10) };
            bar.Items.Add(tab);
            if (key == "conflicts" && count is not null) _ = CountConflictsAsync(count, bar, tab);
        }
        bar.SelectionChanged += (s, _) => { if (s.SelectedItem?.Tag is string k && k != _detailsTab) { _detailsTab = k; DispatcherQueue.TryEnqueue(FillModDetails); } }; // (later: the fill removes this bar)
        // (more tabs than fit in a long language: the row scrolls to the open one)
        bar.Loaded += (_, _) => bar.SelectedItem?.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
        ModDetailsHead.Children.Add(new ScrollViewer { Content = bar, Margin = new Thickness(20 - side, 0, 20 - side, 0), HorizontalScrollMode = ScrollMode.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollMode = ScrollMode.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled });
        ModDetailsHead.Children.Add(new Border { Height = 1, Background = Res("ApexCardBorderBrush") });
    }

    // ---------- the head ----------

    UIElement DetailsHead(ModRow r, MergeRecord? merge, bool problem, bool mine)
    {
        var m = r.File;
        var own = ModNotes.PicturePath(m.Rel) is not null;
        // the picture, and the button that changes it
        FrameworkElement tile = PictureTile(r, merge is not null ? "merge" : r.KindIcon, own);
        // (the player's own picture shows whole under the pointer: the banner cuts it to its shape)
        if (own && r.Image is { } whole) ToolTipService.SetToolTip(tile, new ToolTip { Content = new Image { Source = whole, MaxWidth = 300, MaxHeight = 300 } });
        Button? change = null;
        if (mine)
        {
            var label = own ? T("Change picture") : T("Add picture");
            change = IconButton("image-plus", label);
            var menu = new MenuFlyout();
            menu.Items.Add(MenuItem(label, async () =>
            {
                var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
                foreach (var x in new[] { ".png", ".jpg", ".jpeg", ".bmp", ".gif" }) picker.FileTypeFilter.Add(x);
                if (await picker.PickSingleFileAsync() is { } file) SetPicture(r, file.Path);
            }, "image-plus"));
            if (own) menu.Items.Add(MenuItem(T("Remove picture"), () => SetPicture(r, null), "trash-2"));
            change.Click += (_, _) => menu.ShowAt(change);
        }

        var name = merge is not null ? _draft?.Name ?? r.Name : r.Name;
        // (as wide as the name: the pencil stands beside it)
        var nameRow = new Grid { ColumnSpacing = 4, HorizontalAlignment = HorizontalAlignment.Left };
        nameRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        nameRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var nameText = NameText(name, m.Name);
        nameRow.Children.Add(nameText);
        if (mine)
        {
            var rename = IconButton("pencil", T("Rename"));
            rename.Width = rename.Height = 28;
            rename.VerticalAlignment = VerticalAlignment.Top;
            rename.Click += (_, _) =>
            {
                var box = new TextBox { Text = name, FontSize = 14, MinWidth = 0, Height = 36, Padding = new Thickness(12, 8, 12, 8) };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, T("New name"));
                (nameRow.HorizontalAlignment, nameRow.ColumnSpacing) = (HorizontalAlignment.Stretch, 0);
                nameRow.Children.Remove(nameText);
                nameRow.Children.Insert(0, box);
                rename.Visibility = Visibility.Collapsed;
                box.Loaded += (_, _) => { box.Focus(FocusState.Programmatic); box.SelectAll(); };
                var done = false;
                void End(bool save)
                {
                    if (done) return;
                    done = true;
                    if (!save || box.Text.Trim() is not { Length: > 0 } n || n == name) FillModDetails();
                    else if (merge is not null) RenameMerge(r, n); // (with the merge's other changes, when they are saved)
                    else RenameMod(r, n);
                }
                box.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) { e.Handled = true; End(true); } else if (e.Key == Windows.System.VirtualKey.Escape) { e.Handled = true; End(false); } };
                box.LostFocus += (_, _) => End(OnScreen(box));
            };
            Grid.SetColumn(rename, 1);
            nameRow.Children.Add(rename);
        }

        // badges: what kind of thing this is, at a glance
        var badges = new List<Border>();
        if (r.Defaults.IsDefault) badges.Add(StripBadge(T("Default replacement"), "replace", "#9CC7EE"));
        if (problem) badges.Add(StripBadge(T("Needs a look"), "triangle-alert", "#E0A84F"));
        if (m.Locked) badges.Add(m.FileType switch
        {
            "asi" => StripBadge(T("In the game folder"), "plug", "#CECBF6"),
            _ => StripBadge(T("Installed by Apex Launcher"), "lock", "#CECBF6"),
        });
        var sub = string.Join(" · ", new[] { merge is not null ? T("Merged · {0} mods", merge.Originals.Count) : r.Kind, m.Enabled || m.Locked ? null : T("Off") }.OfType<string>());
        return HeadPanel(null, tile, change, nameRow, sub, badges);
    }

    static TextBlock NameText(string name, string tip)
    {
        var t = new TextBlock { Text = name, FontSize = 20, LineHeight = 28, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHeadingLevel(t, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        ToolTipService.SetToolTip(t, tip);
        return t;
    }

    // (the player's own picture fills the banner, a package's small thumbnail stands whole in its middle)
    static FrameworkElement PictureTile(ModRow r, string icon, bool own)
    {
        if (r.Image is { } img) return new Image { Source = img, Stretch = own ? Stretch.UniformToFill : Stretch.Uniform, MaxHeight = own ? double.PositiveInfinity : 104, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        var glyph = Lucide.Icon(icon, 40, Res("ApexMutedBrush"), 1.5);
        (glyph.HorizontalAlignment, glyph.VerticalAlignment) = (HorizontalAlignment.Center, VerticalAlignment.Center);
        return glyph;
    }

    // the head of a mod's details: its picture as a banner with what closes the pane and what changes the picture in its corners,
    // then the name, what it is, and its badges
    UIElement HeadPanel(UIElement? above, FrameworkElement tile, Button? change, UIElement name, string sub, List<Border> badges, string? closeTip = null)
    {
        var panel = new StackPanel { Spacing = Design.Space.X4, Padding = new Thickness(20, 20, 20, closeTip is null ? 12 : 20) };
        if (above is not null) panel.Children.Add(above);
        var banner = new Grid { Height = 136, CornerRadius = new CornerRadius(Design.Radius.Card), Background = Res("ApexTileBrush") };
        tile.HorizontalAlignment = HorizontalAlignment.Center;
        tile.VerticalAlignment = VerticalAlignment.Center;
        banner.Children.Add(tile);
        var close = IconButton("x", closeTip ?? T("Close"));
        close.Click += (_, _) => CloseModDetails();
        banner.Children.Add(Design.Parts.OverPicture(close, VerticalAlignment.Top));
        if (change is not null) banner.Children.Add(Design.Parts.OverPicture(change, VerticalAlignment.Bottom));
        panel.Children.Add(banner);
        var names = new StackPanel { Spacing = Design.Space.TitleToSub };
        names.Children.Add(name);
        names.Children.Add(new TextBlock { Text = sub, FontSize = 14, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        panel.Children.Add(names);
        if (badges.Count > 0)
        {
            var wrap = new WrapPanel { Spacing = 8, HorizontalAlignment = HorizontalAlignment.Left };
            foreach (var b in badges) wrap.Children.Add(b);
            panel.Children.Add(wrap);
        }
        return panel;
    }

    static Border StripBadge(string text, string icon, string color) => Design.Parts.Chip(icon, text, warn: color == "#E0A84F");

    // ---------- Overview ----------

    void Overview(ModRow r, MergeRecord? merge, bool mine)
    {
        var m = r.File;
        var host = ModDetails;
        // the creator, written in place; the guess shows as the hint
        var (who, guessed) = ModCreators.Of(m, _modCreatorGuess);
        var creator = CreatorBox(who, guessed, mine, name => ModNotes.SetCreator(r.File.Rel, name));
        if (m.Locked) host.Children.Add(Note("lock", "ApexMutedBrush", ModRow.LockText(m)));
        var place = new Grid { ColumnSpacing = 12, HorizontalAlignment = HorizontalAlignment.Right };
        place.ColumnDefinitions.Add(new ColumnDefinition());
        place.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var where = FactText(ModRow.Place(m));
        ToolTipService.SetToolTip(where, ModRow.Place(m));
        var file = FactText(m.Name);
        ToolTipService.SetToolTip(file, m.Name);
        place.Children.Add(where);
        if (mine)
        {
            var move = new HyperlinkButton { Content = T("Move…"), FontSize = 13, Padding = new Thickness(0) };
            move.Click += (_, _) => MoveMenu([r]).ShowAt(move);
            Grid.SetColumn(move, 1);
            place.Children.Add(move);
        }
        host.Children.Add(Facts((T("Size"), FactText(r.Size)), (T("Added"), FactText(Day(Added(m)))), (T("Folder"), place), (T("File"), file), (T("Creator"), creator)));
        if (merge is not null)
        {
            var notes = new StackPanel { Spacing = 10 };
            notes.Children.Add(Note("lightbulb", "ApexAccentBrush", T("The game loads this as one file instead of {0}, so it starts faster. The {0} mods are kept as they were, and you can take any of them out.", merge.Originals.Count)));
            notes.Children.Add(Note("circle-check", "ApexSuccessBrush", T("Checked when it was made, on {0}: everything matches the originals.", Day(merge.Created.ToLocalTime()))));
            host.Children.Add(PaneCard(notes, new Thickness(14, 12, 14, 13)));
        }
        if (!m.Enabled && !m.Locked && HasItems(r))
            host.Children.Add(Warn(T("Off: saving now removes its items"), T("Lots and Sims that use its objects or outfits lose them the next time you save without it.")));

        if (mine) host.Children.Add(LinkSection(r));

        // what it is
        var what = Section(T("What it is"), mine ? T("Change") : null, out var change);
        if (change is not null) change.Click += (_, _) => PickKinds(r, change);
        what.Children.Add(KindChips(r, ModNotes.Get(m.Rel)?.Kinds is not null));
        host.Children.Add(what);

        if (mine)
        {
            var tags = Section(T("Tags"), null, out _);
            tags.Children.Add(TagsRow([r]));
            host.Children.Add(tags);
        }

        host.Children.Add(Actions(CanOpenInS3pe(r) ? ActionRow("package", T("Open in s3pe"), null, () => Safe(OpenInS3peAsync(r))) : null,
            ActionRow("folder-search", T("Show in Explorer"), null, () => ShowFile(r.File.Path))));
    }

    // what the pane's cards are made of: one surface, one corner
    static Border PaneCard(UIElement child, Thickness padding = default) => Design.Parts.Card(child, padding);

    // facts, one to a row: what it is on the left, the value on the right edge
    static Border Facts(params (string Label, FrameworkElement Value)[] facts)
    {
        var rows = new StackPanel();
        foreach (var (label, value) in facts)
        {
            var g = new Grid { ColumnSpacing = 12, MinHeight = 48, Padding = new Thickness(16, 0, 16, 0), BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, rows.Children.Count == 0 ? 0 : 1, 0, 0) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.Children.Add(new TextBlock { Text = label, FontSize = 13, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
            value.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(value, 1);
            g.Children.Add(value);
            rows.Children.Add(g);
        }
        return PaneCard(rows);
    }

    static TextBlock FactText(string t) => new() { Text = t, FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextAlignment = TextAlignment.Right, TextTrimming = TextTrimming.CharacterEllipsis };

    TextBox CreatorBox(string? who, bool guessed, bool mine, Action<string> save)
    {
        var creator = Design.Parts.QuietField(new TextBox { Text = guessed ? "" : who ?? "", PlaceholderText = who ?? T("Unknown"), TextAlignment = TextAlignment.Right,
            Margin = new Thickness(0, 0, -8, 0) }, mine); // (its text stands on the other values' edge)
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(creator, T("Creator"));
        ToolTipService.SetToolTip(creator, guessed ? T("Guessed from the file name. Type the creator's name to change it.") : T("The creator's name"));
        creator.LostFocus += (_, _) =>
        {
            try { save(creator.Text); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        };
        return creator;
    }

    static WrapPanel KindChips(ModRow r, bool chosen)
    {
        var kinds = new WrapPanel { Spacing = 8 };
        foreach (var k in r.Kinds.Take(8))
        {
            kinds.Children.Add(Design.Parts.Chip(KindIcon(k), KindLabel(k)));
        }
        if (chosen) kinds.Children.Add(new TextBlock { Text = T("Chosen by you"), FontSize = 11, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
        return kinds;
    }

    // a small heading with an optional link on its right
    StackPanel Section(string title, string? link, out HyperlinkButton? linkButton)
    {
        var s = new StackPanel { Spacing = 12 };
        var head = new Grid();
        head.Children.Add(Design.Parts.Heading(title));
        linkButton = null;
        if (link is not null)
        {
            linkButton = new HyperlinkButton { Content = link, Padding = new Thickness(0), FontSize = 13, HorizontalAlignment = HorizontalAlignment.Right };
            head.Children.Add(linkButton);
        }
        s.Children.Add(head);
        return s;
    }

    static Grid Note(string icon, string brush, string text)
    {
        var g = new Grid { ColumnSpacing = 8 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        var i = Lucide.Icon(icon, 15, Res(brush));
        i.VerticalAlignment = VerticalAlignment.Top;
        i.Margin = new Thickness(0, 1, 0, 0);
        g.Children.Add(i);
        var t = new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, LineHeight = 18 };
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        return g;
    }

    TextBox NotesBox(string? text, double height, Action<string> save)
    {
        var notes = Field(new TextBox
        {
            Text = text ?? "", PlaceholderText = T("Notes: where it came from, what it changes"), AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap, MinHeight = height, CornerRadius = new CornerRadius(Design.Radius.Card),
        });
        notes.Padding = new Thickness(15, 11, 15, 11);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(notes, T("Notes"));
        notes.LostFocus += (_, _) =>
        {
            try { save(notes.Text); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The notes were not saved"), ErrorText(e)); }
        };
        return notes;
    }

    // ---------- Replaces ----------

    // what it replaces: the pills, then the game's items with the mod's pictures (See all for every one)
    UIElement ReplacesTab(ModRow r)
    {
        var s = new StackPanel { Spacing = 16 };
        if (_gameIndex is { } gi) s.Children.Add(ReplacesPanel(r, gi));
        return s;
    }

    // "Replaces 253 Create a Sim parts" / "Replaces 172 textures" / "Replaces 3 objects and 12 textures"
    static string ReplacesSummary(Replacements d)
    {
        var parts = new List<string>();
        if (d.Objects.Count > 0) parts.Add(d.Objects.Count == 1 ? T("1 object") : T("{0} objects", d.Objects.Count));
        if (d.CasParts.Count > 0) parts.Add(d.CasParts.Count == 1 ? T("1 Create a Sim part") : T("{0} Create a Sim parts", d.CasParts.Count));
        if (d.Images > 0) parts.Add(d.Images == 1 ? T("1 texture") : T("{0} textures", d.Images));
        return parts.Count == 0 ? (d.Replaced == 1 ? T("Replaces 1 of the game's resources") : T("Replaces {0} of the game's resources", d.Replaced)) : T("Replaces {0}", string.Join(", ", parts));
    }

    // ---------- the footer ----------

    void DetailsFoot(ModRow r, MergeRecord? merge, bool mine)
    {
        if (merge is not null && DraftRow() is not null) { DraftFoot(r); return; }
        var m = r.File;
        var f = ModDetailsFoot;
        f.Visibility = Visibility.Visible;
        f.ColumnDefinitions.Add(new ColumnDefinition());
        Button main;
        if (m.Locked) main = MainButton(T("Open in Components"), () => Show(ComponentsStep));
        else if (merge is not null && mine)
        {
            main = WideButton("scissors", T("Split into mods"), () => Safe(SplitAsync(r)));
            main.Height = Design.Size.ControlLarge;
            ToolTipService.SetToolTip(main, T("Split it into its {0} original mods", merge.Originals.Count));
        }
        else main = MainButton(m.Enabled ? T("Turn off") : T("Turn on"), () => SetMods([r], !r.File.Enabled), accent: !m.Enabled);
        f.Children.Add(main);
        void Icon(Button b)
        {
            b.Width = Design.Size.ControlLarge;
            b.Height = Design.Size.ControlLarge;
            b.BorderThickness = new Thickness(1);
            b.BorderBrush = Res("ApexCardBorderBrush");
            f.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(b, f.ColumnDefinitions.Count - 1);
            f.Children.Add(b);
        }
        if (merge is not null && mine)
        {
            var power = IconButton("power", m.Enabled ? T("Turn off") : T("Turn on"));
            power.Click += (_, _) => SetMods([r], !r.File.Enabled);
            Icon(power);
        }
        if (mine)
        {
            var remove = IconButton("trash-2", T("Move to the Recycle Bin"), "ApexErrorBrush");
            remove.Click += (_, _) => RemoveMods([r]);
            Icon(remove);
        }
    }

    // (accent: the screen's main action; turning mods off is not one)
    static Button MainButton(string text, Action click, bool accent = true)
    {
        var b = new Button { Content = text, HorizontalAlignment = HorizontalAlignment.Stretch, Height = Design.Size.ControlLarge };
        if (accent) b.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        b.Click += (_, _) => click();
        return Dims(b);
    }
}
