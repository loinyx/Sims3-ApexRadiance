using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Home, like a game's own page: the picture as a wide rounded card, the game's name low in its corner with Play (and a "more"
// menu for the rarer choices), health as a small glass note in the other corner, and rows of the newest saves, each with its
// own Play, and the newest pictures under it. Play on top opens the game's main menu; a save starts only from its own button. The picture is the player's choice in Settings: a gallery picture,
// the household's own photo, or none.
public sealed partial class MainWindow
{
    DispatcherTimer? _homeTick; // the playing time, ticking while the game runs

    void BuildHome()
    {
        WarmThumbs();
        var host = ScreenHome;
        host.Children.Clear();
        host.Spacing = 0;
        _homeTick?.Stop();
        if (_game is null)
        {
            host.Spacing = 12;
            host.Children.Add(Heading(T("Home")));
            host.Children.Add(Card(Body(T("The launcher can't find The Sims 3. Choose the folder where it is installed.")), Button(T("Choose the game"), () => Show(GameStep))));
            return;
        }
        // (the picture runs to the window's edges; every other screen gets the page margins back in Show)
        Scroller.Padding = new Thickness(0);
        Screens.Margin = new Thickness(0, 0, 0, 32);
        var saves = Saves.List(Docs);
        host.Children.Add(HomeTop(saves.FirstOrDefault()));
        var below = new StackPanel { Spacing = 28, Padding = new Thickness(32, 28, 32, 0) };
        if (_healthState == HealthState.Blocked)
            below.Children.Add(Banner(InfoBarSeverity.Error, T("Apex Radiance can't start"), T("The game still starts, but without Apex Radiance until this is fixed."), T("Open Health"), () => Show(HealthScreen)));
        if (_offline) below.Children.Add(Banner(InfoBarSeverity.Warning, T("No internet"), T("You can still play. Updates wait until you're back online."), T("Try again"), async () => await LoadReleasesAsync()));
        // the last session shows only when it went wrong: straight back into the newest save
        if (!_running && SessionHistory.List().FirstOrDefault() is { End: SessionEnd.Crash or SessionEnd.LowMemory } last && Saves.Newest(Docs) is { } newest)
            below.Children.Add(Banner(InfoBarSeverity.Warning, last.End == SessionEnd.Crash ? T("The game closed on its own") : T("Ran out of memory"),
                Played(last.Played) + " · " + T("memory peaked at {0}", ModRow.SizeText(last.PeakBytes)), T("Open this save again"), async () => await PlayAsync(newest.Name)));
        if (saves.Count > 0) below.Children.Add(HomeSaves(saves.Take(4).ToList()));
        below.Children.Add(HomePictures());
        host.Children.Add(below);
        if (!_homeFitHooked) { _homeFitHooked = true; Scroller.SizeChanged += (_, _) => { if (_step == Home) FitHome(); }; }
        FitHome();
    }

    // a row's head on Home: its title and a quiet link on the right
    static UIElement Head(string title, string? link, Action? go)
    {
        var g = new Grid();
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(new TextBlock { Text = title, Style = (Style)Application.Current.Resources["ApexSection"], VerticalAlignment = VerticalAlignment.Center });
        if (link is not null && go is not null)
        {
            var l = Link(link, go);
            l.Padding = new Thickness(0);
            l.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(l, 1);
            g.Children.Add(l);
        }
        return g;
    }

    // when a save was made: in words while it is recent, else its day
    string SavedWhen(DateTime t)
    {
        var c = LangCulture();
        var days = (DateTime.Today - t.Date).Days;
        return days <= 0 ? T("Saved today at {0}", t.ToString("t", c))
            : days == 1 ? T("Saved yesterday at {0}", t.ToString("t", c))
            : Day(t);
    }

    // how long a session was, in words
    string Played(TimeSpan t) => t.TotalHours >= 1 ? T("{0} h {1} min played", (int)t.TotalHours, t.Minutes) : T("{0} min played", Math.Max(1, (int)t.TotalMinutes));

    static Windows.UI.Color A(Windows.UI.Color c, byte a) => Windows.UI.Color.FromArgb(a, c.R, c.G, c.B);
    static LinearGradientBrush Fade(Windows.UI.Color c, (double At, byte A)[] stops, bool across = false)
    {
        var b = new LinearGradientBrush { StartPoint = across ? new(0, 0) : new(0, 1), EndPoint = across ? new(1, 0) : new(0, 0) };
        foreach (var (at, a) in stops) b.GradientStops.Add(new GradientStop { Color = A(c, a), Offset = at });
        return b;
    }

    // ---------- the top: the picture, the game and Play ----------

    // newest: the save whose household stands on the plain card
    UIElement HomeTop(Save? newest)
    {
        var mode = _settings.HomePicture;
        var shot = mode == "gallery" ? HomePicture() : null;
        var onPhoto = _homeOnPhoto = shot is not null;
        var g = _homeHero = new Grid { Height = onPhoto ? 520 : double.NaN };
        // on the picture the words are white on every theme; on the plain card they take the theme's own colours
        Brush text = onPhoto ? new SolidColorBrush(Microsoft.UI.Colors.White) : Res("TextFillColorPrimaryBrush");
        Brush soft = onPhoto ? new SolidColorBrush(Windows.UI.Color.FromArgb(0xE6, 255, 255, 255)) : Res("ApexSoftTextBrush");

        var pic = new Grid();
        if (shot is not null)
        {
            // the picture as a rounded card inside the page's margins, at full strength unless Settings softens it, with a soft
            // shade in the corner where the words sit so they read on any picture without a box over it
            pic.Children.Add(new Image { Source = Thumb(shot, 2000), Stretch = Stretch.UniformToFill, VerticalAlignment = VerticalAlignment.Center, Opacity = Math.Clamp(_settings.HomePictureStrength, 30, 100) / 100.0 });
            var black = Windows.UI.Color.FromArgb(255, 8, 8, 8);
            pic.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(black, [(0, 0xC8), (0.3, 0x66), (0.6, 0)]) });
            pic.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(black, [(0, 0x8C), (0.35, 0x26), (0.55, 0)], across: true) });
        }
        else
        {
            // no picture: a plain card, lit softly from the right by the accent, with the household standing there when chosen
            var accent = ((SolidColorBrush)Res("ApexAccentBrush")).Color;
            pic.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(accent, [(0, 0), (0.55, 0), (1, 0x1C)], across: true) });
            if (mode == "household" && newest is not null)
            {
                // (low on the right, under the health note; the game stores these photos small, so they stay small)
                var photo = new Grid { HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Height = 170, Margin = new Thickness(0, 0, 96, 12) };
                pic.Children.Add(photo);
                _ = Task.Run(() => Saves.Photo(newest)).ContinueWith(async t =>
                {
                    if (!t.IsFaulted && await SaveCard.SimsOf(t.Result) is { } img) photo.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Bottom });
                }, TaskScheduler.FromCurrentSynchronizationContext());
            }
        }
        g.Children.Add(new Border { Child = pic, CornerRadius = new CornerRadius(12), Margin = new Thickness(32, 24, 32, 0), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexCardBrush") });

        // the game, low in the corner of the picture or centred on the plain card
        var words = new StackPanel { Spacing = 6, HorizontalAlignment = HorizontalAlignment.Left };
        _homeWords = words;
        words.Children.Add(_homeName = new TextBlock
        {
            Text = "The Sims 3", FontFamily = new FontFamily("Segoe UI Variable Display"), FontSize = 48, FontWeight = FontWeights.SemiBold,
            TextTrimming = TextTrimming.CharacterEllipsis, Foreground = text,
        });
        words.Children.Add(new TextBlock { Text = _game is { } game ? string.Join(" \u00b7 ", new[] { (game.Label + " " + game.Version).Trim(), _state?.ApexVersion is { Length: > 0 } av ? "Apex Radiance " + av : "" }.Where(p => p.Length > 0)) : "", FontSize = 14, Foreground = soft });
        var acts = HomeActions();
        acts.Margin = new Thickness(0, 16, 0, 0);
        words.Children.Add(acts);
        words.VerticalAlignment = onPhoto ? VerticalAlignment.Bottom : VerticalAlignment.Center;
        words.Margin = onPhoto ? new Thickness(64, 0, 40, 40) : new Thickness(64, 64, 40, 40);
        g.Children.Add(words);

        var chip = HealthChip();
        chip.HorizontalAlignment = HorizontalAlignment.Right;
        chip.VerticalAlignment = VerticalAlignment.Top;
        chip.Margin = new Thickness(0, 40, 48, 0);
        g.Children.Add(chip);
        return g;
    }

    Grid? _homeHero;
    StackPanel? _homeWords;
    TextBlock? _homeName;
    bool _homeFitHooked, _homeOnPhoto;

    // the top keeps the picture's own wide shape as the window grows, within the share of the window Settings' picture
    // height allows (a plain card takes the height of its words); the name grows a little with it
    void FitHome()
    {
        double w = Scroller.ActualWidth, h = Scroller.ActualHeight;
        if (_homeHero is null || w <= 0 || h <= 0) return;
        var share = Math.Clamp(_settings.HomePictureHeight / 480.0 * 0.9, 0.42, 0.88);
        if (_homeOnPhoto) _homeHero.Height = Math.Clamp(w * 0.44, 360, Math.Max(360, h * share));
        if (_homeName is not null) { _homeName.FontSize = Math.Clamp(w / 32, 40, 64); _homeName.LineHeight = _homeName.FontSize * 1.08; }
        if (_homeWords is not null) _homeWords.MaxWidth = Math.Max(560, w * 0.42);
    }

    // Play opens the game's main menu; the "more" menu holds playing without the updates and all the saves
    StackPanel HomeActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        // (on the picture: white on frosted white; on the plain card: the theme's own fills)
        Brush glass = _homeOnPhoto ? new SolidColorBrush(Windows.UI.Color.FromArgb(0x1F, 255, 255, 255)) : Res("ApexPillBrush");
        Brush ink = _homeOnPhoto ? new SolidColorBrush(Microsoft.UI.Colors.White) : Res("TextFillColorPrimaryBrush");
        Brush inkSoft = _homeOnPhoto ? new SolidColorBrush(Windows.UI.Color.FromArgb(0xCC, 255, 255, 255)) : Res("ApexSoftTextBrush");
        if (_running)
        {
            var playing = new TextBlock { FontWeight = FontWeights.SemiBold, Foreground = ink, VerticalAlignment = VerticalAlignment.Center };
            var pill = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            pill.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center });
            pill.Children.Add(playing);
            row.Children.Add(new Border { Child = pill, Background = glass, CornerRadius = new CornerRadius(8), Padding = new Thickness(18, 0, 20, 0), Height = 56 });
            var mem = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            var bar = new ProgressBar { Width = 160, Maximum = 1, VerticalAlignment = VerticalAlignment.Center };
            var used = new TextBlock { FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Foreground = inkSoft };
            mem.Children.Add(new TextBlock { Text = T("Memory"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Foreground = inkSoft });
            mem.Children.Add(bar);
            mem.Children.Add(used);
            row.Children.Add(mem);
            // the time and the memory the game uses, every second (the memory is the session's own sample, taken every 5 s)
            void Tick()
            {
                playing.Text = _session is { } s ? T("Playing · {0}", Duration(s.Played)) : T("Playing");
                var bytes = _session?.MemoryBytes ?? 0;
                mem.Visibility = bytes > 0 ? Visibility.Visible : Visibility.Collapsed;
                bar.Value = Math.Min(1, bytes / (double)Session.LowMemoryBytes);
                used.Text = T("{0} of {1}", ModRow.SizeText(bytes), ModRow.SizeText(Session.LowMemoryBytes));
            }
            Tick();
            _homeTick ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _homeTick.Tick -= OnHomeTick;
            _homeTick.Tick += OnHomeTick;
            _homeTickAction = Tick;
            _homeTick.Start();
            return row;
        }
        var update = WelcomeUpdates().Count > 0 && _settings.InstallUpdatesOnPlay;
        var blocked = _healthState == HealthState.Blocked;
        row.Children.Add(PlayButton(null));

        var menu = new MenuFlyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedLeft };
        if (update && !blocked)
        {
            var asIs = new MenuFlyoutItem { Text = T("Play without updating"), Icon = Lucide.Element("play"), IsEnabled = !_busy && !Maintenance };
            asIs.Click += async (_, _) => await PlayAsync(null);
            menu.Items.Add(asIs);
            menu.Items.Add(new MenuFlyoutSeparator());
        }
        var all = new MenuFlyoutItem { Text = T("All saves"), Icon = Lucide.Element("save") };
        all.Click += (_, _) => Show(SavesScreen);
        menu.Items.Add(all);
        var more = new Button
        {
            Width = 56, Height = 56, Padding = new Thickness(0), CornerRadius = new CornerRadius(8), Background = glass, BorderThickness = new Thickness(0), Flyout = menu,
            Content = new LucideIcon { Kind = "ellipsis", Size = 16, Stroke = ink, StrokeWidth = 1.75 },
        };
        ToolTipService.SetToolTip(more, T("More options"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(more, T("More options"));
        row.Children.Add(more);
        return row;
    }

    // the large Play: the game's main menu, or straight into a save
    Button PlayButton(string? save)
    {
        var ups = WelcomeUpdates();
        var update = ups.Count > 0 && _settings.InstallUpdatesOnPlay;
        var blocked = _healthState == HealthState.Blocked;
        var play = new Button { Height = 56, Padding = new Thickness(22, 0, 28, 0), CornerRadius = new CornerRadius(8), IsEnabled = !_busy && !Maintenance };
        if (!blocked) play.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        var pc = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        pc.Children.Add(new LucideIcon { Solid = true, Kind = "play", Size = 14, VerticalAlignment = VerticalAlignment.Center, Follow = true, StrokeWidth = 1.75 });
        var label = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        var title = blocked ? T("Play anyway") : T("Play");
        label.Children.Add(new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeights.SemiBold });
        var sub = blocked ? T("The game starts without Apex Radiance") : update ? (ups.Count == 1 ? T("1 update installs first") : T("{0} updates install first", ups.Count)) : null;
        if (sub is not null) label.Children.Add(new TextBlock { Text = sub, FontSize = 12, Opacity = 0.75 });
        pc.Children.Add(label);
        play.Content = pc;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(play, save is null ? "PlayButton" : "PlaySaveButton");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(play, save is null ? title : T("Play {0}", save));
        if (update) ToolTipService.SetToolTip(play, string.Join("\n", ups.Select(c => Name(c) + " " + (ReleaseOf(c)?.Version ?? ""))));
        play.Click += async (_, _) => await PlayAsync(save, update: update);
        return play;
    }

    Action? _homeTickAction;
    void OnHomeTick(object? sender, object e)
    {
        if (!_running || _step != Home) { _homeTick?.Stop(); return; }
        _homeTickAction?.Invoke();
    }

    // ---------- health: a small note in the picture's corner ----------

    // The game's health over the picture: a low glass card with the crystal larger than it, over its left edge, in a light of
    // the state's colour; the state in words, the first thing to look at, and a chevron: the whole card opens Health. The
    // glass is Play and More's (frosted on the picture, the theme's own on the plain card), its corners a card's.
    FrameworkElement HealthChip()
    {
        var calm = _healthState == HealthState.Ready;
        // (as Health's own list: a finding first, else the mods that clash, by their title)
        var first = _health.FirstOrDefault(h => h.Finding.Severity == Severity.Error && !h.Conflict) ?? _health.FirstOrDefault(h => h.Finding.Severity != Severity.Info && !h.Conflict);
        var clashes = ConflictsNow().Where(c => c.Level != ConflictLevel.Small).ToList();
        var note = calm ? T("Every check passed") : first is not null ? Sentences(T(first.Finding.Text, first.Finding.Args)).Item1.TrimEnd('.')
            : clashes.Count == 1 ? T(clashes[0].Title) : clashes.Count > 1 ? T("{0} conflicts between mods", clashes.Count) : "";
        // (the card says the state in words; Health's own title counts the findings)
        var title = _healthState == HealthState.NeedsYou ? T("Needs your attention") : HealthTitle();
        var photo = _homeOnPhoto;
        Brush ink = photo ? new SolidColorBrush(Microsoft.UI.Colors.White) : Res("TextFillColorPrimaryBrush");
        Brush soft = photo ? new SolidColorBrush(Alpha(Microsoft.UI.Colors.White, 0xCC)) : Res("ApexSoftTextBrush");

        const double size = 64, light = 150, over = 26;
        var root = new Grid();
        // the card, starting under the crystal
        var g = new Grid { Padding = new Thickness(size - over + Design.Space.X3, Design.Space.X3, Design.Space.X4, Design.Space.X3), ColumnSpacing = Design.Space.X3,
            MinHeight = 56 };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var black = Windows.UI.Color.FromArgb(255, 0, 0, 0);
        Brush rest = photo ? Glass(0.42, black) : Res("ApexPillBrush"), hover = photo ? Glass(0.3, black) : Res("ApexHoverBrush");
        var card = new Border { Child = g, Margin = new Thickness(over, 0, 0, 0), CornerRadius = new CornerRadius(Design.Radius.Card), Background = rest,
            BorderThickness = new Thickness(1), BorderBrush = photo ? new SolidColorBrush(Alpha(Microsoft.UI.Colors.White, 0x24)) : Res("ApexCardBorderBrush"),
            VerticalAlignment = VerticalAlignment.Center };
        root.Children.Add(card);
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = title, FontSize = Design.TextSize.Body, FontWeight = FontWeights.SemiBold, Foreground = ink, TextTrimming = TextTrimming.CharacterEllipsis });
        if (note.Length > 0) words.Children.Add(new TextBlock { Text = note, FontSize = Design.TextSize.Caption, Foreground = soft, TextTrimming = TextTrimming.CharacterEllipsis });
        g.Children.Add(words);
        var go = Lucide.Icon("chevron-right", Design.Size.IconSmall, soft);
        go.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(go, 1);
        g.Children.Add(go);
        // the crystal over the card's edge, in its light (the light stays still and breathes; the crystal floats)
        var pulse = new ScaleTransform();
        // (a round light that fades the same way on every side: SoftGlow is made for a page's top edge)
        var tint = StateTint(255);
        var fade = new Microsoft.UI.Xaml.Media.RadialGradientBrush();
        foreach (var (at, share) in new[] { (0.0, 1.0), (0.25, 0.8), (0.45, 0.48), (0.65, 0.2), (0.82, 0.06), (1.0, 0.0) })
            fade.GradientStops.Add(new GradientStop { Offset = at, Color = Alpha(tint, (byte)(share * (calm ? 0xB0 : 0xD8))) });
        var glow = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = light, Height = light, Fill = fade, RenderTransform = pulse, RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5) };
        var bob = new CompositeTransform();
        var holder = new Grid { Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Center, RenderTransform = bob };
        var crystal = new Crystal(size) { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        crystal.SetLevel(HealthLevel, animate: false);
        holder.Children.Add(crystal);
        var gem = new Grid { Width = size, Height = size + 8, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Center, IsHitTestVisible = false };
        gem.Children.Add(holder);
        root.Children.Add(gem);
        // (the light lies behind the button, so nothing clips it; the glass blurs what falls under it)
        // (a canvas, as a grid would cut the light at its own edges)
        var halo = new Canvas { Width = size, Height = size + 8, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Center, IsHitTestVisible = false };
        Canvas.SetLeft(glow, -(light - size) / 2);
        Canvas.SetTop(glow, -(light - size - 8) / 2);
        halo.Children.Add(glow);
        if (new Windows.UI.ViewManagement.UISettings().AnimationsEnabled)
        {
            var float_ = new Microsoft.UI.Xaml.Media.Animation.Storyboard { RepeatBehavior = Microsoft.UI.Xaml.Media.Animation.RepeatBehavior.Forever, AutoReverse = true };
            var up = new Microsoft.UI.Xaml.Media.Animation.DoubleAnimation { From = 0, To = -3, Duration = TimeSpan.FromMilliseconds(1800),
                EasingFunction = new Microsoft.UI.Xaml.Media.Animation.SineEase { EasingMode = Microsoft.UI.Xaml.Media.Animation.EasingMode.EaseInOut } };
            Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTarget(up, bob);
            Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTargetProperty(up, "TranslateY");
            float_.Children.Add(up);
            // the light grows and settles slowly, out of step with the bob
            var grow = new Microsoft.UI.Xaml.Media.Animation.Storyboard { RepeatBehavior = Microsoft.UI.Xaml.Media.Animation.RepeatBehavior.Forever, AutoReverse = true };
            foreach (var axis in new[] { "ScaleX", "ScaleY" })
            {
                var s = new Microsoft.UI.Xaml.Media.Animation.DoubleAnimation { From = 0.88, To = 1.1, Duration = TimeSpan.FromMilliseconds(3400),
                    EasingFunction = new Microsoft.UI.Xaml.Media.Animation.SineEase { EasingMode = Microsoft.UI.Xaml.Media.Animation.EasingMode.EaseInOut } };
                Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTarget(s, pulse);
                Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTargetProperty(s, axis);
                grow.Children.Add(s);
            }
            var breathe = new Microsoft.UI.Xaml.Media.Animation.DoubleAnimation { From = 0.75, To = 1, Duration = TimeSpan.FromMilliseconds(3400),
                EasingFunction = new Microsoft.UI.Xaml.Media.Animation.SineEase { EasingMode = Microsoft.UI.Xaml.Media.Animation.EasingMode.EaseInOut } };
            Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTarget(breathe, glow);
            Microsoft.UI.Xaml.Media.Animation.Storyboard.SetTargetProperty(breathe, "Opacity");
            grow.Children.Add(breathe);
            holder.Loaded += (_, _) => { float_.Begin(); grow.Begin(); };
            holder.Unloaded += (_, _) => { float_.Stop(); grow.Stop(); };
        }

        // the whole thing is the button; only the glass answers the pointer
        var b = new Button { Content = root, Padding = new Thickness(0), BorderThickness = new Thickness(0), Background = Res("ApexClearBrush"), MaxWidth = 420,
            HorizontalContentAlignment = HorizontalAlignment.Stretch };
        foreach (var key in new[] { "ButtonBackgroundPointerOver", "ButtonBackgroundPressed" }) b.Resources[key] = Res("ApexClearBrush");
        b.PointerEntered += (_, _) => card.Background = hover;
        b.PointerExited += (_, _) => card.Background = rest;
        b.PointerCanceled += (_, _) => card.Background = rest;
        b.Click += (_, _) => Show(HealthScreen);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, title + (note.Length > 0 ? ". " + note : "") + ". " + T("Open Health"));
        HandCursor.On(b);
        var all = new Grid();
        all.Children.Add(halo);
        all.Children.Add(b);
        return all;
    }

    static Windows.UI.Color Alpha(Windows.UI.Color c, byte a) => Windows.UI.Color.FromArgb(a, c.R, c.G, c.B);
    // the dark glass, with a breath of the state's colour in it
    static Microsoft.UI.Xaml.Media.AcrylicBrush Glass(double tint, Windows.UI.Color hue) => new()
    {
        TintColor = Windows.UI.Color.FromArgb(255, (byte)(20 + hue.R * 0.12), (byte)(20 + hue.G * 0.12), (byte)(20 + hue.B * 0.12)), TintOpacity = tint, TintLuminosityOpacity = 0.3,
        FallbackColor = Windows.UI.Color.FromArgb(0xD0, 20, 20, 20),
    };

    static Microsoft.UI.Xaml.Media.AcrylicBrush Glass(double tint) => new()
    {
        TintColor = Windows.UI.Color.FromArgb(255, 20, 20, 20), TintOpacity = tint, TintLuminosityOpacity = 0.25,
        FallbackColor = Windows.UI.Color.FromArgb(0xC8, 20, 20, 20),
    };

    // ---------- the newest saves: a row of tiles, each with its household and its own Play ----------

    readonly Dictionary<string, string> _homeNames = new(); // (a tile made again shows its family at once)
    UIElement HomeSaves(List<Save> saves)
    {
        var s = new StackPanel { Spacing = 10 };
        s.Children.Add(Head(T("Your saves"), T("All saves"), () => Show(SavesScreen)));
        var grid = new Grid { ColumnSpacing = 16, RowSpacing = 16 };
        for (int i = 0; i < 4; i++) grid.ColumnDefinitions.Add(new ColumnDefinition());
        var docs = Docs;
        var white = new SolidColorBrush(Microsoft.UI.Colors.White);
        var rows = new List<(Save Save, Grid Photo, TextBlock Title, TextBlock Meta)>();
        var tiles = new List<Grid>();
        for (int i = 0; i < saves.Count; i++)
        {
            var save = saves[i];
            var name = save.Name;
            // a stage for the household: a soft light behind them, a shadow at their feet, the name at the foot
            var tile = new Grid { Background = Fade(((SolidColorBrush)Res("ApexRaisedBrush")).Color, [(0, 0xFF), (1, 0x80)]) };
            var photo = new Grid();
            tile.Children.Add(photo);
            var foot = new StackPanel { VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(16, 0, 72, 14), Spacing = 1 };
            var title = new TextBlock { Text = _homeNames.GetValueOrDefault(save.Dir) ?? name, FontSize = 16, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
            foot.Children.Add(title);
            var meta = Muted(Day(save.LastPlayed));
            meta.TextWrapping = TextWrapping.NoWrap;
            meta.TextTrimming = TextTrimming.CharacterEllipsis;
            foot.Children.Add(meta);
            tile.Children.Add(foot);
            // Play in the corner, straight into this save (the updates first, as Play on top)
            var play = new Button
            {
                Height = PlaySize, CornerRadius = new CornerRadius(PlaySize / 2), Padding = new Thickness(0), BorderThickness = new Thickness(0),
                IsEnabled = !_busy && !Maintenance && !_running,
            };
            ToolTipService.SetToolTip(play, T("Play this save"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(play, T("Play {0}", name));
            play.Click += async (_, _) => await PlayAsync(name, update: _settings.InstallUpdatesOnPlay);
            var cell = new Grid();
            cell.Children.Add(new Border { Child = tile, CornerRadius = new CornerRadius(12), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1) });
            var (corner, over) = PlayCorner(play);
            cell.Children.Add(corner);
            ToolTipService.SetToolTip(tile, SavedWhen(save.LastPlayed));
            cell.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(200) };
            cell.PointerEntered += (_, _) => { cell.Translation = new(0, -4, 0); over(true); };
            cell.PointerExited += (_, _) => { cell.Translation = default; over(false); };
            Grid.SetColumn(cell, i);
            grid.Children.Add(cell);
            tiles.Add(tile);
            rows.Add((save, photo, title, meta));
        }
        // tiles keep their shape as the window grows; a narrow window: two a row
        grid.SizeChanged += (_, e) =>
        {
            var two = e.NewSize.Width < 880; // (a household's name keeps its room)
            while (grid.RowDefinitions.Count < 2) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var cols = two ? 2 : 4;
            while (grid.ColumnDefinitions.Count > cols) grid.ColumnDefinitions.RemoveAt(cols); // (an empty column would still take its gap)
            while (grid.ColumnDefinitions.Count < cols) grid.ColumnDefinitions.Add(new ColumnDefinition());
            var tw = (e.NewSize.Width - 16 * (cols - 1)) / cols;
            foreach (var t in tiles) t.Height = Math.Clamp(tw * 0.6, 200, 250);
            for (int i = 0; i < grid.Children.Count; i++) { var el = (FrameworkElement)grid.Children[i]; Grid.SetColumn(el, two ? i % 2 : i); Grid.SetRow(el, two ? i / 2 : 0); }
        };
        s.Children.Add(grid);
        var backup = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Visibility = Visibility.Collapsed };
        s.Children.Add(backup);

        // the photos, sizes and backups read off the UI thread
        _ = Task.Run(() => (SaveHealth.Check(docs, record: false), saves.Select(Saves.Photo).ToList(), saves.Select(x => FamilyName(x, SaveInfo.Of(x))).ToList())).ContinueWith(async t =>
        {
            if (t.IsFaulted) return;
            var (reports, photos, names) = t.Result;
            var bare = new List<Save>();
            for (int i = 0; i < rows.Count; i++)
            {
                var (save, photo, title, meta) = rows[i];
                // (two saves of one household differ by the save's name, said where the size would be)
                title.Text = _homeNames[save.Dir] = names[i];
                var same = names.Count(n => n == names[i]) > 1;
                if (reports.FirstOrDefault(r => r.Save.Dir == save.Dir) is { } r)
                {
                    meta.Text = Day(save.LastPlayed) + " · " + (same ? save.Name : ModRow.SizeText(r.Bytes));
                    if (r.LastBackup is null) bare.Add(save);
                }
                if (await SaveCard.SimsOf(photos[i]) is { } img) photo.Children.Add(new Image { Source = img, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 16, 0, 58) });
            }
            BackupLine(backup, bare, docs);
        }, TaskScheduler.FromCurrentSynchronizationContext());
        return s;
    }

    const double PlaySize = 44;

    // a save's round Play in its card's corner, a dark green circle. Under the pointer anywhere on the card, or with the
    // keyboard on it, the accent rises inside like a liquid (a turning rounded square, larger than the circle) and the triangle
    // turns dark; leaving, it goes back down. Without a wait when Windows' animation effects are off.
    static (FrameworkElement Corner, Action<bool> Over) PlayCorner(Button play)
    {
        const float size = (float)PlaySize, wave = 100, empty = 48, full = -22;
        var none = Res("ApexClearBrush");
        foreach (var state in new[] { "", "PointerOver", "Pressed", "Disabled" }) play.Resources["ButtonBackground" + state] = none;
        play.Background = none;
        var accent = Res("ApexAccentBrush");
        FrameworkElement Tri(Brush fill)
        {
            var path = new Microsoft.UI.Xaml.Shapes.Path
            {
                Data = (Geometry)Microsoft.UI.Xaml.Markup.XamlBindingHelper.ConvertValue(typeof(Geometry), "M3.5 2.2c0-0.9 1-1.5 1.8-1l8 5c0.8 0.5 0.8 1.6 0 2.1l-8 5c-0.8 0.5-1.8-0.1-1.8-1z"),
                Fill = fill, Width = 16, Height = 16,
            };
            return new Canvas { Width = 16, Height = 16, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(13, 14, 0, 0), Children = { path } };
        }
        var liquid = new Microsoft.UI.Xaml.Shapes.Rectangle { Width = wave, Height = wave, RadiusX = wave * 0.42, RadiusY = wave * 0.42, Fill = accent };
        Canvas.SetLeft(liquid, -28);
        var (light, dark) = (Tri(accent), Tri(Res("ApexOnAccentBrush")));
        var inside = new Grid { Width = size, Height = size, IsHitTestVisible = false, Opacity = play.IsEnabled ? 1 : 0.45,
            Background = Res("ApexAccentSoftBrush") };
        foreach (var e in new UIElement[] { new Canvas { Children = { liquid } }, light, dark }) inside.Children.Add(e);
        (play.Width, play.Content) = (size, inside);
        (play.HorizontalAlignment, play.VerticalAlignment, play.Margin) = (HorizontalAlignment.Right, VerticalAlignment.Bottom, new Thickness(0, 0, 14, 14));

        Microsoft.UI.Composition.Visual V(UIElement e) => ElementCompositionPreview.GetElementVisual(e);
        var (pv, iv, wv, lv, dv) = (V(play), V(inside), V(liquid), V(light), V(dark));
        var c = pv.Compositor;
        pv.CenterPoint = new(size / 2, size / 2, 0);
        var round = c.CreateRoundedRectangleGeometry();
        (round.Size, round.CornerRadius) = (new(size, size), new(size / 2, size / 2));
        iv.Clip = c.CreateGeometricClip(round);
        ElementCompositionPreview.SetIsTranslationEnabled(liquid, true);
        wv.Properties.InsertVector3("Translation", new(0, empty, 0));
        wv.CenterPoint = new(wave / 2, wave / 2, 0);
        dv.Opacity = 0;

        var moves = AnimationsOn;
        var ease = c.CreateCubicBezierEasingFunction(new(0.22f, 1f), new(0.36f, 1f));
        var soft = c.CreateCubicBezierEasingFunction(new(0.25f, 0.1f), new(0.25f, 1f));
        TimeSpan Ms(int ms) => TimeSpan.FromMilliseconds(moves ? ms : 1);
        void To(Microsoft.UI.Composition.Visual v, string what, float to, int ms, Microsoft.UI.Composition.CompositionEasingFunction how, int wait = 0)
        {
            var a = c.CreateScalarKeyFrameAnimation();
            a.InsertKeyFrame(1, to, how);
            (a.Duration, a.DelayTime) = (Ms(ms), TimeSpan.FromMilliseconds(moves ? wait : 0));
            v.StartAnimation(what, a);
        }
        var (over, focus, shown) = (false, false, false);
        void Press()
        {
            var a = c.CreateVector3KeyFrameAnimation();
            var to = play.IsPressed ? 0.9f : shown ? 1.08f : 1f;
            a.InsertKeyFrame(1, new(to, to, 1), ease);
            a.Duration = Ms(play.IsPressed ? 100 : 320);
            pv.StartAnimation("Scale", a);
        }
        void Open()
        {
            var on = play.IsEnabled && (over || focus);
            if (on == shown) return;
            shown = on;
            To(wv, "Translation.Y", on ? full : empty, 700, ease);
            To(wv, "RotationAngleInDegrees", on ? 150 : 0, 700, ease);
            To(dv, "Opacity", on ? 1 : 0, 200, soft, 120);
            To(lv, "Opacity", on ? 0 : 1, 200, soft, 120);
            Press();
        }
        play.RegisterPropertyChangedCallback(Microsoft.UI.Xaml.Controls.Primitives.ButtonBase.IsPressedProperty, (_, _) => Press());
        play.GotFocus += (_, _) => { focus = play.FocusState == FocusState.Keyboard; Open(); };
        play.LostFocus += (_, _) => { focus = false; Open(); };
        return (play, on => { over = on; Open(); });
    }

    // a card that rises a little under the pointer, as the gallery's do
    static void Lift(FrameworkElement el, Action? enter = null, Action? exit = null)
    {
        el.SizeChanged += (_, e) => el.CenterPoint = new System.Numerics.Vector3((float)e.NewSize.Width / 2, (float)e.NewSize.Height / 2, 0);
        el.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(200) };
        el.ScaleTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(200) };
        el.PointerEntered += (_, _) => { el.Translation = new System.Numerics.Vector3(0, -4, 0); el.Scale = new System.Numerics.Vector3(1.02f, 1.02f, 1); enter?.Invoke(); };
        el.PointerExited += (_, _) => { el.Translation = System.Numerics.Vector3.Zero; el.Scale = System.Numerics.Vector3.One; exit?.Invoke(); };
    }

    // one quiet line for the saves with no copy, and one link that copies them all
    void BackupLine(StackPanel line, List<Save> bare, string docs)
    {
        line.Children.Clear();
        line.Visibility = bare.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        if (bare.Count == 0) return;
        var icon = Lucide.Icon("hard-drive", 14, Res("ApexMutedBrush"));
        icon.VerticalAlignment = VerticalAlignment.Center;
        line.Children.Add(icon);
        var text = Muted(bare.Count == 1 ? T("1 of these saves has no backup") : T("{0} of these saves have no backup", bare.Count));
        text.VerticalAlignment = VerticalAlignment.Center;
        line.Children.Add(text);
        var now = Link(bare.Count == 1 ? T("Back up now") : T("Back up all"), () => { });
        now.Padding = new Thickness(0);
        now.VerticalAlignment = VerticalAlignment.Center;
        line.Children.Add(now);
        now.Click += async (_, _) =>
        {
            if (_savesBusy) return;
            // (a test run, or the game writing its saves: nothing is copied, as on the Saves screen)
            if (SavesBlocked() is { } why) { await Message(T("Back up now"), why); return; }
            SavesBusy(true); // (the window stays open until they are made)
            now.IsEnabled = false;
            now.Content = new ProgressRing { IsActive = true, Width = 12, Height = 12 };
            var left = new List<Save>();
            foreach (var save in bare)
                try { await Task.Run(() => Saves.Backup(save, docs)); }
                catch (Exception e) { App.Log(e); left.Add(save); ToolTipService.SetToolTip(line, ErrorText(e)); }
            SavesBusy(false);
            BackupLine(line, left, docs);
        };
    }

    // ---------- updates and notes (also the updates-first dialog's rows) ----------

    Release? ReleaseOf(string c) => c == Components.Modernization ? ModRelease() : Repo.TryGetValue(c, out var repo) ? _latest.GetValueOrDefault(repo) : null;
    string? InstalledOf(string c) => c == Components.Apex ? _state?.ApexVersion : Known(c) is { Length: > 0 } v ? v : _state?.Installed(c);
    static List<string> Paragraphs(string? md) => (md ?? "").Replace("\r", "").Split("\n\n").Select(p => p.Trim()).Where(p => p.Length > 0 && !p.StartsWith('#') && !p.StartsWith('-')).Select(Plain).ToList();

    // ---------- the pictures: one row of the newest ----------

    UIElement HomePictures()
    {
        var s = new StackPanel { Spacing = 12 };
        var all = Gallery.List(Docs).Where(p => !p.Video).ToList();
        s.Children.Add(Head(T("Your pictures"), all.Count == 0 ? null : T("Gallery"), () => Show(GalleryScreen)));
        if (all.Count == 0) { s.Children.Add(Muted(T("Pictures you take in the game appear here."))); return s; }
        var shown = all.Take(6).ToList();
        var row = new Grid { ColumnSpacing = 12 };
        var cells = new List<Button>();
        var culture = LangCulture();
        var white = new SolidColorBrush(Microsoft.UI.Colors.White);
        for (int i = 0; i < 6; i++)
        {
            row.ColumnDefinitions.Add(new ColumnDefinition());
            if (i >= shown.Count) continue;
            var p = shown[i];
            var at = i;
            var g = new Grid();
            g.Children.Add(new Image { Source = Thumb(p.Path, 520), Stretch = Stretch.UniformToFill });
            // under the pointer: when it was taken, and a way to put it on top of Home
            var over = new Grid { Opacity = 0, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) } };
            over.Children.Add(new Border { Background = Fade(Windows.UI.Color.FromArgb(255, 12, 12, 12), [(0, 0xD0), (0.6, 0)]), IsHitTestVisible = false });
            over.Children.Add(new TextBlock { Text = Day(p.Taken, hour: true), FontSize = 12, Foreground = white, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(10, 0, 10, 8) });
            g.Children.Add(over);
            var isTop = _gal?.HomePicture == p.Path || (_gal?.HomePicture is null && i == 0);
            var pin = new Button
            {
                Width = 32, Height = 32, Padding = new Thickness(0), CornerRadius = new CornerRadius(16), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
                Margin = new Thickness(0, 8, 8, 0), Opacity = isTop ? 1 : 0, BorderThickness = new Thickness(0),
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0xB8, 20, 20, 20)), OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) },
                Content = Lucide.Icon("house", 14, isTop ? Res("ApexAccentBrush") : white),
            };
            ToolTipService.SetToolTip(pin, isTop ? T("Home picture") : T("Use on Home"));
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(pin, isTop ? T("Home picture") : T("Use on Home"));
            // (shown when reached with the keyboard too, not only under the pointer)
            pin.GotFocus += (_, _) => pin.Opacity = 1;
            pin.LostFocus += (_, _) => pin.Opacity = isTop ? 1 : 0;
            pin.Click += (_, _) =>
            {
                if (isTop) return;
                (_gal ??= LoadGallery()).HomePicture = p.Path;
                Gallery.Save(_gal);
                if (_settings.HomePicture != "gallery") { _settings.HomePicture = "gallery"; _settings.Save(); }
                BuildHome();
                Scroller.ChangeView(null, 0, null);
            };
            var b = new Button
            {
                Content = g, Padding = new Thickness(0), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(0), HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch, Background = Res("ApexPillBrush"),
            };
            b.Click += (_, _) => OpenViewer(all, at); // (no flight from the small copy: the viewer waits for the picture, then fades it in)
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, Day(p.Taken, hour: true));
            var cell = new Grid();
            cell.Children.Add(b);
            cell.Children.Add(pin);
            Lift(cell, () => { over.Opacity = 1; pin.Opacity = 1; }, () => { over.Opacity = 0; pin.Opacity = isTop ? 1 : 0; });
            Grid.SetColumn(cell, i);
            row.Children.Add(cell);
            cells.Add(b);
        }
        // 16:9 at any width
        row.SizeChanged += (_, e) => { var cw = (e.NewSize.Width - 12 * 5) / 6; foreach (var c in cells) c.Height = Math.Max(90, cw * 9 / 16); };
        s.Children.Add(row);
        return s;
    }

    // the window's background, as chosen in Settings: Windows' Mica (tinted by the desktop wallpaper), its stronger kind,
    // Acrylic, or the launcher's own solid colour
    // (a new backdrop only when its kind changes: the one in place follows light and dark by itself, and replacing it at every
    // change of theme made the window's drawing fail after two or three changes)
    string? _backdropKind;
    void ApplyBackdrop()
    {
        if (_backdropKind != _settings.Backdrop)
        {
            _backdropKind = _settings.Backdrop;
            SystemBackdrop = _settings.Backdrop switch
            {
                "micaalt" => new MicaBackdrop { Kind = Microsoft.UI.Composition.SystemBackdrops.MicaKind.BaseAlt },
                "acrylic" => new DesktopAcrylicBackdrop(),
                "solid" => null,
                _ => new MicaBackdrop(),
            };
        }
        // (the theme's pane over what Windows lets through: a tint of the wallpaper, never lighter than the page)
        Root.Background = _settings.Backdrop == "solid" ? Res("ApexPaneBrush") : new SolidColorBrush(Tone("Pane", 0xE6));
    }
}
