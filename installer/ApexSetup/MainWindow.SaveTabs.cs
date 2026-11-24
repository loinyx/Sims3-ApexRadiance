using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using static ApexSetup.Strings;
using Path = System.IO.Path;
using Shape = Microsoft.UI.Xaml.Shapes;

namespace ApexSetup;

// The parts of a save's page (MainWindow.SavePage.cs): Memories, Space, Timeline and Faces.
public sealed partial class MainWindow
{
    // buttons that are pictures: no fill or border of their own in any state
    static void SpBare(FrameworkElement e)
    {
        var none = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        foreach (var state in new[] { "", "PointerOver", "Pressed", "Disabled", "Checked", "CheckedPointerOver", "CheckedPressed", "CheckedDisabled" })
        {
            e.Resources["ToggleButtonBackground" + state] = none;
            e.Resources["ToggleButtonBorderBrush" + state] = none;
        }
    }

    static ToggleButton SpPictureButton(UIElement content, string name)
    {
        var b = new ToggleButton
        {
            Content = content, Padding = new Thickness(0), MinWidth = 0, MinHeight = 0, BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(8),
            HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        AutomationProperties.SetName(b, name);
        return b;
    }

    // a picture that fades in once it is read
    Image SpPicture(SavePageState p, SaveItem item, int side, Func<bool>? wanted = null, bool grey = false)
    {
        var img = new Image { Stretch = Stretch.UniformToFill, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Opacity = 0 };
        if (AnimationsOn) img.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(180) };
        _ = Fill();
        async Task Fill() { if (await SpImage(item, side, p.Stop.Token, wanted, grey) is { } src) { img.Source = src; img.Opacity = 1; } }
        return img;
    }

    double SpScale => Content.XamlRoot?.RasterizationScale ?? 1;

    // ---------- Memories ----------

    const string SpStarShape = "M 12 3.5 l 2.6 5.6 6.1 0.7 -4.5 4.2 1.2 6 -5.4 -3 -5.4 3 1.2 -6 -4.5 -4.2 6.1 -0.7 z";
    static readonly Windows.UI.Color SpStarGold = Rgb(0xF0, 0xBE, 0x5A);

    void SpMemories(SavePageState p)
    {
        if (p.Memories.Count == 0)
        {
            p.Host.Children.Add(Muted(T("No memories in this save yet. The game takes these pictures by itself at moments of your Sims' lives.")));
            return;
        }
        var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var top = new WrapPanel { Spacing = 12 };
        top.Children.Add(chips);
        var say = Muted(T("The starred ones never leave in a clean."));
        say.Margin = new Thickness(0, 8, 0, 0);
        top.Children.Add(say);
        // (why a save grows, and where that stops)
        var why = new WrapPanel { Spacing = 6, Margin = new Thickness(0, 8, 0, 0) };
        why.Children.Add(Muted(T("The game takes these pictures by itself, and the save grows with them.")));
        why.Children.Add(Link(T("Turn that off in Game settings"), () => Show(GameSettingsScreen)));
        top.Children.Add(why);
        var page = new StackPanel { Spacing = 14 };
        page.Children.Add(top);
        var place = new Grid();
        page.Children.Add(place);
        p.Host.Children.Add(page);

        var picked = new TextBlock { FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        AutomationProperties.SetLiveSetting(picked, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        var starred = Muted("");
        var all = Link("", () => { });
        var gallery = Dims(Button(T("Take to the Gallery"), () => SpToGallery(p), accent: true));
        var export = Dims(Button(T("Save pictures"), () => SpExport(p)));
        var drop = Dims(Button(T("Take out of the save"), () => SpTakeOut(p), danger: true));
        void Count()
        {
            var n = p.Picked.Count;
            picked.Text = n == 0 ? T("Tap a memory to choose it") : n == 1 ? T("1 memory chosen") : T("{0} memories chosen", n);
            var kept = p.Memories.Count(i => p.Stars.Contains(i.Key));
            starred.Text = T("{0} starred", kept);
            var shown = SpShown(p);
            all.Visibility = shown.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            all.Content = shown.Count > 0 && shown.All(i => p.Picked.Contains(i.Key)) ? T("Choose none") : T("Choose all");
            gallery.IsEnabled = n > 0 && !DryRun;
            export.IsEnabled = n > 0;
            drop.IsEnabled = n > 0 && p.Blocked is null;
        }
        void Mosaic()
        {
            place.Children.Clear();
            p.Repaint = null;
            var shown = SpShown(p);
            if (shown.Count == 0) { place.Children.Add(Muted(T("No starred memories yet. The star on a memory keeps it."))); Count(); return; }
            var mosaic = new MosaicCanvas(shown.Count, 168, (i, size) => SpMemory(p, shown[i], i, shown.Count, size, Count));
            SpBare(mosaic);
            place.Children.Add(mosaic);
            Count();
        }
        all.Click += (_, _) =>
        {
            var shown = SpShown(p).Select(i => i.Key).ToList();
            if (shown.All(p.Picked.Contains)) p.Picked.Clear(); else p.Picked.UnionWith(shown);
            p.Repaint?.Invoke();
            Count();
        };

        var buttons = new List<(string Key, ToggleButton Button)>();
        void Chip(string key, string text)
        {
            var b = new ToggleButton { Content = text, MinHeight = 36, CornerRadius = new CornerRadius(18), Padding = new Thickness(16, 0, 16, 0), IsChecked = p.Filter == key };
            b.Click += (_, _) =>
            {
                p.Filter = key;
                foreach (var (k, other) in buttons) other.IsChecked = k == key;
                Mosaic();
            };
            buttons.Add((key, b));
            chips.Children.Add(b);
        }
        Chip("all", T("All of them"));
        Chip("stars", T("Starred"));
        // (only a save that has them says so, once they were compared)
        p.ShowRepeated = () => { if (p.Repeated is { Count: > 0 } && buttons.Count == 2) Chip("repeated", T("Repeated")); };
        p.ShowRepeated();
        if (p.Filter == "repeated" && buttons.Count == 2) { p.Filter = "all"; buttons[0].Button.IsChecked = true; }

        Mosaic();

        // the foot: what is chosen and what to do with it, in sight however far the mosaic goes
        var foot = new Grid { ColumnSpacing = 12, RowSpacing = 8 };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        foot.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var words = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, VerticalAlignment = VerticalAlignment.Center };
        foreach (var e in new FrameworkElement[] { picked, starred, all }) { e.VerticalAlignment = VerticalAlignment.Center; words.Children.Add(e); }
        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var b in new[] { gallery, export, drop }) { b.Height = 40; acts.Children.Add(b); }
        foot.Children.Add(words);
        foot.Children.Add(acts);
        foot.SizeChanged += (_, e) =>
        {
            var narrow = e.NewSize.Width < 880;
            Grid.SetColumn(acts, narrow ? 0 : 1);
            Grid.SetColumnSpan(acts, narrow ? 2 : 1);
            Grid.SetRow(acts, narrow ? 1 : 0);
            Grid.SetColumnSpan(words, narrow ? 2 : 1);
        };
        p.Bar.Child = foot;
        p.Bar.Visibility = Visibility.Visible;
    }

