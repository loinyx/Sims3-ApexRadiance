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
    static Windows.UI.Color PageColor => Pick(0xFF0C0C0C, 0xFFF6F6F6);

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
        if (_running) host.Children.Add(Banner(InfoBarSeverity.Success, T("The Sims 3 is running"), _session is { } ss ? T("Playing for {0}.", Duration(ss.Played)) : ""));
        if (_offline) host.Children.Add(Banner(InfoBarSeverity.Warning, T("No internet"), T("You can still play. Updates wait until you're back online."), T("Try again"), async () => await LoadReleasesAsync()));
        if (_healthState == HealthState.Blocked)
            host.Children.Add(Banner(InfoBarSeverity.Error, T("Apex Radiance can't start"), T("The game still starts, but without Apex Radiance until this is fixed."), T("Open Health"), () => Show(HealthScreen)));
        var top = (FrameworkElement)HomeTop(g, saves);
        host.Children.Add(top);
        // the top fills the window, so the households show only once the page scrolls
        void Fit()
        {
            var above = host.Children.OfType<FrameworkElement>().TakeWhile(c => c != top).Sum(c => c.ActualHeight + host.Spacing);
            top.Height = Math.Max(560, Scroller.ActualHeight - Scroller.Padding.Top - 14 - above);
        }
        if (_homeFit is { } old) Scroller.SizeChanged -= old;
        _homeFit = (_, _) => Fit();
        Scroller.SizeChanged += _homeFit;
        top.Loaded += (_, _) => Fit();
        // (a banner above measures after the top: it moves the top down, so fit again once it has a size)
        foreach (var banner in host.Children.OfType<FrameworkElement>().TakeWhile(c => c != top)) banner.SizeChanged += (_, _) => Fit();
        var page = new StackPanel { Spacing = 40, Margin = new Thickness(0, 32, 0, 32) };
        page.Children.Add(HomeSaves(saves));
        if (HomeNews() is { } news) page.Children.Add(news);
        page.Children.Add(HomePictures());
        host.Children.Add(page);
    }

    // ---------- the top: the picture with Play, health and the last session beside it, and the counts under them ----------

    UIElement HomeTop(Game g, List<Save> saves)
    {
        var top = new Grid { ColumnSpacing = 12, RowSpacing = 12 };
        top.ColumnDefinitions.Add(new ColumnDefinition());
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(340) });
        top.RowDefinitions.Add(new RowDefinition());
        top.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        // the picture: rounded, fading into the page at its foot so the buttons sit on it
        // (the picture keeps a dark look in both themes: a light fade over a game picture only washes it out)
        var hero = new Grid { CornerRadius = new CornerRadius(26), BorderBrush = Res("ApexGlassLineBrush"), BorderThickness = new Thickness(1), RequestedTheme = ElementTheme.Dark, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x10, 0x10, 0x10)) };
        if (HomePicture() is { } shot) hero.Children.Add(new Image { Source = Thumb(shot, 1600), Stretch = Stretch.UniformToFill });
        Border Dark(Windows.Foundation.Point from, Windows.Foundation.Point to, params (double At, byte Alpha)[] stops)
        {
            var b = new LinearGradientBrush { StartPoint = from, EndPoint = to };
            foreach (var (at, a) in stops) b.GradientStops.Add(new GradientStop { Color = Windows.UI.Color.FromArgb(a, 0x0C, 0x0C, 0x0C), Offset = at });
            return new Border { Background = b, IsHitTestVisible = false };
        }
        hero.Children.Add(Dark(new(0, 0), new(0, 1), (0, 0x73), (0.25, 0), (0.55, 0), (0.85, 0xB3), (1, 0xE6)));
        hero.Children.Add(Dark(new(0, 0.5), new(1, 0.5), (0, 0x8C), (0.55, 0)));
        if (saves.FirstOrDefault() is { } next)
        {
            var tag = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            var pin = Lucide.Icon("map-pin", 15, Res("ApexAccentBrush"));
            pin.VerticalAlignment = VerticalAlignment.Center;
            tag.Children.Add(pin);
            tag.Children.Add(new TextBlock { Text = next.Name, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            tag.Children.Add(new TextBlock { Text = T("saved {0}", next.LastPlayed.ToString("d MMM", LangCulture())), Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0xBD, 0xBD, 0xBD)), VerticalAlignment = VerticalAlignment.Center });
            var change = Link(T("Change"), () => Show(SavesScreen));
            change.Padding = new Thickness(8, 2, 8, 2);
            tag.Children.Add(change);
            hero.Children.Add(new Border
            {
                Child = tag, Padding = new Thickness(14, 6, 6, 6), CornerRadius = new CornerRadius(20), Margin = new Thickness(26, 24, 0, 0),
                HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x99, 0x0C, 0x0C, 0x0C)), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x1A, 255, 255, 255)), BorderThickness = new Thickness(1),
            });
        }
        var buttons = PlayButtons();
        if (buttons is FrameworkElement fb) { fb.Margin = new Thickness(36, 0, 36, 34); fb.HorizontalAlignment = HorizontalAlignment.Left; fb.VerticalAlignment = VerticalAlignment.Bottom; }
        hero.Children.Add(buttons);
        top.Children.Add(hero);

        // beside it: health with its crystal, and how the last session went
        var side = new Grid { RowSpacing = 12 };
        side.RowDefinitions.Add(new RowDefinition());
        side.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var health = new StackPanel { Spacing = 10, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        _homeCrystal = new Crystal(130); // (a new one each time: a XAML element cannot move between rebuilt screens)
        _homeCrystal.SetLevel(HealthLevel, animate: false);
        _homeCrystal.HorizontalAlignment = HorizontalAlignment.Center;
        health.Children.Add(_homeCrystal);
        health.Children.Add(new TextBlock { Text = HealthTitle(), FontSize = 20, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap });
        var healthNote = Muted(_healthState switch
        {
            HealthState.Blocked => T("The game still starts, but without Apex Radiance until this is fixed."),
            HealthState.Ready => T("Everything is in place."),
            _ => T("Nothing stops the game from starting. Take a look when you have a minute."),
        });
        healthNote.HorizontalAlignment = HorizontalAlignment.Center;
        healthNote.TextAlignment = TextAlignment.Center;
        healthNote.FontSize = 13;
        healthNote.MaxWidth = 260;
        health.Children.Add(healthNote);
        var glow = new LinearGradientBrush { StartPoint = new(0.5, 0), EndPoint = new(0.5, 1) };
        glow.GradientStops.Add(new GradientStop { Color = StateTint(0x22), Offset = 0 });
        glow.GradientStops.Add(new GradientStop { Color = StateTint(0x08), Offset = 0.55 });
        glow.GradientStops.Add(new GradientStop { Color = StateTint(0), Offset = 1 });
        var healthCard = new Grid { CornerRadius = new CornerRadius(22), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), Padding = new Thickness(22) };
        healthCard.Children.Add(new Border { Background = glow, Margin = new Thickness(-22), IsHitTestVisible = false });
        healthCard.Children.Add(health);
        side.Children.Add(Tile(healthCard, T("Open Health"), () => Show(HealthScreen)));

        var last = SessionHistory.List().FirstOrDefault();
        var lastBody = new StackPanel { Spacing = 4 };
        lastBody.Children.Add(new TextBlock { Text = T("LAST TIME"), FontSize = 11, FontWeight = FontWeights.SemiBold, CharacterSpacing = 100, Foreground = Res("ApexMutedBrush") });
        lastBody.Children.Add(new TextBlock { Text = last is null ? T("No game played yet") : Played(last.Played), FontSize = 18, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        lastBody.Children.Add(new TextBlock
        {
            Text = last is null ? T("Press Play to start") : (last.End switch { SessionEnd.Crash => T("The game closed on its own"), SessionEnd.LowMemory => T("Ran out of memory"), _ => T("Ended well") })
                + (last is null ? "" : " · " + last.Started.ToString("dddd HH:mm", LangCulture())),
            FontSize = 13, Foreground = last?.End is SessionEnd.Crash or SessionEnd.LowMemory ? Res("ApexWarningBrush") : Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis,
        });
        var lastCard = new Border { CornerRadius = new CornerRadius(22), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), Padding = new Thickness(22, 18, 22, 18), Child = lastBody };
        var lastTile = Tile(lastCard, T("Open Health"), () => Show(HealthScreen));
        Grid.SetRow(lastTile, 1);
        side.Children.Add(lastTile);
        Grid.SetColumn(side, 1);
        top.Children.Add(side);

        // the counts: mods, packs, saves, pictures; each opens its screen
        var counts = new Grid { ColumnSpacing = 12 };
        for (int i = 0; i < 4; i++) counts.ColumnDefinitions.Add(new ColumnDefinition());
        var mods = _mods.Where(m => m.FileType == "package").ToList();
        var asi = _mods.Count(m => m.FileType == "asi");
        var off = mods.Count(m => !m.Enabled);
        var packs = Expansions.Installed(g);
        var eps = packs.Count(x => x.Pack.Expansion);
        var noBackup = _health.Count(h => h.Finding.Text == "The save {0} has no backup yet.");
        var pictures = Gallery.List(Docs).Where(pc => !pc.Video).ToList();
        void Count(int col, string number, string name, string note, bool warn, Action go)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
            row.Children.Add(new TextBlock { Text = number, FontSize = 26, FontWeight = FontWeights.Bold, VerticalAlignment = VerticalAlignment.Center });
            var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            words.Children.Add(new TextBlock { Text = name, FontSize = 13, FontWeight = FontWeights.SemiBold });
            words.Children.Add(new TextBlock { Text = note, FontSize = 12, Foreground = warn ? Res("ApexWarningBrush") : Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            row.Children.Add(words);
            var card = new Border { CornerRadius = new CornerRadius(16), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), Padding = new Thickness(20, 14, 20, 14), Child = row };
            var tile = Tile(card, name, go);
            Grid.SetColumn(tile, col);
            counts.Children.Add(tile);
        }
        Count(0, mods.Count.ToString(), T("Mods"), (off == 0 ? T("all on") : T("{0} off", off)) + (asi > 0 ? " · " + (asi == 1 ? T("1 game plugin") : T("{0} game plugins", asi)) : ""), false, () => Show(ModsScreen));
        Count(1, packs.Count.ToString(), T("Packs"), (eps == 1 ? T("1 expansion") : T("{0} expansions", eps)) + " · " + T("{0} stuff", packs.Count - eps), false, () => Show(WorldsScreen));
        Count(2, saves.Count.ToString(), T("Saves"), noBackup == 0 ? T("all backed up") : noBackup == 1 ? T("1 without a backup") : T("{0} without a backup", noBackup), noBackup > 0, () => Show(SavesScreen));
        Count(3, pictures.Count.ToString(), T("Pictures"), pictures.Count == 0 ? T("none yet") : T("latest {0}", pictures[0].Taken.ToString("d MMM", LangCulture())), false, () => Show(GalleryScreen));
        Grid.SetRow(counts, 1);
        Grid.SetColumnSpan(counts, 2);
        top.Children.Add(counts);

        // a narrow window: the side column goes under the picture
        top.SizeChanged += (_, e) =>
        {
            var one = e.NewSize.Width < 820;
            top.ColumnDefinitions[1].Width = one ? new GridLength(0) : new GridLength(340);
            side.Visibility = one ? Visibility.Collapsed : Visibility.Visible;
        };
        return top;
    }

    // a card that is a button: the whole of it opens its screen, lifting a little under the pointer
    SizeChangedEventHandler? _homeFit;

    Button Tile(FrameworkElement content, string name, Action go)
    {
        var b = new Button { Content = content, Padding = new Thickness(0), BorderThickness = new Thickness(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch,
            CornerRadius = content switch { Border bd => bd.CornerRadius, Grid gr => gr.CornerRadius, _ => new CornerRadius(22) } }; // the card's own corners, or the button clips its border
        b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        b.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(180) };
        b.PointerEntered += (_, _) => b.Translation = new Vector3(0, -2, 0);
        b.PointerExited += (_, _) => b.Translation = Vector3.Zero;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, name);
        b.Click += (_, _) => go();
        return b;
    }

    // how long a session was, in words
    string Played(TimeSpan t) => t.TotalHours >= 1 ? T("{0} h {1} min played", (int)t.TotalHours, t.Minutes) : T("{0} min played", Math.Max(1, (int)t.TotalMinutes));

    // ---------- the picture, running into the page ----------

    // Play starts the game as it is; Update and play puts the ready updates in first (the player chooses, nothing is forced)
    UIElement PlayButtons()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        if (_running)
        {
            row.Children.Add(new Border { Background = Res("ApexSuccessSoftBrush"), CornerRadius = new CornerRadius(16), Padding = new Thickness(26, 18, 26, 18),
                Child = new TextBlock { Text = _session is { } s ? T("Playing · {0}", Duration(s.Played)) : T("Playing"), Foreground = Res("ApexSuccessBrush"), FontWeight = FontWeights.SemiBold } });
            return row;
        }
        var ups = WelcomeUpdates();
        var play = new Button { Style = (Style)Application.Current.Resources["AccentButtonStyle"], Height = 62, Padding = new Thickness(32, 0, 40, 0), CornerRadius = new CornerRadius(16), IsEnabled = !_busy && !Maintenance };
        var pc = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        pc.Children.Add(new FontIcon { Glyph = "\uF5B0", FontSize = 15, VerticalAlignment = VerticalAlignment.Center });
        var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = _healthState == HealthState.Blocked ? T("Play anyway") : T("Play"), FontSize = 18, FontWeight = FontWeights.SemiBold });
        if (ups.Count > 0) words.Children.Add(new TextBlock { Text = ups.Count == 1 ? T("1 update goes in first") : T("{0} updates go in first", ups.Count), FontSize = 12, Opacity = 0.75 });
        pc.Children.Add(words);
        play.Content = pc;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(play, "PlayButton");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(play, T("Play"));
        if (ups.Count > 0) ToolTipService.SetToolTip(play, string.Join("\n", ups.Select(c => c + " " + (_latest.GetValueOrDefault(c)?.Version ?? ""))));
        play.Click += async (_, _) => await PlayAsync(null, update: ups.Count > 0);
        row.Children.Add(play);
        if (ups.Count > 0)
        {
            var asIs = new Button { Content = T("Play without updating"), Height = 62, Padding = new Thickness(22, 0, 22, 0), CornerRadius = new CornerRadius(16), IsEnabled = !_busy && !Maintenance,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x73, 0x0C, 0x0C, 0x0C)), BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(0x29, 255, 255, 255)), Foreground = new SolidColorBrush(Microsoft.UI.Colors.White) };
            asIs.Click += async (_, _) => await PlayAsync(null);
            row.Children.Add(asIs);
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
    static UIElement SectionHead(string title, string? note = null, string? link = null, Action? go = null)
    {
        var g = new Grid { ColumnSpacing = 14, Margin = new Thickness(0, 0, 0, 16) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var t = new TextBlock { Text = title, FontSize = 22, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
        g.Children.Add(t);
        if (note is not null) { var n = new TextBlock { Text = note, FontSize = 14, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis }; Grid.SetColumn(n, 1); g.Children.Add(n); }
        if (link is not null && go is not null) { var l = Link(link, go); l.Padding = new Thickness(0); l.VerticalAlignment = VerticalAlignment.Center; Grid.SetColumn(l, 2); g.Children.Add(l); }
        return g;
    }

    // the saves as covers: the one Play starts wide, with a ring; the covers come in one after another, lean in under
    // the pointer, and Back up turns into a ring and then a tick
    UIElement HomeSaves(List<Save> saves)
    {
        var s = new StackPanel();
        s.Children.Add(SectionHead(T("Your households"), null, T("All saves"), () => Show(SavesScreen)));
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
                GradientStops = { new GradientStop { Color = Pick(0xFF242424, 0xFFF1F1F1), Offset = 0 }, new GradientStop { Color = Pick(0xFF161616, 0xFFE5E5E5), Offset = 1 } },
            } };
            if (first) { card.BorderBrush = Res("ApexAccentBrush"); card.BorderThickness = new Thickness(2); }
            var photo = new Grid();
            photo.Children.Add(new TextBlock { Text = Initials(save.Name), FontSize = first ? 64 : 48, FontWeight = FontWeights.Bold, Foreground = new SolidColorBrush(Pick(0xFF303030, 0xFFCACACA)), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 0, 60) });
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
            // straight into this save: always shown on the first, under the pointer on the others
            var name = save.Name;
            var go = new Button { Style = (Style)Application.Current.Resources["AccentButtonStyle"], Width = first ? 52 : 44, Height = first ? 52 : 44, Padding = new Thickness(0),
                CornerRadius = new CornerRadius(first ? 26 : 22), Margin = new Thickness(14), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom,
                IsEnabled = !_busy && !Maintenance && !_running, Opacity = first ? 1 : 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(180) },
                Content = new FontIcon { Glyph = "", FontSize = first ? 16 : 14, Margin = new Thickness(2, 0, 0, 0) } };
            ToolTipService.SetToolTip(go, T("Play this save"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(go, T("Play {0}", name));
            go.Click += async (_, _) => await PlayAsync(name);
            card.Children.Add(go);
            card.PointerEntered += (_, _) => { photo.Scale = new Vector3(1.06f, 1.06f, 1); go.Opacity = 1; };
            card.PointerExited += (_, _) => { photo.Scale = Vector3.One; if (!first) go.Opacity = 0; };
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

    // updates and what's new: every part with an update ready (Apex Radiance, Sims 3 Modernization and the rest), its
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
            { new GradientStop { Color = Mix(Pick(0xFF141414, 0xFFFFFFFF), Accent(), _light ? 0.16 : 0.22), Offset = 0 }, new GradientStop { Color = Pick(0xFF161616, 0xFFFFFFFF), Offset = 0.55 }, new GradientStop { Color = Pick(0xFF131313, 0xFFFAFAFA), Offset = 1 } } };
        band.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });
        band.ColumnDefinitions.Add(new ColumnDefinition());
        var lilac = Res("ApexAccentTextBrush");
        var soft = Res("ApexMutedBrush");
        var left = new StackPanel { Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        var right = new StackPanel { Spacing = 14, VerticalAlignment = VerticalAlignment.Center };
        if (ups.Count > 0)
        {
            left.Children.Add(new TextBlock { Text = T("UPDATES"), FontSize = 12, FontWeight = FontWeights.SemiBold, CharacterSpacing = 120, Foreground = lilac });
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
            left.Children.Add(new TextBlock { Text = T("WHAT'S NEW"), FontSize = 12, FontWeight = FontWeights.SemiBold, CharacterSpacing = 120, Foreground = lilac });
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

    // the pictures, a collage of the newest four
    UIElement HomePictures()
    {
        var s = new StackPanel();
        var dir = Path.Combine(Docs, "Screenshots");
        var count = 0;
        try { if (Directory.Exists(dir)) count = Directory.EnumerateFiles(dir).Count(f => Path.GetExtension(f).ToLowerInvariant() is ".jpg" or ".jpeg" or ".png"); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        var shots = Screenshots(4);
        s.Children.Add(SectionHead(T("Your pictures"), count == 0 ? null : count == 1 ? T("1 picture") : T("{0} pictures", count), count == 0 ? null : T("Screenshots folder"), () => OpenFolder(dir)));
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
}
