using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// One mod's details: a picture strip with its name, badges and switch; tabs that only show when they hold something
// (Inside for what it holds or a merge's mods, Replaces for a default replacement, Problems when something needs a look,
// Notes); facts as tiles; and one main action for the case with icon buttons beside it.
public sealed partial class MainWindow
{
    string _detailsTab = "overview";
    string? _detailsFor;
    static readonly string[] MergeColors = ["#6CB2EA", "#B598F6", "#93D27C", "#F2B25C", "#F08FC4", "#62CBDD", "#E8C466", "#EF9872"];

    void FillOneModDetails(ModRow r)
    {
        var m = r.File;
        var mine = Mine(r);
        if (_detailsFor != m.Rel) { _detailsFor = m.Rel; _detailsTab = "overview"; }
        var merge = m.FileType == "package" ? ModMerge.Info(m) : null;

        // what needs a look
        var problems = new List<(string Title, string? Text, string? Action, Action? Run)>();
        if (m.Enabled && !Loaded(m))
            problems.Add((T("The game does not load this folder"), T("Mods\\Resource.cfg does not list it, so the game skips this mod. Move it into Packages (or a folder inside it) to use it."), null, null));
        if (r.DuplicateOf is { } dup) problems.Add((T("Same as {0}", dup), T("The same file is in two places. Keep the newer one and remove the other."), null, null));
        foreach (var f in r.Findings) problems.Add((f, null, T("See in Health"), () => Show(HealthScreen)));

        var tabs = new List<(string Key, string Label)> { ("overview", T("Overview")) };
        if (merge is not null) tabs.Add(("inside", T("Inside ({0})", merge.Originals.Count)));
        else if (m.FileType is "package" or "library") tabs.Add(("inside", T("Inside")));
        if (r.Defaults.IsDefault && _gameIndex is not null) tabs.Add(("replaces", T("Replaces")));
        if (problems.Count > 0) tabs.Add(("problems", T("Problems ({0})", problems.Count)));
        if (mine) tabs.Add(("notes", T("Notes")));
        if (!tabs.Any(t => t.Key == _detailsTab)) _detailsTab = "overview";

        ModDetailsHead.Children.Add(DetailsStrip(r, merge, problems.Count > 0, mine));
        if (tabs.Count > 1)
        {
            var bar = new SelectorBar { Margin = new Thickness(12, 0, 12, 0) };
            foreach (var (key, label) in tabs) bar.Items.Add(new SelectorBarItem { Text = label, Tag = key, IsSelected = key == _detailsTab });
            bar.SelectionChanged += (s, _) => { if (s.SelectedItem?.Tag is string k && k != _detailsTab) { _detailsTab = k; FillModDetails(); } };
            ModDetailsHead.Children.Add(bar);
            ModDetailsHead.Children.Add(new Border { Height = 1, Background = Res("ApexCardBorderBrush") });
        }

        var host = ModDetails;
        switch (_detailsTab)
        {
            case "inside": host.Children.Add(merge is not null ? MergeCards(r, merge) : InsidePanel(r)); break;
            case "replaces": host.Children.Add(ReplacesTab(r)); break;
            case "problems": foreach (var p in problems) host.Children.Add(Warn(p.Title, p.Text, p.Action, p.Run)); break;
            case "notes": host.Children.Add(NotesBox(r, 200)); break;
            default: Overview(r, merge, mine); break;
        }
        DetailsFoot(r, merge, mine);
    }

    // ---------- the strip ----------

    UIElement DetailsStrip(ModRow r, MergeRecord? merge, bool problem, bool mine)
    {
        var m = r.File;
        var g = new Grid { Height = 176, Background = Res("ApexPillBrush") };
        g.Children.Add(new Border { Background = r.TileBrush });
        var placeholder = Lucide.Icon(merge is not null ? "merge" : r.KindIcon, 48, r.GlyphBrush, 1.4);
        placeholder.HorizontalAlignment = HorizontalAlignment.Center;
        placeholder.VerticalAlignment = VerticalAlignment.Center;
        placeholder.Margin = new Thickness(0, 0, 0, 24);
        g.Children.Add(placeholder);
        // (the player's own picture fills the strip; a package's small thumbnail shows at twice its size)
        if (r.Image is { } img)
            g.Children.Add(ModNotes.PicturePath(m.Rel) is not null
                ? new Image { Source = img, Stretch = Stretch.UniformToFill }
                : new Image { Source = img, Stretch = Stretch.Uniform, MaxWidth = 112, MaxHeight = 112, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 0, 24) });

