using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Health: the crystal in a soft light of its colour, what asks for the player, then one tile per area, each opening a page of
// its own (mod conflicts with their levels and the ignored ones, the Stability timeline, what each check found), and the
// history of what the launcher changed.
public sealed partial class MainWindow
{
    const string HistoryPage = "history";
    string _healthPage = "";     // "" the overview, an area's name, or the history
    int _conflictTab;            // Mods: 0 to look at, 1 small, 2 ignored
    string _conflictOpen = "";   // the conflict showing its details
    bool _conflictTech;
    (string Text, string? Action, Action? Run)? _healthNote; // what was just done on this page

    static readonly HealthArea[] HealthAreas =
        [HealthArea.ApexRadiance, HealthArea.Mods, HealthArea.Loaders, HealthArea.Stability, HealthArea.Graphics, HealthArea.Saves, HealthArea.GameSettings, HealthArea.Disk];

    string AreaName(HealthArea a) => a switch
    {
        HealthArea.ApexRadiance => "Apex Radiance", HealthArea.Loaders => T("Loaders"), HealthArea.Graphics => T("Graphics"), HealthArea.GameSettings => T("Game settings"),
        HealthArea.Mods => T("Mods"), HealthArea.Stability => T("Stability"), HealthArea.Saves => T("Saves"), _ => T("Space and caches"),
    };
    string AreaDetail(HealthArea a) => a switch
    {
        HealthArea.ApexRadiance => T("files, settings, one copy"), HealthArea.Loaders => T("ASI loader, Sims 3 Modernization"), HealthArea.Graphics => _state?.Layer.ToString() ?? "",
        HealthArea.GameSettings => "Options.ini, Resource.cfg", HealthArea.Mods => T("Conflicts that change something in the game."), HealthArea.Stability => T("Your latest sessions."),
        HealthArea.Saves => T("size, copies, complete"), _ => T("free space, OneDrive, game caches"),
    };

    // the conflicts the last check found, without or with only the ignored ones
    List<ModConflict> ConflictsNow(bool ignored = false) => (_conflicts?.Found ?? []).Where(c => _settings.IgnoredConflicts.Contains(c.Id) == ignored).ToList();

    void HealthGo(string page)
    {
        (_healthPage, _conflictOpen, _conflictTech, _healthNote) = (page, "", false, null);
        if (page == nameof(HealthArea.Mods)) _conflictTab = ConflictsNow().Any(c => c.Level != ConflictLevel.Small) || !ConflictsNow().Any() ? 0 : 1;
        BuildHealth();
        Scroller.ChangeView(null, 0, null, true);
        if (ScreenHealth.Children.LastOrDefault() is { } body) Animate(body);
    }

    void BuildHealth()
    {
        var host = ScreenHealth;
        host.Children.Clear();
        // (the light runs to the window's edges; every other screen gets the page margins back in Show)
        Scroller.Padding = new Thickness(0);
        if (_game is null)
        {
            var none = new StackPanel { Spacing = 8, Padding = new Thickness(32, 24, 32, 0) };
            none.Children.Add(Heading(T("Health")));
            none.Children.Add(Muted(T("Choose the game first.")));
            host.Children.Add(none);
            return;
        }
        if (_checked == default) _ = CheckHealth();
        var home = _healthPage.Length == 0;
        var tint = StateTint(255);
        host.Children.Add(new SoftGlow(tint, _light ? (home ? 0.075 : 0.04) : home ? 0.1 : 0.045, home ? 0.22 : 0) { Height = home ? 640 : 340 });
        var body = new StackPanel { MaxWidth = 984, Spacing = 28, Padding = new Thickness(32, 24, 32, 8) };
        host.Children.Add(body);
        if (home) HealthHome(body);
        else if (_healthPage == HistoryPage) HealthHistory(body);
        else HealthAreaPage(body, Enum.Parse<HealthArea>(_healthPage));
    }

    // ---------- the look of its parts ----------

    // a surface the light behind it reaches through a little
    static Border Glass(UIElement child, double radius = 12)
    {
        return new Border { Child = child, CornerRadius = new CornerRadius(radius), BorderThickness = new Thickness(1), Background = new SolidColorBrush(Pick(0xE0171717, 0xEBFFFFFF)),
            BorderBrush = new SolidColorBrush(Pick(0x12FFFFFF, 0x14000000)) };
    }