    static List<SaveItem> SpShown(SavePageState p) => p.Filter switch
    {
        "stars" => p.Memories.Where(i => p.Stars.Contains(i.Key)).ToList(),
        "repeated" => p.Repeated ?? [],
        _ => p.Memories.ToList(),
    };

    FrameworkElement SpMemory(SavePageState p, SaveItem item, int index, int total, double size, Action changed)
    {
        var key = item.Key;
        var name = T("Memory {0}", index + 1);
        var cell = new Grid();
        var gone = false; // (scrolled past before its picture's turn came)
        var face = new Border { CornerRadius = new CornerRadius(8), Background = Res("ApexPillBrush"), Child = SpPicture(p, item, (int)(size * SpScale), () => !gone) };
        var pick = SpPictureButton(face, name);
        pick.TabIndex = index * 2;
        AutomationProperties.SetPositionInSet(pick, index + 1);
        AutomationProperties.SetSizeOfSet(pick, total);
        var ring = new Border { CornerRadius = new CornerRadius(11), BorderBrush = Res("ApexAccentBrush"), Child = pick };
        void Paint()
        {
            var on = p.Picked.Contains(key);
            pick.IsChecked = on;
            ring.BorderThickness = new Thickness(on ? 3 : 0);
            ring.Padding = new Thickness(on ? 3 : 0);
        }
        pick.Click += (_, _) => { if (!p.Picked.Remove(key)) p.Picked.Add(key); Paint(); changed(); };
        Paint();
        p.Repaint += Paint;
        cell.Unloaded += (_, _) => { gone = !cell.IsLoaded; if (gone) p.Repaint -= Paint; };
        cell.Children.Add(ring);

        var shape = new Shape.Path { Data = (Geometry)XamlBindingHelper.ConvertValue(typeof(Geometry), SpStarShape), Width = 17, Height = 17, Stretch = Stretch.Uniform, StrokeThickness = 1.5, StrokeLineJoin = PenLineJoin.Round };
        var star = SpPictureButton(new Border { Width = 30, Height = 30, CornerRadius = new CornerRadius(15), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x99, 10, 11, 12)), Child = shape,
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center }, T("Keep memory {0}", index + 1));
        (star.Width, star.Height, star.TabIndex) = (44, 44, index * 2 + 1);
        (star.HorizontalAlignment, star.VerticalAlignment, star.Margin) = (HorizontalAlignment.Right, VerticalAlignment.Top, new Thickness(0, 4, 4, 0));
        ToolTipService.SetToolTip(star, T("Starred memories never leave in a clean"));
        void Shine()
        {
            var on = p.Stars.Contains(key);
            star.IsChecked = on;
            shape.Fill = on ? new SolidColorBrush(SpStarGold) : null;
            shape.Stroke = new SolidColorBrush(on ? SpStarGold : Microsoft.UI.Colors.White);
        }
        star.Click += (_, _) =>
        {
            var on = !p.Stars.Contains(key);
            try
            {
                SaveStars.Set(p.Save.Name, key, on);
                if (on) p.Stars.Add(key); else p.Stars.Remove(key);
                p.Plan = null; // (what a clean would free counts the stars)
            }
            catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException) { SpError(e); }
            Shine();
            changed();
        };
        Shine();
        cell.Children.Add(star);
        if (AnimationsOn) Lift(cell);
        return cell;
    }

    static IEnumerable<SaveItem> SpCounted(List<SaveItem> items, Action<string, double, bool> say)
    {
        for (var i = 0; i < items.Count; i++)
        {
            say(T("{0} of {1}", i + 1, items.Count), i / (double)items.Count, true);
            yield return items[i];
        }
    }

    List<SaveItem> SpPicked(SavePageState p) => p.Memories.Where(i => p.Picked.Contains(i.Key)).ToList();

    async Task SpToGallery(SavePageState p)
    {
        var items = SpPicked(p);
        if (await SpWork(T("Taking the memories to the Gallery"), (say, ct) => SaveContents.ToGallery(p.Docs, p.Save, SpCounted(items, say), ct)) is not { } made) return;
        SpGalleryAgain();
        SpSay(InfoBarSeverity.Success, made.Count == 1 ? T("1 memory in the Gallery.") : T("{0} memories in the Gallery.", made.Count), "", T("Open the Gallery"), () => Show(GalleryScreen));
    }

    async Task SpExport(SavePageState p)
    {
        var items = SpPicked(p);
        try
        {
            var picker = new Microsoft.Windows.Storage.Pickers.FolderPicker(AppWindow.Id);
            if (await picker.PickSingleFolderAsync() is not { } f || _sp != p) return;
            if (await SpWork(T("Saving the pictures"), (say, ct) => SaveContents.Export(p.Save.Name, SpCounted(items, say), f.Path, ct)) is not { } made) return;
            SpSay(InfoBarSeverity.Success, made.Count == 1 ? T("1 picture saved.") : T("{0} pictures saved.", made.Count), "", T("Open folder"), () => OpenFolder(f.Path));
        }
        catch (Exception e) { SpError(e); }
    }

    async Task SpTakeOut(SavePageState p)
    {
        var keys = p.Picked.Where(k => !p.Stars.Contains(k)).ToList();
        if (keys.Count == 0) { SpSay(InfoBarSeverity.Informational, T("The starred ones stay"), T("Take the star off a memory to take it out of the save.")); return; }
        if (await SpClean(p, CleanParts.None, keys, T("Taking the memories out")) is not { } r) return;
        OpenSave(r.Save, p.Tab);
        SpSay(InfoBarSeverity.Success, r.Removed == 1 ? T("Memory taken out. The copy from before is kept.") : T("Memories taken out. The copy from before is kept."), "", T("Undo"), () => _ = SpUndo(r), stays: true);
    }

    // ---------- Space ----------

    Action? _spAside; // the panel beside the tiles, said again

    void SpSpace(SavePageState p)
    {
        if (p.Plan is not { } plan)
        {
            p.Host.Children.Add(SpRing());
            if (p.Points is not null) _ = SpPlan(p); // (the first one is on its way with the rest)
            return;
        }
        CleanGain Gain(CleanParts part) => plan.Gains.GetValueOrDefault(part) ?? new(0, 0);
        var (mem, rep, faces, thumbs) = (Gain(CleanParts.Memories), Gain(CleanParts.RepeatedMemories), Gain(CleanParts.Portraits), Gain(CleanParts.Thumbnails));
        var many = mem.Bytes > 50L << 20;
        string N(int n) => n.ToString("N0", LangCulture());
        var lots = p.Contents.Sample(SaveKind.LotThumbnail, 4);
        var all = new List<(CleanParts Part, string Name, string Info, long Bytes, IReadOnlyList<SaveItem> Pics, int Cols, int Rows)>
        {
            (CleanParts.None, T("World, houses and Sims"), T("It is the save itself. It stays"), plan.Before - mem.Bytes - rep.Bytes - faces.Bytes - thumbs.Bytes, lots, 4, 1),
            (CleanParts.Memories, T("Memories"), mem.Count == 1 ? T("1 picture the game took") : T("{0} pictures the game took", N(mem.Count)), mem.Bytes, p.Contents.Sample(SaveKind.Memory, many ? 32 : 6), many ? 8 : 3, many ? 4 : 2),
            (CleanParts.RepeatedMemories, T("Repeated memories"), rep.Count == 1 ? T("1 copy of the same picture") : T("{0} copies of the same picture", N(rep.Count)), rep.Bytes, p.Repeated is { Count: > 0 } same ? same.Take(6).ToList() : [], 3, 2),
            (CleanParts.Portraits, T("Portraits"), T("{0}, the game makes them again", N(faces.Count)), faces.Bytes, p.Contents.Sample(SaveKind.Portrait, 8), 4, 2),
            (CleanParts.Thumbnails, T("Thumbnails"), T("{0}, the game makes them again", N(thumbs.Count)), thumbs.Bytes, lots.Take(1).ToList(), 1, 1),
        };
        all = all.Where(t => t.Part == CleanParts.None || t.Bytes > 0).OrderByDescending(t => t.Bytes).ToList();
        p.Marked &= all.Aggregate(CleanParts.None, (a, t) => a | t.Part); // (a part that has nothing is not marked)

        var page = new Grid { ColumnSpacing = 20, RowSpacing = 20 };
        page.ColumnDefinitions.Add(new ColumnDefinition());
        page.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        page.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var canvas = new Canvas { VerticalAlignment = VerticalAlignment.Top };
        SpBare(canvas);
        foreach (var t in all) canvas.Children.Add(SpTile(p, t.Part, t.Name, t.Info, t.Bytes, t.Pics, t.Cols, t.Rows));
        page.Children.Add(canvas);
        void Lay()
        {
            if (canvas.ActualWidth <= 0) return;
            var tiles = SaveLayout.Tiles(all.Select(t => (double)t.Bytes).ToList(), canvas.ActualWidth, Math.Max(490, Math.Min(760, SavePage.ActualHeight - 330)), 8, 138, 130);
            for (var i = 0; i < tiles.Count; i++)
            {
                var e = (FrameworkElement)canvas.Children[i];
                (e.Width, e.Height) = (tiles[i].W, tiles[i].H);
                Canvas.SetLeft(e, tiles[i].X);
                Canvas.SetTop(e, tiles[i].Y);
            }
            canvas.Height = tiles.Max(t => t.Y + t.H);
        }
        canvas.SizeChanged += (_, e) => { if (e.NewSize.Width != e.PreviousSize.Width) Lay(); };

        var panel = new StackPanel { Spacing = 16 };
        var aside = new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(24), Child = panel, Width = 340 };
        AutomationProperties.SetName(aside, T("Cleaning"));
        page.Children.Add(aside);
        page.SizeChanged += (_, e) =>
        {
            // a narrow window: the panel under the tiles
            var narrow = e.NewSize.Width < 860;
            aside.Width = narrow ? double.NaN : 340;
            Grid.SetColumn(aside, narrow ? 0 : 1);
            Grid.SetColumnSpan(aside, narrow ? 2 : 1);
            Grid.SetRow(aside, narrow ? 1 : 0);
            Grid.SetColumnSpan(canvas, narrow ? 2 : 1);
        };
        // the words and numbers are set again on every mark; the button stays the same one (the keyboard may be on it)
        var label = Muted("");
        var number = new TextBlock { FontFamily = new FontFamily("Segoe UI Variable Display"), FontSize = 54, FontWeight = FontWeights.SemiBold, LineHeight = 58 };
        var less = new TextBlock { FontSize = 14, Foreground = Res("ApexSuccessBrush"), MinHeight = 20 };
        AutomationProperties.SetLiveSetting(less, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        panel.Children.Add(Col(label, number, less));
        var bar = new ProgressBar { Maximum = Math.Max(1, plan.Before), MinHeight = 8 };
        AutomationProperties.SetName(bar, T("Size after cleaning"));
        panel.Children.Add(bar);
        var notes = new StackPanel { Spacing = 12 };
        panel.Children.Add(notes);
        var clean = MainButton("", () => _ = SpCleanMarked(p));
        clean.Height = 48;
        AutomationProperties.SetAutomationId(clean, "SaveClean");
        panel.Children.Add(clean);
        _spAside = () =>
        {
            var gain = new[] { CleanParts.Memories, CleanParts.RepeatedMemories, CleanParts.Portraits, CleanParts.Thumbnails }.Where(x => p.Marked.HasFlag(x)).Sum(x => Gain(x).Bytes);
            // (the plan of nothing chosen already gives back the room nothing uses)
            var after = p.Marked == CleanParts.None ? plan.Before : plan.After - gain;
            label.Text = p.Marked == CleanParts.None ? T("Size of the save") : T("After cleaning");
            number.Text = SpSize(after);
            less.Text = p.Marked == CleanParts.None ? "" : T("{0} less", SpSize(plan.Before - after));
            bar.Value = after;
            notes.Children.Clear();
            if ((p.Marked & (CleanParts.Portraits | CleanParts.Thumbnails)) != 0) notes.Children.Add(Note("rotate-ccw", "ApexAccentTextBrush", T("Portraits and thumbnails come back as you play. Edit Town opens slower the first time.")));
            if (p.Marked.HasFlag(CleanParts.RepeatedMemories)) notes.Children.Add(Note("rotate-ccw", "ApexAccentTextBrush", T("Each repeated picture stays once. The other memory has no picture.")));
            if (p.Marked.HasFlag(CleanParts.Memories)) notes.Children.Add(Note("triangle-alert", "ApexWarningBrush", T("The memories lose their picture for good. The starred ones stay.")));
            if (p.Marked != CleanParts.None) notes.Children.Add(Note("shield-check", "ApexSuccessBrush", T("A copy is made first, and the save is checked afterwards.")));
            else
            {
                notes.Children.Add(Note("circle-check", "ApexSuccessBrush", T("Tap what you want to take out. Nothing changes until you clean.")));
                if (mem.Count + rep.Count > 0)
                {
                    notes.Children.Add(Note("image", "ApexAccentTextBrush", T("The game takes the memories by itself. You can turn that off in Game settings.")));
                    var open = Link(T("Open Game settings"), () => Show(GameSettingsScreen));
                    (open.FontSize, open.Margin) = (12, new Thickness(23, -8, 0, 0));
                    notes.Children.Add(open);
                }
            }
            if (p.Blocked is { } why) notes.Children.Add(Note("info", "ApexWarningBrush", why));
            else if (plan.Refusal is { } no) notes.Children.Add(Note("info", "ApexWarningBrush", T(no.Key, TArgs(no.Args))));
            clean.Content = p.Marked == CleanParts.None ? T("Choose what to clean") : T("Clean {0}", SpSize(plan.Before - after));
            clean.IsEnabled = p.Marked != CleanParts.None && p.Blocked is null && plan.Refusal is null;
        };
        _spAside();
        p.Host.Children.Add(page);
    }

    async Task SpPlan(SavePageState p)
    {
        try
        {
            var plan = await Task.Run(() => SaveClean.Plan(p.Docs, p.Save, CleanParts.None, null, p.Contents, p.Stop.Token), p.Stop.Token);
            if (_sp != p) return;
            p.Plan = plan;
            if (p.Tab == "space") SpFill(p);
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { if (_sp == p) SpError(e); else App.Log(e); }
    }

    // one tile: its pictures under a shade, its size, its name and what it is; a tap marks it to go (the save itself has none)
    FrameworkElement SpTile(SavePageState p, CleanParts part, string name, string info, long bytes, IReadOnlyList<SaveItem> pics, int cols, int rows)
    {
        var white = new SolidColorBrush(Microsoft.UI.Colors.White);
        var g = new Grid { Background = new SolidColorBrush(Rgb(0x16, 0x16, 0x16)) };
        Grid Pictures(bool grey)
        {
            var m = new Grid { ColumnSpacing = 2, RowSpacing = 2 };
            if (AnimationsOn) m.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(260) };
            for (var c = 0; c < cols; c++) m.ColumnDefinitions.Add(new ColumnDefinition());
            for (var r = 0; r < rows; r++) m.RowDefinitions.Add(new RowDefinition());
            for (var i = 0; pics.Count > 0 && i < cols * rows; i++)
            {
                var img = SpPicture(p, pics[i % pics.Count], (int)(128 * SpScale), grey: grey);
                Grid.SetColumn(img, i % cols);
                Grid.SetRow(img, i / cols);
                m.Children.Add(img);
            }
            return m;
        }
        // (marked to go, the pictures lose their colour: the same ones in grey, made the first time it is marked)
        var (mosaic, faded) = (Pictures(false), (Grid?)null);
        g.Children.Add(mosaic);
        g.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(Windows.UI.Color.FromArgb(255, 8, 9, 10), [(0.04, 0xF2), (0.4, 0x80), (0.72, 0)]) });
        var label = new TextBlock { Text = name, FontWeight = FontWeights.SemiBold, Foreground = white, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis };
        var words = new StackPanel { VerticalAlignment = VerticalAlignment.Bottom, Padding = new Thickness(16, 0, 12, 14) };
        words.Children.Add(new TextBlock { Text = SpSize(bytes), FontSize = bytes > 18L << 20 ? 26 : 18, FontWeight = FontWeights.SemiBold, Foreground = white });
        words.Children.Add(label);
        words.Children.Add(new TextBlock { Text = info, FontSize = 12, Foreground = new SolidColorBrush(Rgb(0xC8, 0xC8, 0xC8)), TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis });
        g.Children.Add(words);
        var say = name + ", " + SpSize(bytes) + ". " + info;
        if (part == CleanParts.None)
        {
            var still = new Border { Child = g, CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(2), BorderBrush = Res("ApexCardBorderBrush") };
            AutomationProperties.SetName(still, say);
            return still;
        }
        var tick = Lucide.Icon("check", 14, Res("TextOnAccentFillColorPrimaryBrush"), 3.2);
        var dot = new Border { Width = 26, Height = 26, CornerRadius = new CornerRadius(13), BorderThickness = new Thickness(2), Child = tick, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 12, 12, 0) };
        (tick.HorizontalAlignment, tick.VerticalAlignment) = (HorizontalAlignment.Center, VerticalAlignment.Center);
        g.Children.Add(dot);
        // (a light over the whole tile, under the pointer)
        var shine = new Border { IsHitTestVisible = false, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x10, 255, 255, 255)), Opacity = 0 };
        if (AnimationsOn) shine.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(140) };
        g.Children.Add(shine);
        var edge = new Border { Child = g, CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(2) };
        var b = SpPictureButton(edge, say);
        b.CornerRadius = new CornerRadius(12);
        b.PointerEntered += (_, _) => shine.Opacity = 1;
        b.PointerExited += (_, _) => shine.Opacity = 0;
        void Paint()
        {
            var on = p.Marked.HasFlag(part);
            b.IsChecked = on;
            edge.BorderBrush = Res(on ? "ApexAccentBrush" : "ApexCardBorderBrush");
            if (on && faded is null) g.Children.Insert(1, faded = Pictures(true));
            mosaic.Opacity = on ? 0 : 1;
            if (faded is not null) faded.Opacity = on ? 0.5 : 0;
            label.TextDecorations = on ? Windows.UI.Text.TextDecorations.Strikethrough : Windows.UI.Text.TextDecorations.None;
            dot.BorderBrush = on ? Res("ApexAccentBrush") : new SolidColorBrush(Windows.UI.Color.FromArgb(0xC0, 255, 255, 255));
            dot.Background = on ? Res("ApexAccentBrush") : new SolidColorBrush(Windows.UI.Color.FromArgb(0x73, 10, 11, 12));
            tick.Opacity = on ? 1 : 0;
        }
        b.Click += (_, _) => { p.Marked ^= part; Paint(); _spAside?.Invoke(); };
        Paint();
        return b;
    }

    sealed class SpProgress(Action<CleanProgress> say) : IProgress<CleanProgress> { public void Report(CleanProgress value) => say(value); }

    // the plan made again from the files as they are now, then run; null when it did not happen (the page says why)
    async Task<CleanResult?> SpClean(SavePageState p, CleanParts parts, List<string>? keys, string title) =>
        await SpWork(title, (say, ct) =>
        {
            var plan = SaveClean.Plan(p.Docs, p.Save, parts, keys, null, ct);
            return SaveClean.Run(plan, new SpProgress(c =>
            {
                var part = c.Total > 0 ? (double)c.Done / c.Total : 0;
                if (c.Step == CleanStep.Copy) say(T("Making a copy of the save"), -1, true);
                else if (c.Step == CleanStep.Write) say(T("Writing the smaller save"), 0.9 * part, true);
                else if (c.Step == CleanStep.Verify) say(T("Checking it against the save"), 0.9 * part, true);
                else say(T("Putting it in place"), 0.9 + 0.1 * part, false);
            }), ct);
        });

    async Task SpCleanMarked(SavePageState p)
    {
        if (p.Marked == CleanParts.None || await SpClean(p, p.Marked, null, T("Cleaning the save")) is not { } r) return;
        OpenSave(r.Save, "space");
        var top = new Grid { Height = 200 };
        if (p.Cover is not null) top.Children.Add(new Image { Source = p.Cover, Stretch = Stretch.UniformToFill, VerticalAlignment = VerticalAlignment.Center, Opacity = 0.5 });
        if (p.Family is not null) top.Children.Add(new Image { Source = p.Family, Height = 182, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Bottom });
        var body = new StackPanel { Spacing = 12, Width = 480 };
        if (top.Children.Count > 0) body.Children.Add(new Border { Child = top, CornerRadius = new CornerRadius(12), Background = new SolidColorBrush(Rgb(0x0F, 0x1A, 0x14)) });
        body.Children.Add(new TextBlock { Text = T("{0} became {1}", SpSize(r.Before), SpSize(r.After)), FontFamily = new FontFamily("Segoe UI Variable Display"), FontSize = 30, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap });
        var text = Body(T("Everything was checked: the family, the houses and the relationships are the same. The copy from before is kept."));
        text.TextAlignment = TextAlignment.Center;
        body.Children.Add(text);
        var d = new ContentDialog { Content = body, PrimaryButtonText = T("Done"), SecondaryButtonText = T("Undo"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        AutomationProperties.SetName(d, T("The save is clean"));
        if (await ShowDialog(d) == ContentDialogResult.Secondary) await SpUndo(r);
    }

    async Task SpUndo(CleanResult r)
    {
        var (docs, tab) = (Docs, _sp?.Tab ?? "space");
        if (await SpWork(T("Putting the save back"), (say, ct) => { say(T("Copying the save and checking the copy"), -1, true); return SaveClean.Undo(docs, r, ct); }) is not { } s) return;
        OpenSave(s, tab);
        SpSay(InfoBarSeverity.Success, T("The save is as it was before."));
    }

    // ---------- Timeline ----------

    void SpTimeline(SavePageState p)
    {
        if (p.Points is not { Count: > 0 } points) { p.Host.Children.Add(SpRing()); return; }
        var culture = LangCulture();
        var blue = new SolidColorBrush(Pick(0xFF8EC2F5, 0xFF2B6CB0));
        var accent = Res("ApexAccentBrush");
        string When(SavePoint x) => x.Kind == CopyKind.Now ? T("Today") : x.When.ToString(x.When.Year == DateTime.Now.Year ? "d MMMM, HH:mm" : "d MMMM yyyy, HH:mm", culture);
        string What(SavePoint x) => x.Kind switch
        {
            CopyKind.Now => T("The save as it is now, {0}", SpSize(x.Bytes)),
            CopyKind.Game => T("Copy by the game, {0}", SpSize(x.Bytes)),
            _ => x.Note == SaveCopies.BeforeClean ? T("Before cleaning, {0}", SpSize(x.Bytes)) : x.Note == SaveCopies.BeforeRestore ? T("Before going back, {0}", SpSize(x.Bytes)) : T("Copy by Apex, {0}", SpSize(x.Bytes)),
        };

        var page = new StackPanel { Spacing = 12 };
        var head = new WrapPanel { Spacing = 18 };
        var h = Section(T("Go back to any point"));
        h.Margin = new Thickness(0);
        head.Children.Add(h);
        void Key(string text, Brush edge, Brush? fill)
        {
            var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 2, 0, 0) };
            s.Children.Add(new Ellipse { Width = 11, Height = 11, Stroke = edge, StrokeThickness = 2, Fill = fill, VerticalAlignment = VerticalAlignment.Center });
            s.Children.Add(Muted(text));
            head.Children.Add(s);
        }
        Key(T("Copy by Apex"), accent, accent);
        Key(T("Copy by the game"), blue, null);
        page.Children.Add(head);

        var plot = new Canvas { Height = 300 };
        SpBare(plot);
        page.Children.Add(plot);
        var area = new Shape.Path { Fill = new SolidColorBrush(A(((SolidColorBrush)accent).Color, 0x17)), IsHitTestVisible = false };
        var line = new Shape.Path { Stroke = accent, StrokeThickness = 2.5, IsHitTestVisible = false };
        plot.Children.Add(area);
        plot.Children.Add(line);
        var card = new Grid { ColumnSpacing = 18, RowSpacing = 12 };
        page.Children.Add(new Border { Child = card, Padding = new Thickness(18, 16, 18, 16), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexRaisedBrush") });
        page.Children.Add(Muted(T("When you go back, today's save becomes one more point here. Nothing is lost.")));
        p.Host.Children.Add(new Border { Style = (Style)Application.Current.Resources["ApexCard"], Padding = new Thickness(24, 20, 24, 18), Child = page });

        // the chosen point: a picture, when it is from, and the way back to it
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) card.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var photo = new Grid { Width = 92, Height = 92 };
        card.Children.Add(new Border { Child = photo, CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(4), BorderBrush = new SolidColorBrush(Rgb(0xF1, 0xEF, 0xEA)), Background = Res("ApexPillBrush"), Visibility = p.Memories.Count > 0 ? Visibility.Visible : Visibility.Collapsed });
        var (title, text) = (new TextBlock { FontSize = 18, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap }, Body(""));
        var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Children = { title, text } };
        AutomationProperties.SetLiveSetting(title, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        Grid.SetColumn(words, 1);
        card.Children.Add(words);
        var act = new Button { Height = 48, Padding = new Thickness(22, 0, 22, 0), CornerRadius = new CornerRadius(8), Style = (Style)Application.Current.Resources["AccentButtonStyle"], VerticalAlignment = VerticalAlignment.Center };
        Dims(act);
        act.Click += async (_, _) => { if (points[p.Point].Kind == CopyKind.Now) await PlayAsync(p.Save.Name, update: _settings.InstallUpdatesOnPlay); else await SpRestore(p, points[p.Point], When(points[p.Point]), undo: true); };
        Grid.SetColumn(act, 2);
        card.Children.Add(act);
        var dots = new List<(ToggleButton Button, Ellipse Dot, Ellipse Halo)>();
        void Choose(int i)
        {
            p.Point = i;
            for (var k = 0; k < dots.Count; k++)
            {
                dots[k].Button.IsChecked = k == i;
                dots[k].Dot.Width = dots[k].Dot.Height = k == i ? 22 : 16;
                dots[k].Halo.Opacity = k == i ? 1 : 0;
                Canvas.SetZIndex(dots[k].Button, k == i ? 2 : 1);
            }
            var x = points[i];
            (title.Text, text.Text) = (When(x), What(x));
            act.Content = x.Kind == CopyKind.Now ? T("Play") : T("Go back to here");
            act.IsEnabled = x.Kind == CopyKind.Now ? !_busy && !Maintenance && !_running && !p.Stored : p.Blocked is null;
            photo.Children.Clear();
            // (one of the save's memories, earlier ones for earlier points)
            if (p.Memories.Count > 0) photo.Children.Add(SpPicture(p, p.Memories[points.Count > 1 ? (int)((long)i * (p.Memories.Count - 1) / (points.Count - 1)) : p.Memories.Count - 1], (int)(96 * SpScale)));
        }
        for (var i = 0; i < points.Count; i++)
        {
            var x = points[i];
            var at = i;
            var dot = new Ellipse { StrokeThickness = 3, Stroke = x.Kind == CopyKind.Game ? blue : accent, Fill = x.Kind == CopyKind.Game ? Res("ApexCardBrush") : x.Kind == CopyKind.Now ? Res("TextFillColorPrimaryBrush") : accent,
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            var halo = new Ellipse { Width = 38, Height = 38, Fill = Res("ApexHairlineBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            var b = SpPictureButton(new Grid { Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), Children = { halo, dot } }, When(x) + ". " + What(x));
            (b.Width, b.Height, b.CornerRadius) = (44, 44, new CornerRadius(22));
            ToolTipService.SetToolTip(b, When(x) + "\n" + What(x));
            b.Click += (_, _) => Choose(at);
            dots.Add((b, dot, halo));
            plot.Children.Add(b);
        }
        Choose(Math.Clamp(p.Point, 0, points.Count - 1));

        var labels = new List<TextBlock>();
        plot.SizeChanged += (_, e) =>
        {
            const double edge = 24, top = 40, bottom = 214, floor = 262;
            var width = e.NewSize.Width - edge * 2;
            if (width < 60) return;
            // the points by their day, kept apart so each can be picked; the sizes in between follow them. The save as it is
            // ends the line, also when a copy of it was made after it was played.
            var times = points.Select(x => (double)x.When.Ticks).ToArray();
            times[^1] = times.Max();
            var history = p.History.Where(x => x.When.Ticks < times[^1]).OrderBy(x => x.When).ToList();
            var start = Math.Min(times[0], history.Count > 0 ? history[0].When.Ticks : double.MaxValue);
            var span = times[^1] - start;
            var xs = SaveLayout.Spread(times.Select(t => span > 0 ? (t - start) / span * width : width).ToList(), width, 30).Select(x => x + edge).ToArray();
            double[] from = [start, .. times], to = [Math.Min(edge, xs[0]), .. xs];
            var nodes = history.Select(x => (X: SaveLayout.Between(from, to, x.When.Ticks), x.Bytes)).Concat(points.Select((x, i) => (X: xs[i], x.Bytes))).OrderBy(n => n.X).ToList();
            if (nodes.Count == 1) nodes.Insert(0, (edge, nodes[0].Bytes)); // (no copy yet: a level line up to today)
            long lo = nodes.Min(n => n.Bytes), hi = nodes.Max(n => n.Bytes);
            double Y(long bytes) => hi > lo ? bottom - (bytes - lo) / (double)(hi - lo) * (bottom - top) : (top + bottom) / 2;
            PathFigure Curve()
            {
                var f = new PathFigure { StartPoint = new Point(nodes[0].X, Y(nodes[0].Bytes)), Segments = new PathSegmentCollection() };
                for (var i = 1; i < nodes.Count; i++)
                {
                    var mid = (nodes[i - 1].X + nodes[i].X) / 2;
                    f.Segments.Add(new BezierSegment { Point1 = new Point(mid, Y(nodes[i - 1].Bytes)), Point2 = new Point(mid, Y(nodes[i].Bytes)), Point3 = new Point(nodes[i].X, Y(nodes[i].Bytes)) });
                }
                return f;
            }
            line.Data = new PathGeometry { Figures = new PathFigureCollection { Curve() } };
            var under = Curve();
            under.IsClosed = true;
            under.Segments.Add(new LineSegment { Point = new Point(nodes[^1].X, floor) });
            under.Segments.Add(new LineSegment { Point = new Point(nodes[0].X, floor) });
            area.Data = new PathGeometry { Figures = new PathFigureCollection { under } };
            foreach (var l in labels) plot.Children.Remove(l);
            labels.Clear();
            // the days under the points that have room for theirs, today's always
            var free = e.NewSize.Width;
            for (var i = points.Count - 1; i >= 0; i--)
            {
                Canvas.SetLeft(dots[i].Button, xs[i] - 22);
                Canvas.SetTop(dots[i].Button, Y(points[i].Bytes) - 22);
                var l = Muted(points[i].Kind == CopyKind.Now ? T("Today") : points[i].When.ToString("d MMM", culture));
                l.Measure(new Size(200, 40));
                var left = Math.Clamp(xs[i] - l.DesiredSize.Width / 2, 0, Math.Max(0, e.NewSize.Width - l.DesiredSize.Width));
                if (left + l.DesiredSize.Width > free) continue;
                free = left - 18;
                Canvas.SetLeft(l, left);
                Canvas.SetTop(l, floor + 10);
                labels.Add(l);
                plot.Children.Add(l);
            }
        };
    }

    async Task SpRestore(SavePageState p, SavePoint to, string when, bool undo)
    {
        if (await SpWork(T("Going back to {0}", when), (say, ct) =>
        {
            say(T("Copying the save and checking the copy"), -1, true);
            var s = SaveCopies.Restore(p.Docs, p.Save, to, ct);
            return Tuple.Create(s, SaveCopies.Timeline(p.Docs, s).LastOrDefault(x => x.Note == SaveCopies.BeforeRestore));
        }) is not { } done) return;
        OpenSave(done.Item1, "timeline");
        SpSay(InfoBarSeverity.Success, T("The save went back to {0}.", when), "", undo && done.Item2 is not null ? T("Undo") : null,
            () => { if (_sp is { } now && done.Item2 is { } before) _ = SpRestore(now, before, T("how it was before"), undo: false); });
    }

    // ---------- Faces ----------

    void SpFaces(SavePageState p)
    {
        var faces = p.Contents.Sample(SaveKind.Portrait, 40);
        if (faces.Count == 0) { p.Host.Children.Add(Muted(T("No portraits in this save. The game makes them as you play."))); return; }
        var page = new StackPanel { Spacing = 16 };
        page.Children.Add(Body(T("Some of the Sims this save knows. The game makes these portraits again by itself.")));
        var grid = new CardGrid { MinItemWidth = 130, Spacing = 14, Aspect = 1 };
        Windows.UI.Color[] washes = [Pick(0xFF2B2A36, 0xFFE4E2F2), Pick(0xFF2A3328, 0xFFE0EDDC), Pick(0xFF36302A, 0xFFF2E8DC), Pick(0xFF283136, 0xFFDCE9F0), Pick(0xFF362A31, 0xFFF2E0E9)];
        for (var i = 0; i < faces.Count; i++)
        {
            var cell = new Grid();
            AutomationProperties.SetName(cell, T("Portrait of a Sim"));
            cell.Children.Add(new Ellipse { Fill = new SolidColorBrush(washes[i % washes.Length]) });
            var brush = new ImageBrush { Stretch = Stretch.UniformToFill };
            cell.Children.Add(new Ellipse { Fill = brush });
            grid.Children.Add(cell);
            _ = Fill(faces[i], brush);
        }
        async Task Fill(SaveItem item, ImageBrush brush) { if (await SpImage(item, 0, p.Stop.Token) is { } src) brush.ImageSource = src; }
        page.Children.Add(grid);
        p.Host.Children.Add(page);
    }
}