        // badges: what kind of thing this is, at a glance
        // (wrapping before the corner buttons: two badges on a narrow panel take two lines, never run under them)
        var badges = new WrapPanel { Spacing = 6, Margin = new Thickness(12, 12, mine || _modsNarrow ? 96 : 12, 12), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        if (merge is not null) badges.Children.Add(StripBadge(T("Merged · {0} mods", merge.Originals.Count), "merge", "#CECBF6"));
        if (r.Defaults.IsDefault) badges.Children.Add(StripBadge(T("Default replacement"), "replace", "#9CC7EE"));
        if (problem) badges.Children.Add(StripBadge(T("Needs a look"), "triangle-alert", "#E0A84F"));
        if (m.Locked) badges.Children.Add(m.FileType switch
        {
            "sims3pack" => StripBadge(T("Not installed yet"), "package", "#E6E6EC"),
            "library" => StripBadge(T("In the Library"), "library", "#E6E6EC"),
            "asi" => StripBadge(T("In the game folder"), "plug", "#CECBF6"),
            _ => StripBadge(T("Installed by Apex Launcher"), "lock", "#CECBF6"),
        });
        g.Children.Add(badges);
        // (badges on two lines push the centre picture down, clear of them)
        if (badges.Children.Count > 1) placeholder.Margin = new Thickness(0, 52, 0, 20);

        var corner = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Margin = new Thickness(12), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top };
        if (mine)
        {
            var own = ModNotes.PicturePath(m.Rel) is not null;
            var pic = StripButton("image-plus", own ? T("Change picture") : T("Add picture"));
            var menu = new MenuFlyout();
            menu.Items.Add(MenuItem(own ? T("Change picture") : T("Add picture"), async () =>
            {
                var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
                foreach (var x in new[] { ".png", ".jpg", ".jpeg", ".bmp", ".gif" }) picker.FileTypeFilter.Add(x);
                if (await picker.PickSingleFileAsync() is { } file) SetPicture(r, file.Path);
            }));
            if (own) menu.Items.Add(MenuItem(T("Remove picture"), () => SetPicture(r, null)));
            pic.Click += (_, _) => menu.ShowAt(pic);
            corner.Children.Add(pic);
        }
        if (_modsNarrow)
        {
            var close = StripButton("x", T("Close"));
            close.Click += (_, _) => CloseModDetails();
            corner.Children.Add(close);
        }
        g.Children.Add(corner);

