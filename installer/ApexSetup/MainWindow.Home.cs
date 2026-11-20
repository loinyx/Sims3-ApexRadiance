using System.Numerics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using static ApexSetup.Strings;

namespace ApexSetup;

// Home: the latest picture runs into the page with the game's title, Play and Update and play in its corner (nothing
// else there), the save Play starts on a tag at its top and the health crystal on its right. Below it, one scroll down:
// the saves as covers, the mods by folder beside health, what's new, the pictures and, quietly, the game's packs.
public sealed partial class MainWindow
{
    static Windows.UI.Color PageColor => Pick(0xFF0B0C0E, 0xFFF6F6F8);

    void BuildHome()
    {
        WarmThumbs();
        var host = ScreenHome;
        host.Children.Clear();
        if (_game is not { } g)
        {
            host.Children.Add(Heading(T("Home")));
            host.Children.Add(Card(Body(T("The launcher found no The Sims 3. Choose the game folder in Settings.")), Button(T("Choose the game"), () => Show(GameStep))));
            return;
        }
        var saves = Saves.List(Docs);
        host.Children.Add(HomeHero(g, saves.FirstOrDefault()));
        if (_running) host.Children.Add(Banner(InfoBarSeverity.Success, T("The Sims 3 is running"), _session is { } ss ? T("Playing for {0}.", Duration(ss.Played)) : ""));
        if (_offline) host.Children.Add(Banner(InfoBarSeverity.Warning, T("No internet"), T("You can still play. Updates wait until you're back online."), T("Try again"), async () => await LoadReleasesAsync()));
        if (_healthState == HealthState.Blocked)
            host.Children.Add(Banner(InfoBarSeverity.Error, T("Apex Radiance can't start"), T("The game still starts, but without Apex Radiance until this is fixed."), T("Open Health"), () => Show(HealthScreen)));

        var page = new StackPanel { Spacing = 48, Margin = new Thickness(8, -70, 8, 40) };
        // health and the saves side by side, right under the picture (the crystal sits on the seam); one above the other
        // on a narrow window
        var top = new Grid { ColumnSpacing = 32, RowSpacing = 40 };
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(2.3, GridUnitType.Star) });
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        top.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        top.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var health = (FrameworkElement)HomeHealth();
        var shelf = (FrameworkElement)HomeSaves(saves);
        shelf.Margin = new Thickness(0, 70, 0, 0);
        Grid.SetColumn(health, 1);
        top.Children.Add(health);
        top.Children.Add(shelf);
        top.SizeChanged += (_, e) =>
        {
            var one = e.NewSize.Width < 900;
            top.ColumnDefinitions[1].Width = one ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
            Grid.SetColumn(health, one ? 0 : 1);
            Grid.SetRow(health, one ? 1 : 0);
            shelf.Margin = new Thickness(0, one ? 0 : 70, 0, 0);
        };
        page.Children.Add(top);
        page.Children.Add(HomeMods());
        if (HomeNews() is { } news) page.Children.Add(news);
        page.Children.Add(HomePictures());
        page.Children.Add(HomeGame(g));
        host.Children.Add(page);
    }

    // ---------- the picture, running into the page ----------

    UIElement HomeHero(Game g, Save? next)
    {
        // (out to the edges of the scrolling area: the picture is not boxed in)
        var grid = new Grid { Height = 640, Margin = new Thickness(-32, -28, -32, 0) };
        if (HomePicture() is { } shot)
            grid.Children.Add(new Image { Source = Thumb(shot, 1600), Stretch = Stretch.UniformToFill });
        grid.Children.Add(Shade(new(0, 0), new(0, 1), (0, 0x8C), (0.22, 0), (0.5, 0), (0.82, 0xE6), (1, 0xFF)));
        grid.Children.Add(Shade(new(0, 0.5), new(1, 0.5), (0, 0xBF), (0.5, 0)));

        if (next is not null)
        {
            var tag = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            var pin = Lucide.Icon("map-pin", 16, Res("ApexAccentBrush"));
            pin.VerticalAlignment = VerticalAlignment.Center;
            tag.Children.Add(pin);
            tag.Children.Add(new TextBlock { Text = next.Name, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            tag.Children.Add(new TextBlock { Text = T("saved {0}", next.LastPlayed.ToString("d MMM")), Foreground = Res("ApexSoftTextBrush"), VerticalAlignment = VerticalAlignment.Center });
            grid.Children.Add(new Border
            {
                Child = tag, Padding = new Thickness(12, 7, 16, 7), CornerRadius = new CornerRadius(18), Margin = new Thickness(56, 26, 0, 0),
                HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
                Background = new SolidColorBrush(Pick(0x990B0C0E, 0xD9FFFFFF)), BorderBrush = new SolidColorBrush(Pick(0x1AFFFFFF, 0x14000000)), BorderThickness = new Thickness(1),
            });
        }
        var versions = new TextBlock
        {
            Text = "Apex Radiance " + (_state?.ApexVersion ?? "?") + (Known(Components.Modernization) is { Length: > 0 } mv ? " · Modernization " + mv : ""),
            FontSize = 12, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(0x99, 0xED, 0xED, 0xF0)),
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 34, 40, 0),
        };
        grid.Children.Add(versions);

        var corner = new StackPanel { Spacing = 24, Margin = new Thickness(56, 0, 0, 120), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Bottom };
        corner.Children.Add(new TextBlock { Text = "The Sims 3", FontSize = 72, FontWeight = FontWeights.Bold, CharacterSpacing = -30, LineHeight = 72 });
        corner.Children.Add(PlayButtons());
        grid.Children.Add(corner);
        grid.SizeChanged += (_, e) => versions.Visibility = e.NewSize.Width < 640 ? Visibility.Collapsed : Visibility.Visible;
        return grid;
    }

    // Play starts the game as it is; Update and play puts the ready updates in first (the player chooses, nothing is forced)
    UIElement PlayButtons()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        if (_running)
        {
            row.Children.Add(new Border { Background = Res("ApexSuccessSoftBrush"), CornerRadius = new CornerRadius(10), Padding = new Thickness(24, 18, 24, 18),
                Child = new TextBlock { Text = _session is { } s ? T("Playing · {0}", Duration(s.Played)) : T("Playing"), Foreground = Res("ApexSuccessBrush"), FontWeight = FontWeights.SemiBold } });
            return row;
        }
        var play = new Button { Style = (Style)Application.Current.Resources["AccentButtonStyle"], Height = 60, Padding = new Thickness(52, 0, 52, 0), CornerRadius = new CornerRadius(10), IsEnabled = !_busy && !Maintenance };
        var pc = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        pc.Children.Add(new FontIcon { Glyph = "", FontSize = 14 });
        pc.Children.Add(new TextBlock { Text = _healthState == HealthState.Blocked ? T("Play anyway") : T("Play"), FontSize = 18, FontWeight = FontWeights.SemiBold });
        play.Content = pc;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(play, "PlayButton");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(play, T("Play"));
        play.Click += async (_, _) => await PlayAsync(null);
        row.Children.Add(play);
        if (WelcomeUpdates() is { Count: > 0 } ups)
        {
            var up = new Button { Height = 60, Padding = new Thickness(22, 0, 18, 0), CornerRadius = new CornerRadius(10), IsEnabled = !_busy && !Maintenance,
                BorderBrush = Res("ApexAccentBrush"), Background = new SolidColorBrush(Pick(0x8C0B0C0E, 0xD9FFFFFF)) };
            var uc = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            uc.Children.Add(new TextBlock { Text = T("Update and play"), FontSize = 15, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            uc.Children.Add(new Border { MinWidth = 22, Height = 22, CornerRadius = new CornerRadius(11), Background = Res("ApexAccentBrush"), Padding = new Thickness(6, 0, 6, 0), VerticalAlignment = VerticalAlignment.Center,
                Child = new TextBlock { Text = ups.Count.ToString(), FontSize = 12, FontWeight = FontWeights.SemiBold, Foreground = Res("TextOnAccentFillColorPrimaryBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } });
            up.Content = uc;
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(up, T("Update and play"));
            ToolTipService.SetToolTip(up, string.Join("\n", ups.Select(c => c + " " + (_latest.GetValueOrDefault(c)?.Version ?? ""))));
            up.Click += async (_, _) => await PlayAsync(null, update: true);
            row.Children.Add(up);
        }
        return row;
    }

    static Border Shade(Windows.Foundation.Point from, Windows.Foundation.Point to, params (double At, byte Alpha)[] stops)
    {
        var b = new LinearGradientBrush { StartPoint = from, EndPoint = to };
        foreach (var (at, a) in stops) b.GradientStops.Add(new GradientStop { Color = Windows.UI.Color.FromArgb(a, PageColor.R, PageColor.G, PageColor.B), Offset = at });
        return new Border { Background = b, IsHitTestVisible = false };
    }

    // ---------- the sections ----------

    // a section's head: a small number, its title, a note and a link on the right
    static UIElement SectionHead(string number, string title, string? note = null, string? link = null, Action? go = null, string brush = "ApexAccentBrush")
    {
        var g = new Grid { ColumnSpacing = 14, Margin = new Thickness(0, 0, 0, 16) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new TextBlock { Text = number, FontSize = 12, FontWeight = FontWeights.Bold, CharacterSpacing = 160, Foreground = Res(brush), VerticalAlignment = VerticalAlignment.Center });
        var t = new TextBlock { Text = title, FontSize = 22, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        if (note is not null) { var n = new TextBlock { Text = note, FontSize = 14, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis }; Grid.SetColumn(n, 2); g.Children.Add(n); }
        if (link is not null && go is not null) { var l = Link(link, go); l.Padding = new Thickness(0); l.VerticalAlignment = VerticalAlignment.Center; Grid.SetColumn(l, 3); g.Children.Add(l); }
        return g;
    }

    // 01 · the saves as covers: the one Play starts wide, with a ring; the covers come in one after another, lean in under
    // the pointer, and Back up turns into a ring and then a tick
    UIElement HomeSaves(List<Save> saves)
    {
        var s = new StackPanel();
        s.Children.Add(SectionHead("01", T("Your households"), null, T("All saves"), () => Show(SavesScreen)));
        if (saves.Count == 0) { s.Children.Add(Muted(T("No saves yet: they show here once you save a game."))); return s; }
        var docs = Docs;
        var shelf = new Grid { ColumnSpacing = 16, Height = 300, ChildrenTransitions = new TransitionCollection { new EntranceThemeTransition { IsStaggeringEnabled = true, FromHorizontalOffset = 40, FromVerticalOffset = 0 } } };
        var shown = saves.Take(5).ToList();
        var covers = new List<(Save Save, Grid Photo, TextBlock Meta, HyperlinkButton? Backup)>();
        for (int i = 0; i < shown.Count; i++)
        {
            shelf.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(i == 0 ? 1.6 : 1, GridUnitType.Star) });
            var save = shown[i];
            var first = i == 0;
            var card = new Grid { CornerRadius = new CornerRadius(14), Background = new LinearGradientBrush
            {
                StartPoint = new(0, 0), EndPoint = new(1, 1),
                GradientStops = { new GradientStop { Color = Pick(0xFF23242B, 0xFFF0F0F4), Offset = 0 }, new GradientStop { Color = Pick(0xFF15161A, 0xFFE4E4EA), Offset = 1 } },
            } };
            if (first) { card.BorderBrush = Res("ApexAccentBrush"); card.BorderThickness = new Thickness(2); }
            var photo = new Grid();
            photo.Children.Add(new TextBlock { Text = Initials(save.Name), FontSize = first ? 64 : 48, FontWeight = FontWeights.Bold, Foreground = new SolidColorBrush(Pick(0xFF2E2F37, 0xFFC9CAD2)), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 0, 60) });
            photo.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(280) };
            card.SizeChanged += (_, e) => photo.CenterPoint = new Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0);
            card.Children.Add(photo);
            card.Children.Add(Shade(new(0, 1), new(0, 0), (0, 0xF2), (0.55, 0)));
            var text = new StackPanel { Spacing = 3, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(first ? 18 : 14, 0, 14, first ? 16 : 14) };
            text.Children.Add(new TextBlock { Text = save.Name, FontSize = first ? 22 : 16, FontWeight = first ? FontWeights.Bold : FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
            var meta = new TextBlock { Text = save.LastPlayed.ToString("d MMM"), FontSize = first ? 13 : 12, Foreground = Res("ApexSoftTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis };
            text.Children.Add(meta);
            HyperlinkButton? backup = null;
            if (!first)
            {
                backup = Link(T("Back up"), () => { });
                backup.Padding = new Thickness(0);
                backup.FontSize = 12;
                text.Children.Add(backup);
                var one = save;
                var b = backup;
                backup.Click += async (_, _) =>
                {
                    b.IsEnabled = false;
                    b.Content = new ProgressRing { IsActive = true, Width = 14, Height = 14 };
                    try
                    {
                        await Task.Run(() => Saves.Backup(one, docs));
                        var done = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(300) } };
                        done.Children.Add(Lucide.Icon("check", 13, Res("ApexSuccessBrush")));
                        done.Children.Add(new TextBlock { Text = T("Backed up"), FontSize = 12, Foreground = Res("ApexSuccessBrush") });
                        b.Content = done;
                        DispatcherQueue.TryEnqueue(() => done.Opacity = 1);
                    }
                    catch (Exception e) { App.Log(e); b.Content = T("Try again"); b.IsEnabled = true; ToolTipService.SetToolTip(b, ErrorText(e)); }
                };
            }
            card.Children.Add(text);
            if (first)
                card.Children.Add(new Border { Background = Res("ApexAccentBrush"), CornerRadius = new CornerRadius(11), Padding = new Thickness(10, 4, 10, 4), Margin = new Thickness(14),
                    HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
                    Child = new TextBlock { Text = T("PLAY STARTS HERE"), FontSize = 11, FontWeight = FontWeights.Bold, CharacterSpacing = 60, Foreground = new SolidColorBrush(PageColor) } });
            card.PointerEntered += (_, _) => photo.Scale = new Vector3(1.06f, 1.06f, 1);
            card.PointerExited += (_, _) => photo.Scale = Vector3.One;
            Grid.SetColumn(card, i);
            shelf.Children.Add(card);
            covers.Add((save, photo, meta, backup));
        }
        // a narrow window keeps the wide cover and two more
        shelf.SizeChanged += (_, e) =>
        {
            var keep = e.NewSize.Width < 760 ? 2 : e.NewSize.Width < 1000 ? 3 : shown.Count;
            for (int i = 0; i < shown.Count; i++)
            {
                shelf.ColumnDefinitions[i].Width = i < keep ? new GridLength(i == 0 ? 1.6 : 1, GridUnitType.Star) : new GridLength(0);
                shelf.Children[i].Visibility = i < keep ? Visibility.Visible : Visibility.Collapsed;
            }
        };
        s.Children.Add(shelf);

        // the household photos, sizes and backups read off the UI thread, then faded in
        _ = Task.Run(() => (SaveHealth.Check(docs, record: false), shown.Select(Saves.Photo).ToList())).ContinueWith(async t =>
        {
            if (t.IsFaulted) return;
            var (reports, photos) = t.Result;
            for (int i = 0; i < covers.Count; i++)
            {
                var (save, photo, meta, backup) = covers[i];
                if (reports.FirstOrDefault(r => r.Save.Dir == save.Dir) is { } r)
                    meta.Text = save.LastPlayed.ToString(i == 0 ? "d MMM, HH:mm" : "d MMM") + " · " + ModRow.SizeText(r.Bytes)
                        + (i == 0 ? " · " + (r.LastBackup is null ? T("no backup yet") : T("backed up")) : r.LastBackup is null ? "" : " · " + T("backed up"));
                if (await SaveCard.ImageOf(photos[i]) is { } img)
                {
                    var image = new Image { Source = img, Stretch = Stretch.Uniform, Margin = new Thickness(0, 12, 0, 70), Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(450) } };
                    photo.Children.Clear();
                    photo.Children.Add(image);
                    DispatcherQueue.TryEnqueue(() => image.Opacity = 1);
                }
            }
        }, TaskScheduler.FromCurrentSynchronizationContext());
        return s;
    }

    static string Initials(string name) => string.Concat(name.Split([' ', '-', '_'], StringSplitOptions.RemoveEmptyEntries).Take(2).Select(w => char.ToUpperInvariant(w[0])));

    // 02 · the mods: a map of their folders, the biggest first (turning mods on and off stays in Mods)
    UIElement HomeMods()
    {
        var s = new StackPanel();
        var mods = _mods.Where(m => m.FileType == "package").ToList();
        var asi = _mods.Count(m => m.FileType == "asi");
        var off = mods.Count(m => !m.Enabled);
        s.Children.Add(SectionHead("02", mods.Count == 1 ? T("1 mod") : T("{0} mods", mods.Count),
            mods.Count == 0 ? null : ModRow.SizeText(mods.Sum(m => m.Size)) + " · " + (off == 0 ? T("all on") : T("{0} off", off)) + (asi > 0 ? " · " + (asi == 1 ? T("1 game plugin") : T("{0} game plugins", asi)) : ""),
            T("Open Mods"), () => Show(ModsScreen)));
        if (mods.Count == 0) { s.Children.Add(Muted(_checked == default ? T("Counting your mods…") : T("No mods yet. Add them in Mods."))); return s; }
        var groups = mods.GroupBy(m => DirOf(m).Replace(Path.DirectorySeparatorChar.ToString(), " › ")).Select(x => (Dir: x.Key.Length == 0 ? "Mods" : x.Key, N: x.Count()))
            .OrderByDescending(x => x.N).ToList();
        var top = groups.Take(4).ToList();
        var next = groups.Skip(4).Take(3).ToList();
        var rest = groups.Skip(7).ToList();
        if (rest.Count > 0) next.Add((rest.Count == 1 ? rest[0].Dir : T("{0} more places", rest.Count), rest.Sum(x => x.N)));
        var map = new Grid { RowSpacing = 6, Height = 260 };
        map.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1.4, GridUnitType.Star) });
        if (next.Count > 0) map.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        byte[] shades = [0x3B, 0x33, 0x2C, 0x27, 0x24, 0x21, 0x21, 0x1F];
        var k = 0;
        Grid Row(List<(string Dir, int N)> items, int row)
        {
            var r = new Grid { ColumnSpacing = 6 };
            for (int i = 0; i < items.Count; i++)
            {
                r.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(items[i].N, GridUnitType.Star) });
                var shade = shades[Math.Min(k++, shades.Length - 1)];
                var block = new Button
                {
                    HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch,
                    Padding = new Thickness(14, 12, 14, 12), CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(0),
                    Background = new SolidColorBrush(Mix(Pick(0xFF131417, 0xFFFFFFFF), Accent(), (shade - 0x16) / 64.0 * (_light ? 0.55 : 0.75))),
                };
                // (a shade of the accent, lighter under the pointer, not the grey of a plain button)
                block.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Mix(Pick(0xFF131417, 0xFFFFFFFF), Accent(), ((shade - 0x16) / 64.0 + 0.12) * (_light ? 0.55 : 0.75)));
                block.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Mix(Pick(0xFF131417, 0xFFFFFFFF), Accent(), ((shade - 0x16) / 64.0 - 0.05) * (_light ? 0.55 : 0.75)));
                block.Resources["ButtonForegroundPointerOver"] = Res("TextFillColorPrimaryBrush");
                var inside = new Grid();
                inside.RowDefinitions.Add(new RowDefinition());
                inside.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                inside.Children.Add(new TextBlock { Text = items[i].Dir, FontSize = 13, Foreground = Res("ApexAccentTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
                var n = new TextBlock { Text = items[i].N.ToString(), FontSize = 15, FontWeight = FontWeights.SemiBold };
                Grid.SetRow(n, 1);
                inside.Children.Add(n);
                block.Content = inside;
                var dir = items[i].Dir;
                block.Click += (_, _) => Show(ModsScreen);
                Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(block, $"{dir}: {items[i].N}");
                Grid.SetColumn(block, i);
                r.Children.Add(block);
            }
            Grid.SetRow(r, row);
            return r;
        }
        map.Children.Add(Row(top, 0));
        if (next.Count > 0) map.Children.Add(Row(next, 1));
        s.Children.Add(map);
        var lately = mods.Select(m => (m, Added(m))).OrderByDescending(x => x.Item2).Take(3).ToList();
        var line = new TextBlock { FontSize = 13, Margin = new Thickness(0, 14, 0, 0), TextWrapping = TextWrapping.Wrap };
        line.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = T("Added lately") + "   ", Foreground = Res("ApexMutedBrush") });
        foreach (var (m, when) in lately)
        {
            line.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = Path.GetFileNameWithoutExtension(m.Name) + " " });
            line.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = when.ToString("d MMM") + "    ", Foreground = Res("ApexMutedBrush") });
        }
        s.Children.Add(line);
        return s;
    }

    // health, under the crystal: the crystal stands mostly in the picture with its tip on the card; the card says the state
    // and lists the notes, one line each with an icon and a quiet action (the saves without a backup gathered in one line)
    UIElement HomeHealth()
    {
        var wrap = new Grid();
        var all = _health.Where(i => i.Finding.Text != "{0} may overlap in {1}: {2}").OrderByDescending(i => i.Finding.Severity).ToList();
        const string NoBackup = "The save {0} has no backup yet.";
        var unsaved = all.Where(i => i.Finding.Text == NoBackup).ToList();
        var rows = all.Where(i => i.Finding.Text != NoBackup).Take(unsaved.Count > 0 ? 3 : 4).ToList();
        var body = new StackPanel { Padding = new Thickness(24, 64, 24, 16) };
        var title = new TextBlock { Text = HealthTitle(), FontSize = 20, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, Foreground = new SolidColorBrush(StateTint(255)) };
        body.Children.Add(title);
        var sub = Muted(_checked == default ? T("Checked when you press Play") : T("Checked {0}", _checked.ToString("t")));
        sub.HorizontalAlignment = HorizontalAlignment.Center;
        sub.Margin = new Thickness(0, 2, 0, 16);
        body.Children.Add(sub);
        var line = Res("ApexHairlineBrush");
        void Note(bool warn, string text, string? tip, FrameworkElement? action)
        {
            var row = new Grid { ColumnSpacing = 12, Padding = new Thickness(0, 11, 0, 11), BorderBrush = line, BorderThickness = new Thickness(0, 1, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            FrameworkElement mark = warn ? Lucide.Icon("triangle-alert", 15, Res("ApexWarningBrush"))
                : new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 6, Height = 6, Fill = Res("ApexMutedBrush"), Margin = new Thickness(4, 0, 5, 0) };
            mark.VerticalAlignment = VerticalAlignment.Center;
            row.Children.Add(mark);
            var t = new TextBlock { Text = text, FontSize = 13, Foreground = warn ? Res("ApexWarningBrush") : Res("ApexSoftTextBrush"), TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
            if (tip is not null) ToolTipService.SetToolTip(t, tip);
            Grid.SetColumn(t, 1);
            row.Children.Add(t);
            if (action is not null) { action.VerticalAlignment = VerticalAlignment.Center; Grid.SetColumn(action, 2); row.Children.Add(action); }
            body.Children.Add(row);
        }
        // (an action as a quiet link in the accent colour, not a grey button)
        static FrameworkElement Quiet(Button b)
        {
            b.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            b.BorderThickness = new Thickness(0);
            b.Padding = new Thickness(6, 2, 0, 2);
            b.MinHeight = 0;
            b.FontSize = 13;
            b.Foreground = Res("ApexAccentBrush");
            return b;
        }
        foreach (var it in rows)
        {
            var full = T(it.Finding.Text, it.Finding.Args);
            var (short1, _) = Sentences(full);
            Note(it.Finding.Severity != Severity.Info, short1, full, FindingAction(it) is { } act ? Quiet(act) : null);
        }
        if (unsaved.Count > 0)
        {
            var names = string.Join(", ", unsaved.Select(i => i.Finding.Args.FirstOrDefault() ?? ""));
            var docs = Docs;
            var back = Button(T("Back up all"), async () =>
            {
                var list = Saves.List(docs).Where(x => unsaved.Any(u => u.Finding.Args.FirstOrDefault() == x.Name)).ToList();
                await BackupAsync(list);
                CheckHealth();
            });
            Note(false, unsaved.Count == 1 ? T("1 save has no backup yet") : T("{0} saves have no backup yet", unsaved.Count), names, Quiet(back));
        }
        if (all.Count == 0) Note(false, T("Every check passed"), null, null);
        var open = Link(T("Open Health"), () => Show(HealthScreen));
        open.HorizontalAlignment = HorizontalAlignment.Center;
        open.Margin = new Thickness(0, 8, 0, 0);
        body.Children.Add(open);
        // (the colour from the top, faint and long, over the card's own grey)
        var card = new Grid { Margin = new Thickness(0, 70, 0, 0), CornerRadius = new CornerRadius(16), Background = Res("ApexGlassBrush"),
            BorderBrush = Res("ApexGlassLineBrush"), BorderThickness = new Thickness(1) };
        var glow = new LinearGradientBrush { StartPoint = new(0.5, 0), EndPoint = new(0.5, 1) };
        glow.GradientStops.Add(new GradientStop { Color = StateTint(0x14), Offset = 0 });
        glow.GradientStops.Add(new GradientStop { Color = StateTint(0x06), Offset = 0.4 });
        glow.GradientStops.Add(new GradientStop { Color = StateTint(0), Offset = 0.85 });
        card.Children.Add(new Border { Background = glow, IsHitTestVisible = false });
        card.Children.Add(body);
        wrap.Children.Add(card);
        // the crystal: mostly in the picture, its tip on the card, in a soft halo of its colour
        var crystal = new Grid { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, -150, 0, 0) };
        crystal.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse
        {
            Width = 300, Height = 300, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, IsHitTestVisible = false,
            Fill = new RadialGradientBrush { GradientStops = { new GradientStop { Color = StateTint(0x38), Offset = 0 }, new GradientStop { Color = StateTint(0x10), Offset = 0.45 }, new GradientStop { Color = StateTint(0), Offset = 1 } } },
        });
        _homeCrystal = new Crystal(190); // (a new one each time: a XAML element cannot move between rebuilt screens)
        _homeCrystal.SetLevel(HealthLevel, animate: false);
        crystal.Children.Add(_homeCrystal);
        crystal.Tapped += (_, _) => Show(HealthScreen);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(crystal, T("Open Health"));
        wrap.Children.Add(crystal);
        return wrap;
    }

    // 03 · updates and what's new: every part with an update ready (Apex Radiance, Sims 3 Modernization and the rest), its
    // installed and new version and its release's headline; with none, the newest Apex Radiance notes
    Release? ReleaseOf(string c) => c == Components.Modernization ? ModRelease() : Repo.TryGetValue(c, out var repo) ? _latest.GetValueOrDefault(repo) : null;
    string? InstalledOf(string c) => c == Components.Apex ? _state?.ApexVersion : Known(c) is { Length: > 0 } v ? v : _state?.Installed(c);
    static List<string> Paragraphs(string? md) => (md ?? "").Replace("\r", "").Split("\n\n").Select(p => p.Trim()).Where(p => p.Length > 0 && !p.StartsWith('#') && !p.StartsWith('-')).Select(Plain).ToList();

    UIElement? HomeNews()
    {
        var ups = WelcomeUpdates();
        var apex = _latest.GetValueOrDefault(Releases.Apex);
        if (ups.Count == 0 && apex is null) return null;
        var band = new Grid { CornerRadius = new CornerRadius(18), BorderThickness = new Thickness(1), BorderBrush = Res("ApexAccentLineBrush"), Padding = new Thickness(44, 36, 44, 36), ColumnSpacing = 40 };
        band.Background = new LinearGradientBrush { StartPoint = new(0, 0), EndPoint = new(1, 1), GradientStops =
            { new GradientStop { Color = Windows.UI.Color.FromArgb(255, 0x2A, 0x25, 0x60), Offset = 0 }, new GradientStop { Color = Windows.UI.Color.FromArgb(255, 0x15, 0x15, 0x1F), Offset = 0.55 }, new GradientStop { Color = Windows.UI.Color.FromArgb(255, 0x12, 0x13, 0x18), Offset = 1 } } };
        band.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });
        band.ColumnDefinitions.Add(new ColumnDefinition());
        var lilac = Res("ApexAccentTextBrush");
        var soft = Res("ApexMutedBrush");
        var left = new StackPanel { Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        var right = new StackPanel { Spacing = 14, VerticalAlignment = VerticalAlignment.Center };
        if (ups.Count > 0)
        {
            left.Children.Add(new TextBlock { Text = "03 · " + T("UPDATES"), FontSize = 12, FontWeight = FontWeights.Bold, CharacterSpacing = 160, Foreground = lilac });
            left.Children.Add(new TextBlock { Text = ups.Count == 1 ? T("1 update ready") : T("{0} updates ready", ups.Count), FontSize = 28, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
            left.Children.Add(new TextBlock { Text = T("Nothing changes until you choose; your settings stay."), FontSize = 14, Foreground = soft, TextWrapping = TextWrapping.Wrap });
            foreach (var c in ups)
            {
                var item = new StackPanel { Spacing = 3 };
                var head = new TextBlock { FontSize = 16, TextWrapping = TextWrapping.Wrap };
                head.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = c, FontWeight = FontWeights.SemiBold });
                var from = InstalledOf(c);
                var to = ReleaseOf(c)?.Version;
                if (to is not null) head.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "   " + (from is { Length: > 0 } && from != "?" ? from + " → " + to : to), Foreground = soft, FontSize = 14 });
                item.Children.Add(head);
                if (Paragraphs(ReleaseOf(c)?.Body).FirstOrDefault() is { } headline)
                    item.Children.Add(new TextBlock { Text = headline, FontSize = 13, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis });
                right.Children.Add(item);
            }
        }
        else
        {
            var paras = Paragraphs(apex!.Body);
            left.Children.Add(new TextBlock { Text = "03 · " + T("WHAT'S NEW"), FontSize = 12, FontWeight = FontWeights.Bold, CharacterSpacing = 160, Foreground = lilac });
            left.Children.Add(new TextBlock { Text = "Apex Radiance " + apex.Version, FontSize = 28, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
            left.Children.Add(new TextBlock { Text = T("installed"), FontSize = 14, Foreground = soft });
            if (paras.Count > 0) right.Children.Add(new TextBlock { Text = paras[0], FontSize = 20, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
            if (paras.Count > 1) right.Children.Add(new TextBlock { Text = paras[1], FontSize = 14, LineHeight = 22, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, MaxLines = 4, TextTrimming = TextTrimming.CharacterEllipsis });
        }
        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, Margin = new Thickness(0, 4, 0, 0) };
        if (ups.Count > 0 && !_running) acts.Children.Add(Button(T("Update and play"), async () => await PlayAsync(null, update: true), accent: true));
        var read = Link(T("Read the notes"), () => Show(NewsScreen));
        read.VerticalAlignment = VerticalAlignment.Center;
        acts.Children.Add(read);
        right.Children.Add(acts);
        band.Children.Add(left);
        Grid.SetColumn(right, 1);
        band.Children.Add(right);
        // a narrow window: the title above the list
        band.SizeChanged += (_, e) =>
        {
            var one = e.NewSize.Width < 760;
            band.ColumnDefinitions[0].Width = one ? new GridLength(1, GridUnitType.Star) : new GridLength(300);
            band.ColumnDefinitions[1].Width = one ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
            if (band.RowDefinitions.Count == 0) { band.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); band.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); }
            Grid.SetColumn(right, one ? 0 : 1);
            Grid.SetRow(right, one ? 1 : 0);
            right.Margin = new Thickness(0, one ? 20 : 0, 0, 0);
        };
        return band;
    }

    // 05 · the pictures, a collage of the newest four
    UIElement HomePictures()
    {
        var s = new StackPanel();
        var dir = Path.Combine(Docs, "Screenshots");
        var count = 0;
        try { if (Directory.Exists(dir)) count = Directory.EnumerateFiles(dir).Count(f => Path.GetExtension(f).ToLowerInvariant() is ".jpg" or ".jpeg" or ".png"); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        var shots = Screenshots(4);
        s.Children.Add(SectionHead("04", T("Your pictures"), count == 0 ? null : count == 1 ? T("1 picture") : T("{0} pictures", count), count == 0 ? null : T("Screenshots folder"), () => OpenFolder(dir)));
        if (shots.Count == 0) { s.Children.Add(Muted(T("Pictures you take in the game show here."))); return s; }
        var grid = new Grid { ColumnSpacing = 12, RowSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(2, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(150) });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(150) });
        (int Col, int Row, int ColSpan, int RowSpan)[] spots = [(0, 0, 1, 2), (1, 0, 1, 1), (2, 0, 1, 1), (1, 1, 2, 1)];
        for (int i = 0; i < shots.Count; i++)
        {
            var b = new Border { CornerRadius = new CornerRadius(14), Background = Res("ApexPillBrush"), Child = new Image { Source = Thumb(shots[i], i == 0 ? 640 : 420), Stretch = Stretch.UniformToFill } };
            Grid.SetColumn(b, spots[i].Col); Grid.SetRow(b, spots[i].Row); Grid.SetColumnSpan(b, spots[i].ColSpan); Grid.SetRowSpan(b, spots[i].RowSpan);
            grid.Children.Add(b);
        }
        s.Children.Add(grid);
        return s;
    }

    // the game, quietly at the foot: its packs, edition and the parts the launcher put in
    UIElement HomeGame(Game g)
    {
        var row = new Grid { ColumnSpacing = 24, Padding = new Thickness(0, 22, 0, 0), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var packs = Expansions.Installed(g);
        var badges = new WrapPanel { Spacing = 5 };
        foreach (var (p, dir) in packs) badges.Children.Add(PackBadge(p, dir, 20));
        badges.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(badges);
        var eps = packs.Count(x => x.Pack.Expansion);
        var text = Muted(g.Label + " · " + T("base game") + (eps > 0 ? ", " + (eps == 1 ? T("1 expansion") : T("{0} expansions", eps)) : "")
            + (packs.Count - eps is var sp and > 0 ? ", " + (sp == 1 ? T("1 stuff pack") : T("{0} stuff packs", sp)) : ""));
        text.VerticalAlignment = VerticalAlignment.Center;
        text.TextTrimming = TextTrimming.CharacterEllipsis;
        Grid.SetColumn(text, 1);
        row.Children.Add(text);
        var links = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 20, VerticalAlignment = VerticalAlignment.Center };
        var worlds = Link(T("Worlds"), () => Show(WorldsScreen)); worlds.Padding = new Thickness(0);
        var comps = Link(T("Components"), () => Show(ComponentsStep)); comps.Padding = new Thickness(0);
        links.Children.Add(worlds);
        links.Children.Add(comps);
        Grid.SetColumn(links, 2);
        row.Children.Add(links);
        return row;
    }
}