    static TextBlock SectionTitle(string text) => new() { Text = text, FontSize = 15, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 2) };

    static FrameworkElement IconTile(string icon, Windows.UI.Color tint) => new Border
    {
        Width = 40, Height = 40, CornerRadius = new CornerRadius(10), VerticalAlignment = VerticalAlignment.Center,
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x24, tint.R, tint.G, tint.B)), Child = Lucide.Icon(icon, 20, new SolidColorBrush(tint)),
    };

    // an icon, a title with a line under it, and an action on the right
    static Grid Line(FrameworkElement? icon, string title, string detail, FrameworkElement? action, double pad = 16)
    {
        var g = new Grid { ColumnSpacing = 16, Padding = new Thickness(18, pad, 18, pad) };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        if (icon is not null) g.Children.Add(icon);
        var text = new StackPanel { Spacing = 3, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = title, FontSize = 15, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        if (detail.Length > 0) text.Children.Add(new TextBlock { Text = detail, FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        if (action is not null) { Grid.SetColumn(action, 2); action.VerticalAlignment = VerticalAlignment.Center; g.Children.Add(action); }
        return g;
    }

    static Button PillButton(UIElement content, string name, Action click)
    {
        var b = new Button { Content = content, CornerRadius = new CornerRadius(18), Height = 36, MinWidth = 36, Padding = new Thickness(16, 0, 18, 0), BorderThickness = new Thickness(1),
            Background = new SolidColorBrush(Pick(0x12FFFFFF, 0x0A000000)), BorderBrush = new SolidColorBrush(Pick(0x1FFFFFFF, 0x1F000000)) };
        b.Click += (_, _) => click();
        AutomationProperties.SetName(b, name);
        return b;
    }

    // ---------- the overview ----------

    void HealthHome(StackPanel body)
    {
        var top = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center, Spacing = 10, Margin = new Thickness(0, 12, 0, 4) };
        _healthCrystal = new Crystal(150) { HorizontalAlignment = HorizontalAlignment.Center };
        _healthCrystal.SetLevel(HealthLevel, animate: false);
        top.Children.Add(_healthCrystal);
        TextBlock Center(string t, double size, Brush? brush = null) => new()
        {
            Text = t, FontSize = size, HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap, MaxWidth = 720,
            Foreground = brush ?? Res("TextFillColorPrimaryBrush"),
        };
        var title = Center(HealthTitle(), 34);
        title.FontFamily = new FontFamily("Segoe UI Variable Display");
        title.FontWeight = FontWeights.SemiBold;
        top.Children.Add(title);
        top.Children.Add(Center(_healthState switch
        {
            HealthState.Blocked => T("The game still starts, but without Apex Radiance until this is fixed."),
            HealthState.NeedsYou => T("The game starts fine. Have a look when you have a minute."),
            _ when ModsUnread || _healthAll.Count(i => i.Finding.Severity != Severity.Info) > 0 => T("Nothing else needs your attention."),
            _ => T("Every check passed. The launcher checks again each time you press Play."),
        }, 14, Res("ApexSoftTextBrush")));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 8, 0, 0) };
        var check = PillButton(Labeled("rotate-ccw", T("Check now")), T("Check now"), () => { _modsLoaded = false; RefreshGame(); BuildHealth(); });
        AutomationProperties.SetAutomationId(check, "HealthCheckNow");
        actions.Children.Add(check);
        var copy = PillButton(Lucide.Icon("copy", 15, Res("ApexSoftTextBrush")), T("Copy report"), CopyReport);
        copy.Padding = new Thickness(0);
        copy.Width = 36;
        ToolTipService.SetToolTip(copy, T("Copy report"));
        actions.Children.Add(copy);
        top.Children.Add(actions);
        top.Children.Add(Center(_checked == default ? T("Checking…") : T("Last checked {0}", _checked.ToString("t", LangCulture())), 12, Res("ApexMutedBrush")));
        body.Children.Add(top);

        if (NoteStrip() is { } note) body.Children.Add(note);

        // what asks for the player: the mod conflicts as one line, then each other finding
        var mine = ConflictsNow().Where(c => c.Level != ConflictLevel.Small).ToList();
        var needs = _health.Where(i => i.Finding.Severity != Severity.Info && !i.Conflict).ToList();
        if (mine.Count + needs.Count > 0)
        {
            var list = new StackPanel { Spacing = 8 };
            list.Children.Add(SectionTitle(T("Needs your attention")));
            if (mine.Count > 0)
            {
                var see = Button(T("See"), () => HealthGo(nameof(HealthArea.Mods)), accent: true);
                see.MinWidth = 84;
                list.Children.Add(Glass(Line(IconTile(AreaIcon(HealthArea.Mods), ((SolidColorBrush)Res("ApexWarningBrush")).Color),
                    mine.Count == 1 ? T(mine[0].Title) : T("{0} conflicts between mods", mine.Count), mine.Count == 1 ? string.Join(", ", mine[0].Names) : T("They change the same things in the game."), see)));
            }
            foreach (var it in needs)
            {
                var (t, d) = Sentences(T(it.Finding.Text, it.Finding.Args));
                list.Children.Add(Glass(Line(IconTile(AreaIcon(it.Area), ((SolidColorBrush)Res(it.Finding.Severity == Severity.Error ? "ApexErrorBrush" : "ApexWarningBrush")).Color), t, d, FindingButtons(it))));
            }
            body.Children.Add(list);
        }

        var checks = new StackPanel { Spacing = 8 };
        checks.Children.Add(SectionTitle(T("Checks")));
        var grid = new CardGrid { MinItemWidth = 310, Spacing = 8 };
        foreach (var a in HealthAreas) grid.Children.Add(AreaTile(a));
        checks.Children.Add(grid);
        body.Children.Add(checks);

        var foot = new Grid { ColumnSpacing = 8, Padding = new Thickness(0, 20, 0, 0), BorderThickness = new Thickness(0, 1, 0, 0), BorderBrush = Res("ApexHairlineBrush") };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.Children.Add(Col(new TextBlock { Text = T("Something wrong in the game?"), FontSize = 14, FontWeight = FontWeights.SemiBold },
            Muted(T("Repair copies the launcher's files again. Your saves and mods don't change."))));
        var hist = Button(T("History"), () => HealthGo(HistoryPage));
        var repair = Button(T("Repair"), async () => await Repair());
        repair.IsEnabled = !_running && !_busy && _state?.ApexVersion is not null;
        AutomationProperties.SetAutomationId(repair, "HealthRepair");
        foreach (var (b, i) in new[] { (hist, 1), (repair, 2) }) { Grid.SetColumn(b, i); b.VerticalAlignment = VerticalAlignment.Center; b.MinWidth = 92; foot.Children.Add(b); }
        body.Children.Add(foot);
    }

    // one area: how it is, and a click to its page
    Button AreaTile(HealthArea area)
    {
        var items = _health.Where(i => i.Area == area).ToList();
        var bad = items.Count(i => i.Finding.Severity != Severity.Info);
        var gone = IgnoredIn(area).Count + (area == HealthArea.Mods ? ConflictsNow(ignored: true).Count : 0);
        var brush = bad == 0 ? gone > 0 ? "ApexMutedBrush" : "ApexSuccessBrush" : items.Any(i => i.Finding.Severity == Severity.Error) ? "ApexErrorBrush" : "ApexWarningBrush";
        var unread = bad == 0 && area == HealthArea.Mods && ModsUnread;
        if (unread) brush = "ApexMutedBrush";
        var state = unread ? T("Not checked") : bad == 0 ? gone == 0 ? T("All good") : gone == 1 ? T("1 ignored") : T("{0} ignored", gone) : bad == 1 ? T("1 to look at") : T("{0} to look at", bad);
        var g = new Grid { ColumnSpacing = 14, Padding = new Thickness(16, 0, 14, 0), Height = 58 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var icon = Lucide.Icon(AreaIcon(area), 18, Res("ApexSoftTextBrush"));
        var name = new TextBlock { Text = AreaName(area), FontSize = 14, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        var how = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 7 };
        how.Children.Add(bad == 0 && !unread ? Lucide.Icon("check", 14, Res(brush)) : new Border { Width = 8, Height = 8, CornerRadius = new CornerRadius(4), Background = Res(brush), VerticalAlignment = VerticalAlignment.Center });
        how.Children.Add(new TextBlock { Text = state, FontSize = 13, Foreground = Res(brush), VerticalAlignment = VerticalAlignment.Center });
        var go = Lucide.Icon("chevron-right", 14, Res("ApexMutedBrush"));
        foreach (var (e, i) in new[] { (icon, 0), (name, 1), ((FrameworkElement)how, 2), (go, 3) }) { Grid.SetColumn(e, i); e.VerticalAlignment = VerticalAlignment.Center; g.Children.Add(e); }
        var b = new Button { Content = g, Padding = new Thickness(0), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch, Background = new SolidColorBrush(Pick(0xE0171717, 0xEBFFFFFF)), BorderBrush = new SolidColorBrush(Pick(0x12FFFFFF, 0x14000000)) };
        b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Pick(0xF0202020, 0xFFF6F6F6));
        b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Pick(0xF01B1B1B, 0xFFF0F0F0));
        b.Resources["ButtonBorderBrushPointerOver"] = new SolidColorBrush(Pick(0x24FFFFFF, 0x24000000));
        b.Click += (_, _) => HealthGo(area.ToString());
        AutomationProperties.SetName(b, AreaName(area) + ", " + state);
        AutomationProperties.SetAutomationId(b, "Health" + area);
        return b;
    }

    // ---------- the pages ----------

    void HealthHead(StackPanel body, string title, string sub)
    {
        var back = new Button { Content = Labeled("chevron-left", T("Health")), Padding = new Thickness(8, 6, 12, 6), Margin = new Thickness(-11, 0, 0, 6), BorderThickness = new Thickness(0), HorizontalAlignment = HorizontalAlignment.Left,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        back.Click += (_, _) => HealthGo("");
        AutomationProperties.SetAutomationId(back, "HealthBack");
        AutomationProperties.SetName(back, T("Back to {0}", T("Health")));
        var head = new StackPanel { Spacing = 4 };
        head.Children.Add(back);
        head.Children.Add(PageHead(title, sub.Length > 0 ? char.ToUpper(sub[0], LangCulture()) + sub[1..] : null));
        body.Children.Add(head);
        if (NoteStrip() is not { } strip) return;
        strip.Margin = new Thickness(0, -14, 0, -14);
        body.Children.Add(strip);
    }

    // what was just done, with its Undo
    Border? NoteStrip()
    {
        if (_healthNote is not { } n) return null;
        var said = new Grid { ColumnSpacing = 12, Padding = new Thickness(18, 6, 8, 6), MinHeight = 46 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) said.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var tick = Lucide.Icon("check", 16, Res("ApexSuccessBrush"), 2.5);
        tick.VerticalAlignment = VerticalAlignment.Center;
        said.Children.Add(tick);
        var what = new TextBlock { Text = n.Text, FontSize = 14, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
        AutomationProperties.SetLiveSetting(what, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        Grid.SetColumn(what, 1);
        said.Children.Add(what);
        if (n.Action is { } act && n.Run is { } run)
        {
            var undo = Link(act, run);
            undo.Padding = new Thickness(12, 6, 12, 6);
            Grid.SetColumn(undo, 2);
            said.Children.Add(undo);
        }
        return Glass(said);
    }

    // a page with nothing in it
    static Border Empty(string title, string text)
    {
        var s = new StackPanel { Spacing = 4, Padding = new Thickness(24, 40, 24, 42) };
        s.Children.Add(new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap });
        s.Children.Add(new TextBlock { Text = text, FontSize = 13, Foreground = Res("ApexMutedBrush"), HorizontalAlignment = HorizontalAlignment.Center, TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap });
        return Glass(s);
    }

    // rows of one panel, divided by hairlines
    static Border Rows(IEnumerable<UIElement> rows)
    {
        var s = new StackPanel();
        foreach (var r in rows)
            s.Children.Add(new Border { Child = r, BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, s.Children.Count == 0 ? 0 : 1, 0, 0) });
        return Glass(s);
    }

    Grid FindingLine(Health.Item it)
    {
        var (t, d) = Sentences(T(it.Finding.Text, it.Finding.Args));
        var brush = it.Finding.Severity switch { Severity.Error => "ApexErrorBrush", Severity.Warning => "ApexWarningBrush", _ => "ApexMutedBrush" };
        return Line(IconTile(it.Finding.Severity == Severity.Info ? "info" : "triangle-alert", ((SolidColorBrush)Res(brush)).Color), t, d, FindingButtons(it), 14);
    }

    // what can be done about a finding, and Ignore; an ignored one only comes back
    StackPanel FindingButtons(Health.Item it)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var off = IsIgnored(it);
        if (!off && FindingAction(it) is { } act) { act.MinWidth = 88; s.Children.Add(act); }
        var b = Button(off ? T("Show again") : T("Ignore"), () => IgnoreFinding(it, !off));
        b.MinWidth = 88;
        AutomationProperties.SetName(b, (off ? T("Show again") : T("Ignore")) + ", " + Sentences(T(it.Finding.Text, it.Finding.Args)).Item1);
        s.Children.Add(b);
        return s;
    }

    List<Health.Item> IgnoredIn(HealthArea area) => _healthAll.Where(i => i.Area == area && IsIgnored(i)).ToList();

    void IgnoreFinding(Health.Item it, bool on, bool note = true)
    {
        _settings.IgnoredFindings.Remove(it.Id);
        if (on) _settings.IgnoredFindings.Add(it.Id);
        _settings.Save();
        _modsLoaded = false; // (the Mods screen marks the mods a finding names)
        SetHealth(_healthAll);
        IgnoreDone(on, note, () => IgnoreFinding(it, false, note: false));
    }

    void IgnoreDone(bool on, bool note, Action undo)
    {
        HealthBadge.Visibility = _healthState != HealthState.Ready ? Visibility.Visible : Visibility.Collapsed;
        _healthNote = !note ? null : on ? (T("Ignored. It won't show again."), T("Undo"), undo) : (T("Showing again."), null, null);
        BuildHealth();
    }

    void HealthAreaPage(StackPanel body, HealthArea area)
    {
        HealthHead(body, AreaName(area), AreaDetail(area));
        if (area == HealthArea.Mods) { HealthMods(body); return; }
        var items = _health.Where(i => i.Area == area).ToList();
        var timeline = area == HealthArea.Stability ? Health.Timeline : [];
        if (timeline.Count > 0)
        {
            body.Children.Add(Rows(timeline.Select(TimelineRow)));
            items = items.Where(i => !i.Finding.Text.Contains("crashed", StringComparison.Ordinal)).ToList();
        }
        if (items.Count > 0) body.Children.Add(Rows(items.OrderByDescending(i => i.Finding.Severity != Severity.Info).Select(FindingLine)));
        else if (timeline.Count == 0 && IgnoredIn(area).Count == 0) body.Children.Add(Empty(T("All good"), area == HealthArea.Stability ? T("Play once through the launcher and the session shows here.") : T("Nothing to report.")));
        if (IgnoredIn(area) is { Count: > 0 } gone)
        {
            var list = new StackPanel { Spacing = 8 };
            list.Children.Add(SectionTitle(T("Ignored")));
            list.Children.Add(Rows(gone.Select(FindingLine)));
            body.Children.Add(list);
        }
    }

    // ---------- Mods: the conflicts ----------

    // the last check did not get through the mods: no conflict shown is not no conflict
    bool ModsUnread => !_healthChecking && _conflicts?.Last is null;

    void HealthMods(StackPanel body)
    {
        var live = ConflictsNow();
        var look = live.Where(c => c.Level != ConflictLevel.Small).ToList();
        var small = live.Where(c => c.Level == ConflictLevel.Small).ToList();
        var gone = ConflictsNow(ignored: true);
        var goneToo = IgnoredIn(HealthArea.Mods);
        var other = _health.Where(i => i.Area == HealthArea.Mods && !i.Conflict).ToList();

        var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        (string Name, int Count)[] tabs = [(T("To look at"), look.Count + other.Count(i => i.Finding.Severity != Severity.Info)), (T("Minor"), small.Count), (T("Ignored"), gone.Count + goneToo.Count)];
        for (int i = 0; i < tabs.Length; i++)
        {
            var at = i;
            var on = i == _conflictTab;
            var words = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            words.Children.Add(new TextBlock { Text = tabs[i].Name, FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Res(on ? "TextFillColorPrimaryBrush" : "ApexMutedBrush") });
            words.Children.Add(new TextBlock { Text = tabs[i].Count.ToString(LangCulture()), FontSize = 13, Foreground = Res("ApexMutedBrush") });
            var b = new Button { Content = words, Height = 30, Padding = new Thickness(14, 0, 14, 0), CornerRadius = new CornerRadius(4), BorderThickness = new Thickness(0),
                Background = new SolidColorBrush(on ? Pick(0x1FFFFFFF, 0x14000000) : Microsoft.UI.Colors.Transparent) };
            b.Click += (_, _) => { (_conflictTab, _conflictOpen, _healthNote) = (at, "", null); BuildHealth(); };
            AutomationProperties.SetName(b, $"{tabs[i].Name} {tabs[i].Count}");
            AutomationProperties.SetItemStatus(b, on ? T("Selected") : "");
            bar.Children.Add(b);
        }
        var page = new StackPanel { Spacing = 12 };
        var strip = Glass(bar, 6);
        (strip.Height, strip.Padding) = (36, new Thickness(2));
        strip.HorizontalAlignment = HorizontalAlignment.Left;
        strip.Margin = new Thickness(0, 0, 0, 4);
        page.Children.Add(strip);
        body.Children.Add(page);
        if (_conflicts?.Last is null && _conflicts?.Found.Count is null or 0 && _healthChecking)
        {
            var wait = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, Padding = new Thickness(20, 22, 20, 22), HorizontalAlignment = HorizontalAlignment.Center };
            wait.Children.Add(new ProgressRing { IsActive = true, Width = 18, Height = 18 });
            wait.Children.Add(Muted(T("Checking your mods…")));
            page.Children.Add(Glass(wait));
            return;
        }
        if (ModsUnread) page.Children.Add(Empty(T("Your mods were not checked"), T("The conflict check didn't finish. Check again in a moment.")));
        var shown = _conflictTab switch { 1 => small, 2 => gone, _ => look };
        foreach (var c in shown) page.Children.Add(ConflictCard(c));
        if (_conflictTab == 0 && other.Count > 0) page.Children.Add(Rows(other.OrderByDescending(i => i.Finding.Severity != Severity.Info).Select(FindingLine)));
        else if (_conflictTab == 2 && goneToo.Count > 0) page.Children.Add(Rows(goneToo.Select(FindingLine)));
        else if (shown.Count == 0 && (!ModsUnread || _conflictTab == 2))
            page.Children.Add(_conflictTab switch
            {
                1 => Empty(T("No minor conflicts"), T("None of your mods share a command, a picture or text.")),
                2 => Empty(T("Nothing ignored"), T("Anything you ignore appears here.")),
                _ when small.Count + gone.Count + goneToo.Count > 0 => Empty(T("Nothing needs your attention"), T("Everything left is minor or ignored.")),
                _ => Empty(T("No conflicts"), T("Your mods don't conflict with each other.")),
            });
    }

    Border ConflictCard(ModConflict c)
    {
        var ignored = _settings.IgnoredConflicts.Contains(c.Id);
        var open = _conflictOpen == c.Key;
        var all = new StackPanel();
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var more = Button(open ? T("Close") : T("Details"), () => { (_conflictOpen, _conflictTech) = (open ? "" : c.Key, false); BuildHealth(); });
        var ignore = Button(ignored ? T("Show again") : T("Ignore"), () => IgnoreConflict(c, !ignored));
        AutomationProperties.SetName(more, (open ? T("Close") : T("Details")) + ", " + string.Join(", ", c.Names));
        AutomationProperties.SetName(ignore, (ignored ? T("Show again") : T("Ignore")) + ", " + string.Join(", ", c.Names));
        more.MinWidth = ignore.MinWidth = 88;
        buttons.Children.Add(more);
        buttons.Children.Add(ignore);
        var color = ((SolidColorBrush)Res(c.Level switch { ConflictLevel.Fix => "ApexErrorBrush", ConflictLevel.Look => "ApexWarningBrush", _ => "ApexMutedBrush" })).Color;
        all.Children.Add(Line(IconTile(AreaIcon(HealthArea.Mods), color), T(c.Title), string.Join(", ", c.Names), buttons));
        if (open)
        {
            var inner = new StackPanel { Spacing = 16, Padding = new Thickness(74, 16, 18, 18), BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, 1, 0, 0) };
            inner.Children.Add(new TextBlock { Text = T(c.Say, c.SayArgs), FontSize = 14, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, MaxWidth = 620, HorizontalAlignment = HorizontalAlignment.Left });
            if (c.Values.Length > 0) inner.Children.Add(ConflictValues(c));
            inner.Children.Add(ConflictMods(c));
            var foot = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            if (ModsFor(c.Files) is { Count: > 0 } hits) foot.Children.Add(Button(T("Show in Mods"), () => ShowInMods(hits)));
            var tech = Link(_conflictTech ? T("Hide technical details") : T("Technical details"), () => { _conflictTech = !_conflictTech; BuildHealth(); });
            tech.Padding = new Thickness(10, 6, 10, 6);
            tech.Foreground = Res("ApexMutedBrush");
            foot.Children.Add(tech);
            inner.Children.Add(foot);
            if (_conflictTech)
                inner.Children.Add(new Border { CornerRadius = new CornerRadius(8), Padding = new Thickness(14, 12, 14, 12), Background = new SolidColorBrush(Pick(0xFF101010, 0xFFF3F3F3)),
                    Child = new TextBlock { Text = c.Raw, FontFamily = new FontFamily("Consolas"), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true } });
            all.Children.Add(inner);
        }
        return Glass(all);
    }

    // what two mods set to something else: each name with its value in use and the other mod's
    static FrameworkElement ConflictValues(ModConflict c)
    {
        var g = new Grid { ColumnSpacing = 24, RowSpacing = 6, HorizontalAlignment = HorizontalAlignment.Left };
        foreach (var w in new[] { GridLength.Auto, GridLength.Auto, GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        void Cell(string text, int col, int row, string brush, bool strong = false)
        {
            var t = new TextBlock { Text = text, FontSize = 12, Foreground = Res(brush), FontWeight = strong ? FontWeights.SemiBold : FontWeights.Normal, MaxWidth = 280, TextTrimming = TextTrimming.CharacterEllipsis };
            ToolTipService.SetToolTip(t, text);
            Grid.SetColumn(t, col);
            Grid.SetRow(t, row);
            g.Children.Add(t);
        }
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Cell(T("What differs"), 0, 0, "ApexMutedBrush");
        Cell(T("In use"), 1, 0, "ApexMutedBrush");
        Cell(T("The other"), 2, 0, "ApexMutedBrush");
        foreach (var (name, used, other) in c.Values)
        {
            var row = g.RowDefinitions.Count;
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Cell(name, 0, row, "ApexSoftTextBrush");
            Cell(used, 1, row, "TextFillColorPrimaryBrush", strong: true);
            Cell(other, 2, row, "ApexMutedBrush");
        }
        return g;
    }

    // the mods of a conflict, what each one has in it, and a way to switch one off
    FrameworkElement ConflictMods(ModConflict c)
    {
        var rows = new StackPanel();
        var files = c.Names.Zip(c.Files, (n, f) => (Party: n, File: _mods.FirstOrDefault(m => m.Enabled && m.Path.Equals(f, StringComparison.OrdinalIgnoreCase)))).ToList();
        var dates = files.Select(f => f.File is { } m && File.Exists(m.Path) ? File.GetLastWriteTime(m.Path) : (DateTime?)null).ToList();
        var newest = dates.Max();
        for (int i = 0; i < files.Count; i++)
        {
            var (party, file) = files[i];
            var info = c.InUse is { } w ? (w == c.Labels[i] ? T("In use") : c.Alike.Contains(c.Labels[i]) ? T("Same as the one in use") : c.Dead.Contains(c.Labels[i]) ? T("Does nothing") : T("Replaced by the other"))
                : c.Title == "Two versions of the same mod" && dates[i] is { } d ? (d == newest ? T("Newest, from {0}", d.ToString("d MMM yyyy", LangCulture())) : T("From {0}", d.ToString("d MMM yyyy", LangCulture()))) : "";
            var g = new Grid { ColumnSpacing = 12, Padding = new Thickness(14, 8, 8, 8), MinHeight = 48, BorderBrush = Res("ApexHairlineBrush"), BorderThickness = new Thickness(0, i == 0 ? 0 : 1, 0, 0) };
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var words = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
            words.Children.Add(new TextBlock { Text = party, FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
            if (info.Length > 0) words.Children.Add(new TextBlock { Text = info, FontSize = 12, Foreground = Res("ApexMutedBrush") });
            g.Children.Add(words);
            var others = files.Where((_, n) => n != i).Select(f => f.File).OfType<ModFile>().ToList();
            if (c.CanOrder && file is not null && others.Count == files.Count - 1 && ModPriority.Moves(Docs, file, others) is { Count: > 0 } moves)
            {
                var up = Button(T("Give priority"), async () => await GivePriority(c, party, moves));
                (up.Background, up.BorderThickness, up.VerticalAlignment, up.IsEnabled) = (new SolidColorBrush(Microsoft.UI.Colors.Transparent), new Thickness(0), VerticalAlignment.Center, !_running && !_busy);
                AutomationProperties.SetName(up, T("Give priority") + ", " + party);
                ToolTipService.SetToolTip(up, string.Join("\n", moves.Select(x => T("Moves {0} to {1}", Path.GetFileNameWithoutExtension(x.Mod.Name), "Mods\\" + x.Folder))));
                Grid.SetColumn(up, 1);
                g.Children.Add(up);
            }
            if (file is { Locked: false, FileType: "package" } m)
            {
                var off = Button(T("Turn off"), async () => await TurnOffMod(m));
                (off.Background, off.BorderThickness, off.VerticalAlignment, off.IsEnabled) = (new SolidColorBrush(Microsoft.UI.Colors.Transparent), new Thickness(0), VerticalAlignment.Center, !_running && !_busy);
                AutomationProperties.SetName(off, T("Turn off {0}", party));
                Grid.SetColumn(off, 2);
                g.Children.Add(off);
            }
            rows.Children.Add(g);
        }
        return new Border { Child = rows, CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), BorderBrush = Res("ApexHairlineBrush"), Background = new SolidColorBrush(Pick(0x66101010, 0x66F6F6F6)) };
    }

    // ignored: not shown again, also after the mods update, until Warn again
    void IgnoreConflict(ModConflict c, bool on, bool note = true)
    {
        if (on) { if (!_settings.IgnoredConflicts.Contains(c.Id)) _settings.IgnoredConflicts.Add(c.Id); }
        else _settings.IgnoredConflicts.Remove(c.Id);
        _settings.Save();
        // (what Health counts follows at once; the next check finds the same)
        var same = (_conflicts?.Found ?? [c]).Where(x => x.Id == c.Id && x.Finding is not null).ToList();
        if (same.Count > 0)
        {
            var rest = _healthAll.Where(i => !same.Any(x => ReferenceEquals(x.Finding, i.Finding))).ToList();
            SetHealth(on ? rest : [.. rest, .. same.Select(x => new Health.Item(x.Finding!, HealthArea.Mods, x.Files) { Conflict = true })]);
        }
        _conflictOpen = "";
        IgnoreDone(on, note, () => IgnoreConflict(c, false, note: false));
    }

    // one mod of a conflict first: its file (or the others') moves to the folder that comes first (or after). The next
    // check no longer finds a conflict there.
    async Task GivePriority(ModConflict c, string party, List<(ModFile Mod, string Folder)> moves)
    {
        var docs = Docs;
        void Again() { _conflictOpen = ""; _modsLoaded = false; RefreshGame(); BuildHealth(); }
        try
        {
            var done = await ModWork(() => ModPriority.Apply(docs, moves));
            _healthNote = (T("{0} has priority now.", party), T("Undo"), async () =>
            {
                try { await ModWork(() => { ModPriority.Back(docs, done); return 0; }); _healthNote = null; }
                catch (Exception e) { App.Log(e); await Message(T("The mod was not moved"), ErrorText(e)); }
                Again();
            });
            Again();
        }
        catch (Exception e) { App.Log(e); await Message(T("The mod was not moved"), ErrorText(e)); }
    }

    async Task TurnOffMod(ModFile m)
    {
        var docs = Docs;
        try
        {
            var path = await ModWork(() => Mods.SetEnabled(m, false, docs));
            var off = m with { Path = path, Enabled = false };
            _healthNote = (T("{0} is off. Nothing was deleted.", Path.GetFileNameWithoutExtension(m.Name)), T("Undo"), async () =>
            {
                try { await ModWork(() => Mods.SetEnabled(off, true, docs)); _healthNote = null; }
                catch (Exception e) { App.Log(e); await Message(T("The mod was not moved"), ErrorText(e)); }
                _modsLoaded = false;
                RefreshGame();
            });
            _conflictOpen = "";
            _modsLoaded = false;
            RefreshGame(); // (starts the check again, which finds the conflict gone)
            BuildHealth();
        }
        catch (Exception e) { App.Log(e); await Message(T("The mod was not moved"), ErrorText(e)); }
    }

    // ---------- the history ----------

    void HealthHistory(StackPanel body)
    {
        HealthHead(body, T("History"), T("What the launcher changed. Going back undoes that point and every later one."));
        var waiting = _health.Where(i => i.Finding.CanFix).ToList();
        if (waiting.Count > 0) body.Children.Add(Rows(waiting.Select(FindingLine)));
        var points = _game is { } g ? RestorePoints.List(g.Bin) : [];
        if (points.Count == 0) { body.Children.Add(Empty(T("Nothing yet"), T("Changes made by the launcher appear here."))); return; }
        body.Children.Add(Rows(points.Select(p =>
        {
            var fix = p.Label.StartsWith("Fix", StringComparison.Ordinal);
            var b = Button(fix ? T("Undo") : T("Go back"), async () => { await RestoreAsync(p); _ = CheckHealth(); BuildHealth(); });
            b.IsEnabled = !_running && !_busy;
            b.MinWidth = 88;
            return (UIElement)Line(IconTile(fix ? "wrench" : "rotate-ccw", ((SolidColorBrush)Res("ApexMutedBrush")).Color), T(p.Label, p.Args), p.Created.ToLocalTime().ToString("d MMM, HH:mm", LangCulture()), b, 12);
        })));
    }
}
