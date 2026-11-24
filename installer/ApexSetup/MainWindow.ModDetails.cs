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
        if (r.DuplicateOf is { } dup) problems.Add((T("Same as {0}", dup), T("The same file is in two places. Keep the newer one and remove the other."), null, null));
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
        foreach (var (key, label) in tabs)
        {
            var tab = new SelectorBarItem { Text = label, Tag = key, IsSelected = key == _detailsTab, FontSize = 13, Padding = new Thickness(8, 10, 8, 8) };
            bar.Items.Add(tab);
            if (key == "conflicts" && count is not null) _ = CountConflictsAsync(count, bar, tab);
        }
        bar.SelectionChanged += (s, _) => { if (s.SelectedItem?.Tag is string k && k != _detailsTab) { _detailsTab = k; DispatcherQueue.TryEnqueue(FillModDetails); } }; // (later: the fill removes this bar)
        // (more tabs than fit in a long language: the row scrolls to the open one)
        bar.Loaded += (_, _) => bar.SelectedItem?.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
        ModDetailsHead.Children.Add(new ScrollViewer { Content = bar, Margin = new Thickness(12, 0, 12, 0), HorizontalScrollMode = ScrollMode.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollMode = ScrollMode.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled });
        ModDetailsHead.Children.Add(new Border { Height = 1, Background = Res("ApexCardBorderBrush") });
    }

    // ---------- the head ----------

    UIElement DetailsHead(ModRow r, MergeRecord? merge, bool problem, bool mine)
    {
        var m = r.File;
        var own = ModNotes.PicturePath(m.Rel) is not null;
        // the picture, which is also its button
        FrameworkElement tile = PictureTile(r, merge is not null ? "merge" : r.KindIcon, own);
        if (mine)
        {
            var label = own ? T("Change picture") : T("Add picture");
            var mark = new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), Background = Res("ApexRaisedBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1),
                HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 2, 2), Child = Lucide.Icon("image-plus", 12, Res("ApexSoftTextBrush")) };
            var both = new Grid();
            both.Children.Add(tile);
            both.Children.Add(mark);
            var pic = new Button { Content = both, Padding = new Thickness(0), Width = 60, Height = 60, CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), VerticalAlignment = VerticalAlignment.Top };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(pic, label);
            // (the player's own picture shows whole under the pointer: the tile is small)
            ToolTipService.SetToolTip(pic, own && r.Image is { } whole ? new ToolTip { Content = new Image { Source = whole, MaxWidth = 300, MaxHeight = 300 } } : label);
            var menu = new MenuFlyout();
            menu.Items.Add(MenuItem(label, async () =>
            {
                var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
                foreach (var x in new[] { ".png", ".jpg", ".jpeg", ".bmp", ".gif" }) picker.FileTypeFilter.Add(x);
                if (await picker.PickSingleFileAsync() is { } file) SetPicture(r, file.Path);
            }));
            if (own) menu.Items.Add(MenuItem(T("Remove picture"), () => SetPicture(r, null)));
            pic.Click += (_, _) => menu.ShowAt(pic);
            tile = pic;
        }

        var name = merge is not null ? _draft?.Name ?? r.Name : r.Name;
        var nameRow = new Grid { ColumnSpacing = 2 };
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
                var box = new TextBox { Text = name, FontSize = 15, MinWidth = 0 };
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, T("New name"));
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

        ToggleSwitch? sw = null;
        if (!m.Locked)
        {
            sw = new ToggleSwitch { IsOn = m.Enabled, OnContent = "", OffContent = "", MinWidth = 0 };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(sw, m.Enabled ? T("Turn off") : T("Turn on"));
            var on = sw;
            sw.Toggled += (_, _) => { if (on.IsOn != r.File.Enabled) SetMods([r], on.IsOn); };
        }

        // badges: what kind of thing this is, at a glance
        var badges = new List<Border>();
        if (merge is not null) badges.Add(StripBadge(T("Merged · {0} mods", merge.Originals.Count), "merge", "#CECBF6"));
        if (r.Defaults.IsDefault) badges.Add(StripBadge(T("Default replacement"), "replace", "#9CC7EE"));
        if (problem) badges.Add(StripBadge(T("Needs a look"), "triangle-alert", "#E0A84F"));
        if (m.Locked) badges.Add(m.FileType switch
        {
            "asi" => StripBadge(T("In the game folder"), "plug", "#CECBF6"),
            _ => StripBadge(T("Installed by Apex Launcher"), "lock", "#CECBF6"),
        });
        var sub = string.Join(" · ", new[] { r.Kinds.Count > 0 ? KindLabel(r.Kinds[0]) : null, r.Size, m.Enabled || m.Locked ? null : T("Off") }.OfType<string>());
        return HeadPanel(r, null, tile, nameRow, sub, sw, badges);
    }

    static TextBlock NameText(string name, string tip)
    {
        var t = new TextBlock { Text = name, FontSize = 17, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetHeadingLevel(t, Microsoft.UI.Xaml.Automation.Peers.AutomationHeadingLevel.Level2);
        ToolTipService.SetToolTip(t, tip);
        return t;
    }

    // (the player's own picture fills it, a package's small thumbnail shows whole)
    static Border PictureTile(ModRow r, string icon, bool own)
    {
        UIElement inside;
        if (r.Image is { } img) inside = new Image { Source = img, Stretch = own ? Stretch.UniformToFill : Stretch.Uniform };
        else
        {
            var glyph = Lucide.Icon(icon, 28, r.GlyphBrush, 1.7);
            glyph.HorizontalAlignment = HorizontalAlignment.Center;
            glyph.VerticalAlignment = VerticalAlignment.Center;
            inside = glyph;
        }
        return new Border { Width = 60, Height = 60, CornerRadius = new CornerRadius(12), Background = r.TileBrush, VerticalAlignment = VerticalAlignment.Top, Child = inside };
    }

    // the head of one mod's details, on a faint wash of its colour: picture, name and what it is, close and the switch; its badges under them
    UIElement HeadPanel(ModRow r, UIElement? above, FrameworkElement tile, UIElement name, string sub, ToggleSwitch? sw, List<Border> badges)
    {
        var panel = new StackPanel { Spacing = 12, Padding = new Thickness(20, 14, 12, 10) };
        if (r.TileBrush is SolidColorBrush wash)
            panel.Background = new LinearGradientBrush
            {
                StartPoint = new Windows.Foundation.Point(0, 0), EndPoint = new Windows.Foundation.Point(0, 1),
                GradientStops = { new GradientStop { Color = wash.Color, Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(0, wash.Color.R, wash.Color.G, wash.Color.B), Offset = 1 } },
            };
        if (above is not null) panel.Children.Add(above);
        var g = new Grid { ColumnSpacing = 14 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        g.Children.Add(tile);
        var names = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        names.Children.Add(name);
        names.Children.Add(new TextBlock { Text = sub, FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(names, 1);
        g.Children.Add(names);
        var right = new StackPanel { Spacing = 2 };
        var close = IconButton("x", T("Close"));
        close.HorizontalAlignment = HorizontalAlignment.Right;
        close.Click += (_, _) => CloseModDetails();
        right.Children.Add(close);
        if (sw is not null) { sw.HorizontalAlignment = HorizontalAlignment.Right; right.Children.Add(sw); }
        Grid.SetColumn(right, 2);
        g.Children.Add(right);
        panel.Children.Add(g);
        if (badges.Count > 0)
        {
            var wrap = new WrapPanel { Spacing = 6, HorizontalAlignment = HorizontalAlignment.Left };
            foreach (var b in badges) wrap.Children.Add(b);
            panel.Children.Add(wrap);
        }
        return panel;
    }

    static Border StripBadge(string text, string icon, string color)
    {
        var brush = TagDot.BrushOf(color);
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        s.Children.Add(Lucide.Icon(icon, 12, brush, 2.2));
        s.Children.Add(new TextBlock { Text = text, FontSize = 12, FontWeight = FontWeights.SemiBold, Foreground = brush, VerticalAlignment = VerticalAlignment.Center });
        return new Border { Child = s, Height = 26, Padding = new Thickness(10, 0, 10, 0), CornerRadius = new CornerRadius(13), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB3, 11, 12, 14)) };
    }

    static Button StripButton(string icon, string tip)
    {
        var b = new Button { Content = Lucide.Icon(icon, 15, Res("ApexSoftTextBrush")), Width = 32, Height = 32, Padding = new Thickness(0), CornerRadius = new CornerRadius(8),
            Background = new SolidColorBrush(Pick(0x990B0C0E, 0xCCFFFFFF)), BorderThickness = new Thickness(0) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, tip);
        ToolTipService.SetToolTip(b, tip);
        return b;
    }

    // ---------- Overview ----------

    void Overview(ModRow r, MergeRecord? merge, bool mine)
    {
        var m = r.File;
        var host = ModDetails;
        if (merge is not null)
        {
            var explain = Note("lightbulb", "ApexAccentBrush", T("The game loads this as one file instead of {0}, so it starts faster. The {0} mods are kept as they were, and you can take any of them out.", merge.Originals.Count));
            host.Children.Add(new Border { Child = explain, Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(10), Background = Res("ApexAccentSoftBrush"), BorderBrush = Res("ApexAccentLineBrush"), BorderThickness = new Thickness(1) });
        }
        // the creator, written in place; the guess shows as the hint
        var (who, guessed) = ModCreators.Of(m, _modCreatorGuess);
        var creator = CreatorBox(who, guessed, mine, name => ModNotes.SetCreator(r.File.Rel, name));
        if (m.Locked) host.Children.Add(Note("lock", "ApexMutedBrush", ModRow.LockText(m)));
        host.Children.Add(Tiles((T("Size"), TileText(r.Size)), (T("Added"), TileText(Added(m).ToString("d"))), (T("Creator"), creator),
            merge is not null ? (T("Made from"), TileText(T("{0} mods", merge.Originals.Count)))
            : (T("State"), TileText(!m.Locked ? (m.Enabled ? T("On") : T("Off")) : m.FileType switch { "asi" => T("Loaded"), _ => T("Managed") },
                m.Enabled || m.Locked ? null : "ApexWarningBrush"))));
        if (merge is not null) host.Children.Add(Note("circle-check", "ApexSuccessBrush", T("Checked on {0}, when it was made: every resource matched.", merge.Created.ToLocalTime().ToString("d"))));
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

        // where it is, in full
        var where = Section(T("Where"), mine ? T("Move…") : null, out var move);
        if (move is not null) move.Click += (_, _) => MoveMenu([r]).ShowAt(move);
        where.Children.Add(new TextBlock { Text = ModRow.Place(m) + " › " + m.Name, FontSize = 13, TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush") });
        host.Children.Add(where);

        var actions = new StackPanel { Spacing = 8 };
        if (CanOpenInS3pe(r)) actions.Children.Add(ActionRow("package", T("Open in s3pe"), null, () => Safe(OpenInS3peAsync(r))));
        actions.Children.Add(ActionRow("folder-search", T("Show file"), null, () => ShowFile(r.File.Path)));
        host.Children.Add(actions);
    }

    // facts as tiles, two to a row
    static Grid Tiles(params (string Label, FrameworkElement Value)[] facts)
    {
        var tiles = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
        tiles.ColumnDefinitions.Add(new ColumnDefinition());
        tiles.ColumnDefinitions.Add(new ColumnDefinition());
        for (var i = 0; i < facts.Length; i++)
        {
            if (i % 2 == 0) tiles.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var s = new StackPanel { Spacing = 3 };
            s.Children.Add(new TextBlock { Text = facts[i].Label, FontSize = 11, Foreground = Res("ApexMutedBrush") });
            s.Children.Add(facts[i].Value);
            var b = new Border { Child = s, Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) };
            Grid.SetRow(b, i / 2);
            Grid.SetColumn(b, i % 2);
            tiles.Children.Add(b);
        }
        return tiles;
    }

    static TextBlock TileText(string t, string? brush = null)
    {
        var b = new TextBlock { Text = t, FontSize = 14, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        if (brush is not null) b.Foreground = Res(brush);
        return b;
    }

    TextBox CreatorBox(string? who, bool guessed, bool mine, Action<string> save)
    {
        var creator = new TextBox { Text = guessed ? "" : who ?? "", PlaceholderText = who ?? T("Unknown"), FontSize = 14, FontWeight = FontWeights.SemiBold, MinWidth = 0, MinHeight = 0, Padding = new Thickness(0),
            BorderThickness = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), IsReadOnly = !mine };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(creator, T("Creator"));
        ToolTipService.SetToolTip(creator, guessed ? T("Guessed from the file name. Type the maker's name to change it.") : T("The maker's name."));
        creator.LostFocus += (_, _) =>
        {
            try { save(creator.Text); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        };
        return creator;
    }

    static WrapPanel KindChips(ModRow r, bool chosen)
    {
        var kinds = new WrapPanel { Spacing = 6 };
        foreach (var k in r.Kinds.Take(8))
        {
            var chip = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
            chip.Children.Add(Lucide.Icon(KindIcon(k), 12, Res("ApexMutedBrush")));
            chip.Children.Add(new TextBlock { Text = KindLabel(k), FontSize = 12, VerticalAlignment = VerticalAlignment.Center });
            kinds.Children.Add(new Border { Child = chip, Height = 26, Padding = new Thickness(10, 0, 10, 0), CornerRadius = new CornerRadius(13), Background = Res("ApexPillBrush") });
        }
        if (chosen) kinds.Children.Add(new TextBlock { Text = T("Chosen by you"), FontSize = 11, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
        return kinds;
    }

    // a small heading with an optional link on its right
    StackPanel Section(string title, string? link, out HyperlinkButton? linkButton)
    {
        var s = new StackPanel { Spacing = 8 };
        var head = new Grid();
        head.Children.Add(new TextBlock { Text = title, FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
        linkButton = null;
        if (link is not null)
        {
            linkButton = new HyperlinkButton { Content = link, Padding = new Thickness(0), FontSize = 12, HorizontalAlignment = HorizontalAlignment.Right };
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
        var notes = new TextBox
        {
            Text = text ?? "", PlaceholderText = T("Notes: where it came from, what it changes"), AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap, MinHeight = height,
        };
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
        return parts.Count == 0 ? T("Replaces {0} of the game's resources", d.Replaced) : T("Replaces {0}", string.Join(", ", parts));
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
            main.Height = 36;
            ToolTipService.SetToolTip(main, T("Split it into its {0} original mods", merge.Originals.Count));
        }
        else main = MainButton(m.Enabled ? T("Turn off") : T("Turn on"), () => SetMods([r], !r.File.Enabled));
        f.Children.Add(main);
        void Icon(Button b)
        {
            b.Width = 36;
            b.Height = 36;
            b.BorderThickness = new Thickness(1);
            b.BorderBrush = Res("ApexCardBorderBrush");
            f.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(b, f.ColumnDefinitions.Count - 1);
            f.Children.Add(b);
        }
        if (merge is not null && mine)
        {
            var power = IconButton(m.Enabled ? "power-off" : "power", m.Enabled ? T("Turn off") : T("Turn on"));
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

    static Button MainButton(string text, Action click)
    {
        var b = new Button { Content = text, Style = (Style)Application.Current.Resources["AccentButtonStyle"], HorizontalAlignment = HorizontalAlignment.Stretch, Height = 36 };
        b.Click += (_, _) => click();
        return Dims(b);
    }
}