        // the name over the strip's foot, with its switch
        var foot = new Grid { VerticalAlignment = VerticalAlignment.Bottom, Padding = new Thickness(20, 36, 16, 12), ColumnSpacing = 10 };
        foot.Background = new LinearGradientBrush
        {
            StartPoint = new Windows.Foundation.Point(0, 1), EndPoint = new Windows.Foundation.Point(0, 0),
            GradientStops = { new GradientStop { Color = Windows.UI.Color.FromArgb(255, 15, 16, 19), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(0, 15, 16, 19), Offset = 1 } },
        };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var names = new StackPanel { Spacing = 2 };
        var nameRow = new Grid { ColumnSpacing = 2 };
        nameRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Auto) });
        nameRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var nameText = new TextBlock { Text = r.Name, FontSize = 18, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        ToolTipService.SetToolTip(nameText, m.Name);
        nameRow.Children.Add(nameText);
        if (mine)
        {
            var rename = IconButton("pencil", T("Rename"));
            rename.Width = rename.Height = 28;
            rename.Click += (_, _) =>
            {
                var box = new TextBox { Text = r.Name, FontSize = 15 };
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
                    if (save && box.Text.Trim() is { Length: > 0 } n && n != r.Name) RenameMod(r, n);
                    else FillModDetails();
                }
                box.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) { e.Handled = true; End(true); } else if (e.Key == Windows.System.VirtualKey.Escape) { e.Handled = true; End(false); } };
                box.LostFocus += (_, _) => End(true);
            };
            Grid.SetColumn(rename, 1);
            nameRow.Children.Add(rename);
        }
        names.Children.Add(nameRow);
        names.Children.Add(new TextBlock
        {
            Text = string.Join(" · ", new[] { r.Kinds.Count > 0 ? KindLabel(r.Kinds[0]) : null, r.Size, m.Enabled || m.Locked ? null : T("Off") }.OfType<string>()),
            FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis,
        });
        foot.Children.Add(names);
        if (!m.Locked && m.FileType != "sims3pack")
        {
            var sw = new ToggleSwitch { IsOn = m.Enabled, OnContent = "", OffContent = "", MinWidth = 0, VerticalAlignment = VerticalAlignment.Bottom };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(sw, m.Enabled ? T("Turn off") : T("Turn on"));
            sw.Toggled += (_, _) => { if (sw.IsOn != r.File.Enabled) SetMods([r], sw.IsOn); };
            Grid.SetColumn(sw, 1);
            foot.Children.Add(sw);
        }
        g.Children.Add(foot);
        return g;
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
            Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x99, 11, 12, 14)), BorderThickness = new Thickness(0) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, tip);
        ToolTipService.SetToolTip(b, tip);
        return b;
    }

    // ---------- Overview ----------

    void Overview(ModRow r, MergeRecord? merge, bool mine)
    {
        var m = r.File;
        var host = ModDetails;
        var tiles = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
        tiles.ColumnDefinitions.Add(new ColumnDefinition());
        tiles.ColumnDefinitions.Add(new ColumnDefinition());
        void Tile(int i, string label, FrameworkElement value)
        {
            if (i % 2 == 0) tiles.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var s = new StackPanel { Spacing = 3 };
            s.Children.Add(new TextBlock { Text = label, FontSize = 11, Foreground = Res("ApexMutedBrush") });
            s.Children.Add(value);
            var b = new Border { Child = s, Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) };
            Grid.SetRow(b, i / 2);
            Grid.SetColumn(b, i % 2);
            tiles.Children.Add(b);
        }
        TextBlock Big(string t, string? brush = null)
        {
            var b = new TextBlock { Text = t, FontSize = 14, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
            if (brush is not null) b.Foreground = Res(brush);
            return b;
        }
        Tile(0, T("Size"), Big(r.Size));
        Tile(1, T("Added"), Big(Added(m).ToString("d")));
        // the creator, written in place; the guess shows as the hint
        var (who, guessed) = ModCreators.Of(m, _modCreatorGuess);
        var creator = new TextBox { Text = guessed ? "" : who ?? "", PlaceholderText = who ?? T("Unknown"), FontSize = 14, FontWeight = FontWeights.SemiBold, MinWidth = 0, MinHeight = 0, Padding = new Thickness(0),
            BorderThickness = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), IsReadOnly = !mine };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(creator, T("Creator"));
        ToolTipService.SetToolTip(creator, guessed ? T("Guessed from the file name; write the maker's name to set it.") : T("The maker's name."));
        creator.LostFocus += (_, _) =>
        {
            try { ModNotes.SetCreator(r.File.Rel, creator.Text); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        };
        Tile(2, T("Creator"), creator);
        Tile(3, merge is not null ? T("Made from") : T("State"),
            merge is not null ? Big(T("{0} mods", merge.Originals.Count))
            : Big(!m.Locked ? (m.Enabled ? T("On") : T("Off")) : m.FileType switch { "sims3pack" => T("Not installed"), "library" => T("In the Library"), "asi" => T("Loaded"), _ => T("Managed") },
                m.Enabled || m.Locked ? null : "ApexWarningBrush"));
        if (m.Locked) host.Children.Add(Note("lock", "ApexMutedBrush", ModRow.LockText(m)));
        host.Children.Add(tiles);
        if (!m.Enabled && !m.Locked && HasItems(r))
            host.Children.Add(Warn(T("Off: a game saved now loses its items"), T("Lots and Sims that use its objects or outfits lose them the next time a game is saved without it.")));

        // what it is
        var what = Section(T("What it is"), mine ? T("Change") : null, out var change);
        var kinds = new WrapPanel { Spacing = 6 };
        foreach (var k in r.Kinds.Take(8))
        {
            var chip = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
            chip.Children.Add(Lucide.Icon(KindIcon(k), 12, Res("ApexMutedBrush")));
            chip.Children.Add(new TextBlock { Text = KindLabel(k), FontSize = 12, VerticalAlignment = VerticalAlignment.Center });
            kinds.Children.Add(new Border { Child = chip, Height = 26, Padding = new Thickness(10, 0, 10, 0), CornerRadius = new CornerRadius(13), Background = Res("ApexPillBrush") });
        }
        if (ModNotes.Get(m.Rel)?.Kinds is not null)
            kinds.Children.Add(new TextBlock { Text = T("Chosen by you"), FontSize = 11, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
        if (change is not null) change.Click += (_, _) => PickKinds(r, change);
        what.Children.Add(kinds);
        host.Children.Add(what);

        if (mine)
        {
            var tags = Section(T("Tags"), null, out _);
            tags.Children.Add(TagsRow([r]));
            host.Children.Add(tags);
        }

        // a lot or world in Downloads: the store items it uses
        if (m.FileType == "sims3pack" && _installedIds is { } have && Sims3Pack.Read(m.Path) is { } pack && pack.StoreDependencies.ToList() is { Count: > 0 } needs)
        {
            var missing = needs.Count(d => !have.Contains(d));
            host.Children.Add(missing == 0
                ? Note("circle-check", "ApexSuccessBrush", needs.Count == 1 ? T("The store item it uses is installed.") : T("All {0} store items it uses are installed.", needs.Count))
                : Warn(T("{0} of {1} store items it uses are not installed", missing, needs.Count), T("Get them from the store, or the lot shows without them.")));
        }

        // where it is, in full
        var where = Section(T("Where"), mine ? T("Move…") : null, out var move);
        if (move is not null) move.Click += (_, _) => MoveMenu([r]).ShowAt(move);
        where.Children.Add(new TextBlock { Text = ModRow.Place(m) + " › " + m.Name, FontSize = 13, TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush") });
        host.Children.Add(where);
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

    TextBox NotesBox(ModRow r, double height)
    {
        var notes = new TextBox
        {
            Text = ModNotes.Get(r.File.Rel)?.Notes ?? "", PlaceholderText = T("Notes: where it came from, what it changes"), AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap, MinHeight = height,
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(notes, T("Notes"));
        notes.LostFocus += (_, _) =>
        {
            try { ModNotes.SetNotes(r.File.Rel, notes.Text); r.Refresh(); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The notes were not saved"), ErrorText(e)); }
        };
        return notes;
    }

    // ---------- Replaces ----------

    UIElement ReplacesTab(ModRow r)
    {
        var d = r.Defaults;
        var s = new StackPanel { Spacing = 16 };
        var card = new StackPanel { Spacing = 10 };
        var share = d.Resources > 0 ? (double)d.Replaced / d.Resources : 0;
        var head = new Grid();
        head.Children.Add(new TextBlock { Text = ReplacesSummary(d), FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 48, 0) });
        head.Children.Add(new TextBlock { Text = $"{share:P0}", FontSize = 12, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = HorizontalAlignment.Right });
        card.Children.Add(head);
        var bar = new Grid { Height = 8, CornerRadius = new CornerRadius(4), Background = Res("ApexPillBrush") };
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(Math.Max(share, 0.001), GridUnitType.Star) });
        bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(Math.Max(1 - share, 0.001), GridUnitType.Star) });
        bar.Children.Add(new Border { Background = TagDot.BrushOf("#6CB2EA") });
        card.Children.Add(bar);
        card.Children.Add(new TextBlock
        {
            Text = d.Share >= 0.999 ? T("Everything in it changes something the game already has. Turned off, the game's originals come back.")
                : T("{0} of its {1} resources replace the game's own; the rest is new. Turned off, the game's originals come back.", d.Replaced, d.Resources),
            FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap,
        });
        s.Children.Add(new Border { Child = card, Padding = new Thickness(14), CornerRadius = new CornerRadius(10), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) });
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

    // ---------- a merge's mods ----------

    UIElement MergeCards(ModRow r, MergeRecord info)
    {
        var s = new StackPanel { Spacing = 10 };
        var explain = Note("lightbulb", "ApexAccentBrush", T("The game loads this as one file instead of {0}, so it starts faster. The {0} mods are kept as they were, and any of them can come back out.", info.Originals.Count));
        s.Children.Add(new Border { Child = explain, Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(10), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x17, 0x16, 0x2A)), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x2C, 0x2A, 0x4C)), BorderThickness = new Thickness(1) });
        var head = Section(T("Made from {0} mods", info.Originals.Count), Mine(r) ? T("Edit merge") : null, out var edit);
        if (edit is not null) edit.Click += (_, _) => Safe(EditMergeAsync(r, info));
        s.Children.Add(head);
        var i = 0;
        foreach (var o in info.Originals.OrderBy(o => o.Rel, StringComparer.OrdinalIgnoreCase))
        {
            var color = MergeColors[i++ % MergeColors.Length];
            var g = new Grid { ColumnSpacing = 12, Padding = new Thickness(12) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(6) });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(new Border { CornerRadius = new CornerRadius(3), Background = TagDot.BrushOf(color) });
            var text = new StackPanel { Spacing = 3, VerticalAlignment = VerticalAlignment.Center };
            var name = Path.GetFileNameWithoutExtension(o.Rel);
            text.Children.Add(new TextBlock { Text = name, FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
            var what = new TextBlock { Text = ModRow.SizeText(o.Size), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap };
            text.Children.Add(what);
            Grid.SetColumn(text, 1);
            g.Children.Add(text);
            // what this one replaces in the game, read from the copy kept at the merge
            var kept = Path.Combine(info.Dir, Path.GetRelativePath("Mods", o.Rel));
            if (_gameIndex is { } gi && File.Exists(kept))
                _ = Task.Run(() => DefaultReplacements.Analyze(gi, kept)).ContinueWith(t =>
                {
                    if (t.IsCompletedSuccessfully && t.Result.Replaced > 0)
                    {
                        what.Text = ReplacesSummary(t.Result) + " · " + ModRow.SizeText(o.Size);
                        what.Foreground = TagDot.BrushOf("#9CC7EE");
                    }
                }, TaskScheduler.FromCurrentSynchronizationContext());
            if (Mine(r))
            {
                var take = new Button { Content = T("Take out"), Height = 30, Padding = new Thickness(10, 0, 10, 0), FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
                var rel = o.Rel;
                take.Click += (_, _) => Safe(TakeOutAsync(r, [rel]));
                Grid.SetColumn(take, 2);
                g.Children.Add(take);
            }
            s.Children.Add(new Border { Child = g, CornerRadius = new CornerRadius(12), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) });
        }
        s.Children.Add(Note("circle-check", "ApexSuccessBrush", T("Checked when it was made on {0}: every resource read back identical.", info.Created.ToLocalTime().ToString("d"))));
        return s;
    }

    // rename it, untick mods to take them out, tick others to add them; the result is merged again and checked
    async Task EditMergeAsync(ModRow r, MergeRecord info)
    {
        var body = new StackPanel { Spacing = 14, Width = 440 };
        var name = new TextBox { Header = T("Name"), Text = r.Name };
        body.Children.Add(name);
        body.Children.Add(new TextBlock { Text = T("Mods in it · untick one to take it out"), FontSize = 12, Foreground = Res("ApexMutedBrush") });
        var boxes = new List<(CheckBox Box, MergeOriginal O)>();
        var adds = new List<(CheckBox Box, ModRow Row)>();
        var list = new StackPanel { Spacing = 4 };
        var after = new TextBlock { FontSize = 12, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap };
        void Update()
        {
            var stay = boxes.Count(b => b.Box.IsChecked == true);
            var added = adds.Count(a => a.Box.IsChecked == true);
            var keep = stay + added;
            after.Text = keep == 0 ? T("After saving: every mod is back on its own and the merge goes away.")
                : keep == 1 ? T("After saving: a merge of one is no merge, so every mod is back on its own.")
                : added == 0 && stay == info.Originals.Count ? T("After saving: the same {0} mods, in one file.", keep)
                : added == 0 ? T("After saving: {0} mods in one file, written again and checked. The others are back on their own, still on.", keep)
                : T("After saving: {0} mods in one file, {1} of them new, written again and checked.", keep, added);
        }
        foreach (var o in info.Originals.OrderBy(o => o.Rel, StringComparer.OrdinalIgnoreCase))
        {
            var cb = new CheckBox { IsChecked = true, Content = $"{Path.GetFileNameWithoutExtension(o.Rel)}   {ModRow.SizeText(o.Size)}" };
            cb.Checked += (_, _) => Update();
            cb.Unchecked += (_, _) => Update();
            boxes.Add((cb, o));
            list.Children.Add(cb);
        }
        body.Children.Add(new ScrollViewer { Content = list, MaxHeight = 240 });

        // other mods that can go in: packages of the player's that are on, found by name or folder
        var candidates = _modRows.Where(x => Mine(x) && x.File.Enabled && !x.File.Rel.Equals(r.File.Rel, StringComparison.OrdinalIgnoreCase))
            .OrderBy(x => x.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        var addPanel = new StackPanel { Spacing = 8, Visibility = Visibility.Collapsed };
        var search = new TextBox { PlaceholderText = T("Search mods") };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(search, T("Search mods"));
        var addList = new StackPanel { Spacing = 2 };
        foreach (var c in candidates)
        {
            var cb = new CheckBox { Content = $"{c.Name}   {c.Size}", Tag = (c.Name + " " + ModRow.Place(c.File)).ToLowerInvariant() };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(cb, c.Name);
            ToolTipService.SetToolTip(cb, ModRow.Place(c.File) + " › " + c.File.Name);
            cb.Checked += (_, _) => Update();
            cb.Unchecked += (_, _) => Update();
            adds.Add((cb, c));
            addList.Children.Add(cb);
        }
        search.TextChanged += (_, _) =>
        {
            var q = search.Text.Trim().ToLowerInvariant();
            foreach (var (cb, _) in adds) cb.Visibility = q.Length == 0 || ((string)cb.Tag).Contains(q) || cb.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        };
        addPanel.Children.Add(search);
        addPanel.Children.Add(new ScrollViewer { Content = addList, MaxHeight = 220 });
        var addLabel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
        addLabel.Children.Add(Lucide.Icon("plus", 15, Res("ApexAccentBrush")));
        addLabel.Children.Add(new TextBlock { Text = T("Add mods to this merge"), FontSize = 13 });
        var addButton = new Button { Content = addLabel, Height = 44, HorizontalAlignment = HorizontalAlignment.Stretch, CornerRadius = new CornerRadius(10), IsEnabled = candidates.Count > 0 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(addButton, T("Add mods to this merge"));
        addButton.Click += (_, _) => { addButton.Visibility = Visibility.Collapsed; addPanel.Visibility = Visibility.Visible; search.Focus(FocusState.Programmatic); };
        body.Children.Add(addButton);
        body.Children.Add(addPanel);
        body.Children.Add(new Border { Child = after, Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x17, 0x16, 0x2A)) });
        Update();
        var d = new ContentDialog { Title = T("Edit merge"), Content = body, PrimaryButtonText = T("Save and check"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        var outRels = boxes.Where(b => b.Box.IsChecked != true).Select(b => b.O.Rel).ToList();
        var addFiles = adds.Where(a => a.Box.IsChecked == true).Select(a => a.Row.File).ToList();
        var newName = name.Text.Trim();
        if (outRels.Count == info.Originals.Count && addFiles.Count == 0) { await SplitAsync(r); return; }
        if (outRels.Count > 0 || addFiles.Count > 0)
        {
            ModsInfo.IsOpen = true;
            ModsInfo.Severity = InfoBarSeverity.Informational;
            ModsInfo.Title = addFiles.Count > 0 ? T("Merging again with the new mods…") : T("Merging the rest again…");
            ModsInfo.Message = "";
            ModMerge.TakeOutResult res;
            try { res = await Task.Run(() => ModMerge.Edit(Docs, r.File, outRels, addFiles)); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The merge was not changed as asked"), ErrorText(e)); return; }
            ModsResult(res.Remerged is { } m ? T("{0} now holds {1} mods", m.Merged.Name, m.Mods) : T("Every mod is back on its own"), null);
            _modsLoaded = false;
            _folderSig = "";
            await LoadModsAsync();
        }
        if (newName.Length > 0 && newName != r.Name && _modRows.FirstOrDefault(x => x.File.Rel.Equals(r.File.Rel, StringComparison.OrdinalIgnoreCase)) is { } now)
            RenameMod(now, newName);
    }

    // one mod out of a merge, and what becomes of it: kept on, turned off, or to the Recycle Bin
    async Task TakeOutAsync(ModRow r, List<string> rels)
    {
        var info = ModMerge.Info(r.File);
        var name = Path.GetFileNameWithoutExtension(rels[0]);
        var body = new StackPanel { Spacing = 14, Width = 440 };
        var n = info?.Originals.Count ?? 0;
        body.Children.Add(new TextBlock
        {
            Text = n > 2 ? T("{0} goes back where it was. The other {1} are merged again into {2} and checked.", name, n - 1, r.Name)
                : T("{0} goes back where it was, and so does the other mod: a merge of one is no merge.", name),
            TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush"),
        });
        body.Children.Add(new TextBlock { Text = T("Then {0}", name), FontSize = 12, Foreground = Res("ApexMutedBrush") });
        var on = new RadioButton { GroupName = "after", IsChecked = true, Content = T("Stays on, as its own mod") };
        var off = new RadioButton { GroupName = "after", Content = T("Turned off: the game's own items come back") };
        var bin = new RadioButton { GroupName = "after", Content = T("Goes to the Recycle Bin") };
        body.Children.Add(on);
        body.Children.Add(off);
        body.Children.Add(bin);
        var d = new ContentDialog { Title = T("Take {0} out of {1}?", name, r.Name), Content = body, PrimaryButtonText = T("Take it out"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        var then = off.IsChecked == true ? 1 : bin.IsChecked == true ? 2 : 0;
        ModsInfo.IsOpen = true;
        ModsInfo.Severity = InfoBarSeverity.Informational;
        ModsInfo.Title = T("Taking {0} out of the merge…", name);
        ModsInfo.Message = "";
        ModMerge.TakeOutResult? res = null;
        try { res = await Task.Run(() => ModMerge.TakeOut(Docs, r.File, rels)); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The merge was not changed as asked"), ErrorText(e)); }
        _modsLoaded = false;
        _folderSig = "";
        await LoadModsAsync();
        if (res is null) return;
        var back = _modRows.FirstOrDefault(x => x.File.Rel.Equals(rels[0], StringComparison.OrdinalIgnoreCase));
        if (back is not null && then == 1) SetMods([back], false);
        else if (back is not null && then == 2) await ModsChange(() => ModLibrary.Remove(back.File), T("{0} moved to the Recycle Bin", name));
        else ModsResult(res.Remerged is { } m ? T("{0} is back on its own; {1} now holds {2} mods", name, m.Merged.Name, m.Mods) : T("{0} is back on its own; the other mod is back too", name), null);
    }

    // ---------- the footer ----------

    void DetailsFoot(ModRow r, MergeRecord? merge, bool mine)
    {
        var m = r.File;
        var f = ModDetailsFoot;
        f.Visibility = Visibility.Visible;
        f.ColumnDefinitions.Add(new ColumnDefinition());
        Button main;
        if (m.FileType == "library") main = MainButton(T("Show file"), () => ShowFile(m.Path));
        else if (m.FileType == "sims3pack")
        {
            var pack = Sims3Pack.Read(m.Path);
            if (pack is { Extractable: true }) main = MainButton(T("Install"), () => Safe(InstallFilesAsync([m.Path])));
            else
            {
                var exe = _game is { } g ? Path.Combine(g.Bin, "Sims3Launcher.exe") : null;
                main = MainButton(T("Install in the game's launcher"), () =>
                {
                    if (Games.IsRunning()) { SetInfo(ModsInfo, InfoBarSeverity.Warning, T("Close The Sims 3 first"), ""); return; }
                    try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(exe!) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe)! }); }
                    catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The game's launcher did not open"), ErrorText(e)); }
                });
                main.IsEnabled = exe is not null && File.Exists(exe);
            }
        }
        else if (m.Locked) main = MainButton(T("Open in Components"), () => Show(ComponentsStep));
        else if (merge is not null && mine) main = MainButton(T("Edit merge"), () => Safe(EditMergeAsync(r, merge)));
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
            var split = IconButton("scissors", T("Split it back into its {0} mods", merge.Originals.Count));
            split.Click += (_, _) => Safe(SplitAsync(r));
            Icon(split);
            var power = IconButton(m.Enabled ? "power-off" : "power", m.Enabled ? T("Turn off") : T("Turn on"));
            power.Click += (_, _) => SetMods([r], !r.File.Enabled);
            Icon(power);
        }
        if (m.FileType != "library")
        {
            var show = IconButton("folder-search", T("Show file"));
            show.Click += (_, _) => ShowFile(m.Path);
            Icon(show);
        }
        if (mine)
        {
            var remove = IconButton("trash-2", T("Remove to the Recycle Bin"), "ApexErrorBrush");
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
