using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using static ApexSetup.Strings;

namespace ApexSetup;

// Saves: the household played last over one of its own pictures, the memories of every save, how many saves have a copy, the
// other households on a shelf, and those that left the game: the lost ones it still keeps a copy of, and the ones the player
// put out of its sight. A save's own page is MainWindow.SavePage.cs.
public sealed partial class MainWindow
{
    const string LostPage = "lost", OutPage = "out";
    string _savesPage = "";     // "" the households, LostPage or OutPage
    Action? _saveView;          // a save's own page while it is open: draws it in SavesHost again
    string _savesSig = "";      // what SavesHost shows (the same again is left as it is: the search keeps its place)
    string _savesQuery = "";
    bool _savesBySize, _savesBusy;
    int _savesRun;
    SavesNow? _savesNow;
    List<SaveLook>? _away;                                // the households LostPage or OutPage opened with
    readonly Dictionary<string, Save> _back = new();      // and those brought back since, by the folder they came from
    readonly List<Control> _savesWork = [];               // the buttons that wait while a save is copied or moved
    readonly Dictionary<string, Task> _saveReads = new(); // what was read of the saves, kept while the launcher is open
    readonly SemaphoreSlim _saveReading = new(2);

    // a save, what it says about itself, and when the launcher last copied it
    sealed record SaveLook(Save Save, SaveInfo Info, DateTime? Copy);
    sealed record SavesNow(string Docs, List<SaveLook> Here, List<SaveLook> Lost, List<SaveLook> Out);

    static SavesNow ReadSaves(string docs)
    {
        List<SaveLook> Look(List<Save> saves) => saves.Select(s => new SaveLook(s, SaveInfo.Of(s), SaveHealth.LastBackup(s, docs))).ToList();
        return new(docs, Look(Saves.List(docs)), Look(SaveCopies.Lost(docs)), Look(SaveCopies.Stored(docs)));
    }

    // what was known shows at once; the folders are read off the window's thread and only a change is drawn
    void FillSaves()
    {
        if (_game is null)
        {
            (_savesSig, _saveView) = ("", null);
            SavesHost.Children.Clear();
            SavesHost.Children.Add(Heading(T("Saves")));
            SavesHost.Children.Add(Muted(T("Choose the game first.")));
            return;
        }
        if (_saveView is { } open) { _savesSig = ""; open(); return; }
        var docs = Docs;
        if (_savesNow is { } last && last.Docs == docs) DrawSaves(last);
        else if (_savesNow is not null) { _savesNow = null; _savesSig = ""; SavesHost.Children.Clear(); }
        _ = Read(++_savesRun);

        async Task Read(int run)
        {
            try
            {
                var now = await Task.Run(() => ReadSaves(docs));
                if (run != _savesRun) return;
                _savesNow = now;
                if (_step == SavesScreen && _saveView is null) DrawSaves(now);
            }
            catch (Exception e) { App.Log(e); if (run == _savesRun) SetInfo(SavesInfo, InfoBarSeverity.Error, T("The launcher can't read your saves"), ErrorText(e)); }
        }
    }

    // another page of Saves, from its top
    void SavesGo(string page)
    {
        (_savesPage, _saveView, _savesSig, _away) = (page, null, "", null);
        _back.Clear();
        SavesInfo.IsOpen = false;
        FillSaves();
        Scroller.ChangeView(null, 0, null, true);
        Animate(SavesHost);
    }

    void DrawSaves(SavesNow now)
    {
        var look = $"{_savesPage}|{Lang}|{_light}|{_running}|{_busy}|{Maintenance}|{_healthState}|{_settings.InstallUpdatesOnPlay}|{WelcomeUpdates().Count}";
        var sig = _savesPage.Length > 0 ? look : string.Join('\n', new[] { now.Here, now.Lost, now.Out }
            .SelectMany(l => l.Select(s => $"{s.Save.Dir}|{s.Info.LastPlayed.Ticks}|{s.Info.Bytes}|{s.Copy?.Ticks}").Append("")).Prepend(look));
        if (sig == _savesSig) return;
        _savesSig = sig;
        SavesHost.Children.Clear();
        _savesWork.Clear();
        if (_savesPage.Length > 0) { DrawAway(_away ??= _savesPage == LostPage ? now.Lost : now.Out, _savesPage == LostPage); return; }
        if (now.Here.Count == 0)
        {
            var none = new StackPanel { Spacing = 8 };
            none.Children.Add(Heading(T("Saves")));
            none.Children.Add(Muted(T("No saves yet. They show here after you save a game.")));
            SavesHost.Children.Add(none);
        }
        else SavesHost.Children.Add(SavesTop(now));
        SavesHost.Children.Add(SavesShelf(now));
    }

    // ---------- words ----------

    static string FamilyName(SaveLook l) => l.Info.Household.Length > 0 ? T("{0} household", l.Info.Household) : l.Save.Name;

    // the worlds it has, the one being played first
    static string PlaceName(SaveLook l)
    {
        var w = l.Info.Worlds.DefaultIfEmpty(l.Info.World).Select(Worlds.Pretty).Where(n => n.Length > 0).Distinct().ToList();
        return w.Count == 2 ? T("{0} and {1}", w[0], w[1]) : string.Join(", ", w);
    }

    // two saves of one household in one world differ only by the save's name
    static string SavePlace(SaveLook l) => PlaceName(l) is var p && p.Length == 0 ? l.Save.Name
        : l.Info.Worlds.Append(l.Info.World).Select(Worlds.Pretty).Any(w => w.StartsWith(l.Save.Name, StringComparison.OrdinalIgnoreCase)) ? p : l.Save.Name + " · " + p;

    string PlayedWhen(DateTime t)
    {
        var c = LangCulture();
        var days = (DateTime.Today - t.Date).Days;
        return days <= 0 ? T("Played today, {0}", t.ToString("t", c)) : days == 1 ? T("Played yesterday, {0}", t.ToString("t", c))
            : T("Played on {0}", t.ToString(t.Year == DateTime.Today.Year ? "d MMM" : "d MMM yyyy", c));
    }

    // ---------- pictures ----------

    // read once for a save as it is; a save played since is read again
    Task<R> OfSave<R>(SaveLook l, string what, Func<Task<R>> read)
    {
        var key = $"{what}|{l.Save.Dir}|{l.Info.LastPlayed.Ticks}|{l.Info.Bytes}";
        return _saveReads.TryGetValue(key, out var t) ? (Task<R>)t : (Task<R>)(_saveReads[key] = read());
    }

    // off the window's thread, two saves at a time; null when it can't be read
    async Task<R?> OffThread<R>(Func<R?> read)
    {
        await _saveReading.WaitAsync();
        try { return await Task.Run(read); }
        catch (Exception e) { App.Log(e); return default; }
        finally { _saveReading.Release(); }
    }

    Task<ImageSource?> FamilyPicture(SaveLook l, bool grey = false) => OfSave(l, "family" + grey, async () => await SaveCard.SimsOf(await OffThread(() => Saves.Photo(l.Save)), grey));

    // of the save's fullest lots, a light one: many are taken at night
    Task<byte[]?> Lot(SaveLook l) => OfSave(l, "lot", async () =>
    {
        (byte[]? Png, double Light) best = default;
        foreach (var png in await OffThread(() => SaveCover.Lots(SaveContents.Read(l.Save.Dir), 6).Select(SaveContents.Data).OfType<byte[]>().ToList()) ?? [])
            if (await SaveCard.LightOf(png) is var light && light > best.Light + 8) best = (png, light);
        return best.Png;
    });

    Task<ImageSource?> LotPicture(SaveLook l, bool grey = false) => OfSave(l, "lot picture" + grey, async () => await SaveCard.SimsOf(await Lot(l), grey));

    // behind the household: a memory of a place, else a lot, else its world's own picture
    Task<ImageSource?> CoverPicture(SaveLook l, Game g, string docs) => OfSave(l, "cover", async () => await SaveCard.ImageOf(
        await OffThread(() => SaveCover.Best(SaveContents.Read(l.Save.Dir)) is { } view ? SaveContents.Png(view) : null)
        ?? await Lot(l)
        ?? await OffThread(() => Worlds.List(g, docs).FirstOrDefault(w => System.IO.Path.GetFileNameWithoutExtension(w.Path).Equals(l.Info.World, StringComparison.OrdinalIgnoreCase)) is { } world ? Worlds.Picture(world) : null)));

    Task<int> MemoryCount(SaveLook l) => OfSave(l, "memories", () => OffThread(() => SaveContents.Read(l.Save.Dir).Of(SaveKind.Memory).Count));

    Task<List<ImageSource>> MemoryFan(SaveLook l) => OfSave(l, "fan", async () =>
    {
        var list = new List<ImageSource>();
        foreach (var png in await OffThread(() => { var c = SaveContents.Read(l.Save.Dir); return SaveCover.Fan(c, 5).Select(i => SaveContents.Png(i, 200)).OfType<byte[]>().ToList(); }) ?? [])
            if (await SaveCard.ImageOf(png) is { } img) list.Add(img);
        return list;
    });

    // a picture when it is there: at once when it was read before, else faded in over the room kept for it
    static void ShowWhenRead(UIElement on, Task<ImageSource?> read, Action<ImageSource> set, double opacity = 1, Action? none = null)
    {
        if (read.IsCompletedSuccessfully) { Put(read.Result); return; }
        on.Opacity = 0;
        if (AnimationsOn) on.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(320) };
        _ = Later();
        async Task Later() { try { Put(await read); } catch (Exception e) { App.Log(e); none?.Invoke(); } }
        void Put(ImageSource? img) { if (img is null) { none?.Invoke(); return; } set(img); on.Opacity = opacity; }
    }

    static Image SaveImage(Task<ImageSource?> read, double opacity = 1, Action? none = null)
    {
        var i = new Image { Stretch = Stretch.Uniform, IsHitTestVisible = false };
        ShowWhenRead(i, read, img => i.Source = img, opacity, none);
        return i;
    }

    // a household with no picture in its save
    static FrameworkElement NoPicture(double size) => Lucide.Icon("users", size, Res("ApexIconMutedBrush"), 1.4);

    // ---------- the look of its parts ----------

    static SolidColorBrush Argb(uint argb) => new(Windows.UI.Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb));

    // a whole card that is a button: its border and fill a step up under the pointer
    static Button SaveButton(UIElement content, string name, double radius, Action click)
    {
        var b = new Button
        {
            Content = content, Padding = new Thickness(0), CornerRadius = new CornerRadius(radius), BorderThickness = new Thickness(1), Background = Res("ApexCardBrush"), BorderBrush = Res("ApexCardBorderBrush"),
            HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        b.Resources["ButtonBackgroundPointerOver"] = b.Resources["ButtonBackgroundPressed"] = Res("ApexHoverBrush");
        b.Resources["ButtonBorderBrushPointerOver"] = new SolidColorBrush(Pick(0xFF454545, 0xFFB4B4B4));
        b.Resources["ButtonBorderBrushPressed"] = Res("ApexAccentBrush");
        AutomationProperties.SetName(b, name);
        b.Click += (_, _) => click();
        return b;
    }

    static TextBlock SaveText(string text, double size, string brush = "TextFillColorPrimaryBrush", bool strong = false, int lines = 1) => new()
    {
        Text = text, FontSize = size, Foreground = Res(brush), FontWeight = strong ? FontWeights.SemiBold : FontWeights.Normal, MaxLines = lines,
        TextWrapping = lines > 1 ? TextWrapping.Wrap : TextWrapping.NoWrap, TextTrimming = TextTrimming.CharacterEllipsis,
    };

    // ---------- the top: the household played last, the memories, the copies ----------

    FrameworkElement SavesTop(SavesNow now)
    {
        var top = new Grid { RowSpacing = 18 };
        var side = new Grid { ColumnSpacing = 18, RowSpacing = 18 };
        foreach (var g in new[] { top, side })
        {
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0) });
            g.RowDefinitions.Add(new RowDefinition());
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        }
        var hero = SavesHero(now);
        var (memories, copies) = (MemoriesCard(now), CopiesCard(now));
        top.Children.Add(hero);
        top.Children.Add(side);
        side.Children.Add(memories);
        side.Children.Add(copies);
        // beside the household, one over the other; under it, one by the other, in a narrow window
        void Fit(double w)
        {
            var wide = w >= 880;
            top.ColumnSpacing = wide ? 18 : 0;
            top.ColumnDefinitions[1].Width = new GridLength(wide ? Math.Clamp(w * 0.29, 320, 420) : 0);
            Grid.SetColumnSpan(hero, wide ? 1 : 2);
            (int col, int row, int span) = wide ? (1, 0, 1) : (0, 1, 2);
            Grid.SetColumn(side, col);
            Grid.SetRow(side, row);
            Grid.SetColumnSpan(side, span);
            side.ColumnSpacing = wide ? 0 : 18;
            side.ColumnDefinitions[1].Width = wide ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
            side.RowSpacing = wide ? 18 : 0;
            Grid.SetColumnSpan(memories, wide ? 2 : 1);
            Grid.SetRow(copies, wide ? 1 : 0);
            Grid.SetColumn(copies, wide ? 0 : 1);
            Grid.SetColumnSpan(copies, wide ? 2 : 1);
        }
        Fit(Scroller.ActualWidth - 48);
        top.SizeChanged += (_, e) => Fit(e.NewSize.Width);
        return top;
    }

    // dark in both themes, as any picture with words on it
    FrameworkElement SavesHero(SavesNow now)
    {
        var l = now.Here[0];
        var g = new Grid { MinHeight = 452, Background = Argb(0xFF141414) };
        g.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(Accent(), [(0, 0), (0.5, 0), (1, 0x30)], across: true) });
        // (as a brush: a picture's own size never makes the card taller)
        var cover = new Border { IsHitTestVisible = false };
        if (_game is { } game)
            ShowWhenRead(cover, CoverPicture(l, game, now.Docs), img => cover.Background = new ImageBrush { ImageSource = img, Stretch = Stretch.UniformToFill });
        g.Children.Add(cover);
        var black = Windows.UI.Color.FromArgb(255, 10, 11, 12);
        g.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(black, [(0, 0xF0), (0.34, 0xB8), (0.66, 0x1F), (1, 0x4D)], across: true) });
        g.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(black, [(0, 0xEB), (0.6, 0)]) });
        var family = SaveImage(FamilyPicture(l));
        (family.HorizontalAlignment, family.VerticalAlignment) = (HorizontalAlignment.Right, VerticalAlignment.Bottom);
        g.Children.Add(family);

        var words = new StackPanel { Spacing = 18, Margin = new Thickness(32), MaxWidth = 500, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Bottom };
        var names = new StackPanel { Spacing = 4 };
        TextBlock White(string text, double size, byte alpha, int lines = 1) => new()
        {
            Text = text, FontSize = size, Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(alpha, 255, 255, 255)), TextWrapping = TextWrapping.Wrap, MaxLines = lines, TextTrimming = TextTrimming.CharacterEllipsis,
        };
        names.Children.Add(White(PlayedWhen(l.Save.LastPlayed), 13, 0xCC));
        var title = White(FamilyName(l), 46, 0xFF, 2);
        (title.FontFamily, title.FontWeight, title.LineHeight) = (new FontFamily("Segoe UI Variable Display"), FontWeights.SemiBold, 50);
        AutomationProperties.SetHeadingLevel(title, AutomationHeadingLevel.Level1);
        names.Children.Add(title);
        names.Children.Add(White(SavePlace(l), 15, 0xDD, 2));
        words.Children.Add(names);
        if (l.Info.Description.Trim() is { Length: > 0 } about)
        {
            var text = White(about, 14, 0xCC, 4);
            (text.MaxWidth, text.HorizontalAlignment) = (420, HorizontalAlignment.Left);
            ToolTipService.SetToolTip(text, about);
            words.Children.Add(text);
        }
        var acts = new WrapPanel { Spacing = 10 };
        if (_running)
        {
            var pill = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            pill.Children.Add(new Ellipse { Width = 8, Height = 8, Fill = Res("ApexAccentBrush"), VerticalAlignment = VerticalAlignment.Center });
            pill.Children.Add(new TextBlock { Text = T("Playing"), FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, Foreground = Argb(0xFFFFFFFF) });
            acts.Children.Add(new Border { Child = pill, Background = Argb(0x1FFFFFFF), CornerRadius = new CornerRadius(8), Padding = new Thickness(18, 0, 20, 0), Height = 56 });
        }
        else acts.Children.Add(PlayButton(l.Save.Name));
        var album = new Button { Content = T("Open album"), FontSize = 15, Height = 56, Padding = new Thickness(24, 0, 24, 0), CornerRadius = new CornerRadius(8), Background = Argb(0x800C0D0E), BorderBrush = Argb(0x38FFFFFF), Foreground = Argb(0xFFFFFFFF) };
        album.Resources["ButtonBackgroundPointerOver"] = Argb(0xA02A2B2D);
        album.Resources["ButtonBackgroundPressed"] = Argb(0xA01C1D1E);
        foreach (var state in new[] { "PointerOver", "Pressed" }) (album.Resources["ButtonForeground" + state], album.Resources["ButtonBorderBrush" + state]) = (Argb(0xFFFFFFFF), Argb(0x59FFFFFF));
        album.Click += (_, _) => OpenSave(l.Save, "mem");
        AutomationProperties.SetName(album, T("Open album") + ", " + FamilyName(l));
        acts.Children.Add(album);
        words.Children.Add(acts);
        g.Children.Add(words);

        var card = new Border { Child = g, CornerRadius = new CornerRadius(18), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), RequestedTheme = ElementTheme.Dark, ContextFlyout = SaveMenu(l) };
        // (the Sims stand clear of the words; in a narrow window they are smaller and the words keep half of it)
        void Fit(double w)
        {
            var wide = w >= 700;
            family.Height = wide ? 350 : 250;
            family.MaxWidth = Math.Max(120, w * 0.42);
            family.Margin = new Thickness(0, 0, w * 0.04, 22);
            words.MaxWidth = wide ? Math.Min(500, w * 0.5 - 16) : w;
        }
        Fit(640);
        card.SizeChanged += (_, e) => { if (Math.Abs(e.NewSize.Width - e.PreviousSize.Width) > 0.5) Fit(e.NewSize.Width); };
        return card;
    }

    // a few of the pictures the game took, spread like prints, and how many there are in all
    Button MemoriesCard(SavesNow now)
    {
        const double side = 118;
        var g = new Grid { MinHeight = 262 };
        var fan = new Canvas { Height = 168, Margin = new Thickness(0, 20, 0, 0), VerticalAlignment = VerticalAlignment.Top };
        var prints = new List<(Border Print, double At)>();
        foreach (var (at, y, turn) in new[] { (0.06, 34, -9), (0.22, 10, 4), (0.40, 30, -3), (0.57, 6, 7), (0.70, 40, -5) })
        {
            var print = new Border
            {
                Width = side, Height = side, CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(4), BorderBrush = Argb(0xFFF1EFEA), Background = Res("ApexPillBrush"), Opacity = 0,
                // (turned on its visual: a print with a RenderTransform refuses the fade)
                Rotation = turn, CenterPoint = new System.Numerics.Vector3((float)side / 2, (float)side / 2, 0), Shadow = new ThemeShadow(), Translation = new System.Numerics.Vector3(0, 0, 16),
            };
            Canvas.SetTop(print, y);
            fan.Children.Add(print);
            prints.Add((print, at));
        }
        fan.SizeChanged += (_, e) => { foreach (var (print, at) in prints) Canvas.SetLeft(print, at / 0.70 * Math.Min(0.70 * e.NewSize.Width, Math.Max(0, e.NewSize.Width - side - 14))); };
        g.Children.Add(fan);
        var number = new TextBlock { Text = " ", FontSize = 40, LineHeight = 44, FontWeight = FontWeights.SemiBold, FontFamily = new FontFamily("Segoe UI Variable Display"), VerticalAlignment = VerticalAlignment.Bottom };
        var label = SaveText(" ", 14, strong: true);
        var text = Col(label, SaveText(T("The game takes these pictures by itself"), 12, "ApexMutedBrush", lines: 2));
        ((FrameworkElement)text).VerticalAlignment = VerticalAlignment.Center;
        var foot = new Grid { ColumnSpacing = 12, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(22, 0, 22, 20) };
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.Children.Add(number);
        Grid.SetColumn((FrameworkElement)text, 1);
        foot.Children.Add(text);
        g.Children.Add(foot);
        var open = now.Here[0];
        var b = SaveButton(new Border { Child = g, CornerRadius = new CornerRadius(17) }, T("Memories"), 18, () => OpenSave(open.Save, "mem"));
        _ = Fill();
        return b;

        async Task Fill()
        {
            try
            {
                var counts = await Task.WhenAll(now.Here.Select(MemoryCount));
                var total = counts.Sum();
                var with = now.Here.Where((_, i) => counts[i] > 0).ToList();
                (number.Text, number.Visibility) = (total.ToString("N0", LangCulture()), total > 0 ? Visibility.Visible : Visibility.Collapsed);
                label.Text = total switch { 0 => T("No memories yet"), 1 => T("memory in your saves"), _ => T("memories in your saves") };
                AutomationProperties.SetName(b, total > 0 ? number.Text + " " + label.Text : label.Text);
                if (total == 0)
                {
                    var icon = Lucide.Icon("image", 44, Res("ApexIconMutedBrush"), 1.4);
                    (icon.HorizontalAlignment, icon.VerticalAlignment, icon.Margin) = (HorizontalAlignment.Center, VerticalAlignment.Top, new Thickness(0, 78, 0, 0));
                    g.Children.Add(icon);
                    return;
                }
                if (counts[0] == 0) open = with[0];
                // two saves' pictures in turn, the last played one's first
                var sets = await Task.WhenAll(with.Take(2).Select(MemoryFan));
                var pics = Enumerable.Range(0, 5).SelectMany(i => sets.Where(s => i < s.Count).Select(s => s[i])).Take(5).ToList();
                for (int i = 0; i < pics.Count; i++)
                {
                    var print = prints[i].Print;
                    if (AnimationsOn) print.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(320) };
                    print.Child = new Border { CornerRadius = new CornerRadius(6), Background = new ImageBrush { ImageSource = pics[i], Stretch = Stretch.UniformToFill } };
                    print.Opacity = 1;
                }
            }
            catch (Exception e) { App.Log(e); }
        }
    }

    // how many saves have a copy made since they were played, or not long ago
    Button CopiesCard(SavesNow now)
    {
        var total = now.Here.Count;
        var safe = now.Here.Count(l => SaveHealth.RecentCopy(l.Copy, l.Save));
        var newest = now.Here.Where(l => l.Copy is not null).MaxBy(l => l.Copy);
        var title = total == 1 ? safe == 1 ? T("Protected") : T("Not protected") : safe == 1 ? T("1 of {0} protected", total) : T("{0} of {1} protected", safe, total);
        var sub = (total - safe) switch { 0 => total == 1 ? T("It has a recent copy") : T("Every save has a recent copy"), 1 => T("1 save has no recent copy"), var n => T("{0} saves have no recent copy", n) };
        var g = new Grid { MinHeight = 172, Padding = new Thickness(22, 20, 22, 20), RowSpacing = 14 };
        g.RowDefinitions.Add(new RowDefinition());
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var head = new Grid { ColumnSpacing = 14, VerticalAlignment = VerticalAlignment.Top };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.Children.Add(CopiesRing(safe / (double)total, Res(safe == total ? "ApexSuccessBrush" : "ApexWarningBrush")));
        var words = (FrameworkElement)Col(SaveText(title, 17, strong: true, lines: 2), SaveText(sub, 13, "ApexMutedBrush", lines: 2));
        words.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(words, 1);
        head.Children.Add(words);
        g.Children.Add(head);
        // (no copy at all: there is nothing to see yet, so it makes them)
        var act = newest is null ? total == 1 ? T("Back up this save") : T("Back up all") : T("See copies");
        var button = new Border { Height = 44, CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexPillBrush"),
            Child = new TextBlock { Text = act, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } };
        Grid.SetRow(button, 1);
        g.Children.Add(button);
        var b = SaveButton(g, title + ". " + sub + ". " + act, 18, () => { if (newest is null) BackupAll(); else OpenSave(newest.Save, "time"); });
        return newest is null ? WaitsForSaves(b) : b;
    }

    static FrameworkElement CopiesRing(double share, Brush brush)
    {
        const double size = 44, w = 5, c = size / 2, r = (size - w) / 2;
        var g = new Grid { Width = size, Height = size, VerticalAlignment = VerticalAlignment.Center };
        g.Children.Add(new Ellipse { StrokeThickness = w, Stroke = share >= 1 ? brush : new SolidColorBrush(Pick(0xFF2A2A2A, 0xFFE4E4E4)) });
        if (share is <= 0 or >= 1) return g;
        var a = share * 2 * Math.PI;
        var fig = new PathFigure { StartPoint = new(c, c - r) };
        fig.Segments.Add(new ArcSegment { Point = new(c + r * Math.Sin(a), c - r * Math.Cos(a)), Size = new(r, r), IsLargeArc = share > 0.5, SweepDirection = SweepDirection.Clockwise });
        g.Children.Add(new Microsoft.UI.Xaml.Shapes.Path { Data = new PathGeometry { Figures = { fig } }, Stroke = brush, StrokeThickness = w, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round });
        return g;
    }

    // ---------- the shelf: the other households, and those out of the game ----------

    FrameworkElement SavesShelf(SavesNow now)
    {
        var s = new StackPanel { Spacing = 18 };
        var others = now.Here.Skip(1).ToList();
        var none = Muted(T("No household or world with that name."));
        var cards = new CardGrid { MinItemWidth = 232, Spacing = 16 };
        void Fill()
        {
            bool Has(string t) => t.Contains(_savesQuery, StringComparison.CurrentCultureIgnoreCase);
            var shown = others.Where(l => _savesQuery.Length == 0 || Has(FamilyName(l)) || Has(l.Save.Name) || Has(PlaceName(l))).ToList();
            if (_savesBySize) shown = shown.OrderByDescending(l => l.Info.Bytes).ToList();
            cards.Children.Clear();
            foreach (var l in shown) cards.Children.Add(SaveTile(l));
            if (now.Lost.Count > 0)
                cards.Children.Add(AwayTile(now.Lost.Count == 1 ? T("1 lost household") : T("{0} lost households", now.Lost.Count), now.Lost.Count == 1 ? T("The game still keeps a copy of it") : T("The game still keeps a copy of them"),
                    now.Lost, true, () => SavesGo(LostPage)));
            if (now.Out.Count > 0)
                cards.Children.Add(AwayTile(T("Out of the game"), now.Out.Count == 1 ? T("1 save the game doesn't see") : T("{0} saves the game doesn't see", now.Out.Count), now.Out, false, () => SavesGo(OutPage)));
            none.Visibility = others.Count > 0 && shown.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }
        if (now.Here.Count > 0)
        {
            var bar = new Grid { ColumnSpacing = 16 };
            bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            bar.ColumnDefinitions.Add(new ColumnDefinition());
            var title = SaveText(others.Count > 0 ? T("Other households") : "", 18, strong: true);
            (title.VerticalAlignment, title.Margin) = (VerticalAlignment.Center, new Thickness(2, 0, 0, 0));
            AutomationProperties.SetHeadingLevel(title, AutomationHeadingLevel.Level2);
            bar.Children.Add(title);
            var tools = new WrapPanel { Spacing = 10, HorizontalAlignment = HorizontalAlignment.Right };
            if (others.Count > 1)
            {
                var find = new TextBox { PlaceholderText = T("Search household or world"), Text = _savesQuery, Width = 250, Height = 36 };
                AutomationProperties.SetName(find, T("Search household or world"));
                var typed = 0;
                find.TextChanged += (_, _) =>
                {
                    var mine = ++typed;
                    After(160, () => { if (mine != typed || find.Text.Trim() == _savesQuery) return; _savesQuery = find.Text.Trim(); Fill(); });
                };
                find.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Escape && find.Text.Length > 0) { find.Text = ""; e.Handled = true; } };
                tools.Children.Add(find);
                var sort = new Button { Height = 36 };
                void Say() { sort.Content = _savesBySize ? T("Largest") : T("Newest"); AutomationProperties.SetName(sort, T("Sort by") + ": " + sort.Content); }
                Say();
                sort.Click += (_, _) => { _savesBySize = !_savesBySize; Say(); Fill(); };
                tools.Children.Add(sort);
            }
            else _savesQuery = "";
            tools.Children.Add(WaitsForSaves(new Button { Content = others.Count == 0 ? T("Back up this save") : T("Back up all"), Height = 36 }, BackupAll));
            Grid.SetColumn(tools, 1);
            bar.Children.Add(tools);
            s.Children.Add(bar);
        }
        s.Children.Add(none);
        s.Children.Add(cards);
        Fill();
        return s;
    }

    // a household before one of its lots; Play in the corner under the pointer, the rest on its page and in its menu
    FrameworkElement SaveTile(SaveLook l)
    {
        var (name, place, size) = (FamilyName(l), SavePlace(l), SpSize(l.Info.Bytes));
        var g = new Grid { Height = 244 };
        var lot = SaveImage(LotPicture(l), 0.62);
        // (it fills the card however wide: a lot's picture has an edge of its own)
        (lot.Stretch, lot.Height, lot.Margin, lot.HorizontalAlignment, lot.VerticalAlignment) = (Stretch.UniformToFill, 250, new Thickness(0, -14, 0, 0), HorizontalAlignment.Center, VerticalAlignment.Top);
        g.Children.Add(lot);
        g.Children.Add(new Border { IsHitTestVisible = false, Background = Fade(((SolidColorBrush)Res("ApexCardBrush")).Color, [(0, 0xF8), (0.14, 0xF5), (0.52, 0x59), (0.8, 0)]) });
        var stand = new Grid { Height = 150, MaxWidth = 190, Margin = new Thickness(22, 0, 0, 74), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Bottom };
        var family = SaveImage(FamilyPicture(l), none: () => stand.Children.Add(NoPicture(54)));
        family.HorizontalAlignment = HorizontalAlignment.Left;
        stand.Children.Add(family);
        g.Children.Add(stand);
        var foot = new Grid { ColumnSpacing = 10, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(18, 0, 18, 16) };
        foot.ColumnDefinitions.Add(new ColumnDefinition());
        foot.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        foot.Children.Add(Col(SaveText(name, 17, strong: true), SaveText(place, 12, "ApexSoftTextBrush")));
        var mb = SaveText(size, 12, "ApexSoftTextBrush");
        mb.VerticalAlignment = VerticalAlignment.Bottom;
        Grid.SetColumn(mb, 1);
        foot.Children.Add(mb);
        g.Children.Add(foot);
        var b = SaveButton(new Border { Child = g, CornerRadius = new CornerRadius(15) }, $"{name}, {place}, {PlayedWhen(l.Save.LastPlayed)}, {size}", 16, () => OpenSave(l.Save, "mem"));
        b.ContextFlyout = SaveMenu(l);
        ToolTipService.SetToolTip(b, name + "\n" + place + "\n" + PlayedWhen(l.Save.LastPlayed));
        var play = new Button { Height = PlaySize, CornerRadius = new CornerRadius(PlaySize / 2), Padding = new Thickness(0), BorderThickness = new Thickness(0), IsEnabled = !_busy && !Maintenance && !_running };
        ToolTipService.SetToolTip(play, T("Play this save"));
        AutomationProperties.SetName(play, T("Play {0}", l.Save.Name));
        play.Click += async (_, _) => await PlayAsync(l.Save.Name, update: _settings.InstallUpdatesOnPlay);
        var cell = new Grid();
        cell.Children.Add(b);
        var (corner, over) = PlayCorner(play);
        (corner.VerticalAlignment, corner.Margin) = (VerticalAlignment.Top, new Thickness(0, 14, 14, 0));
        // (the button's own visual is animated by PlayCorner, so what fades is a layer around it)
        var layer = new Grid { Children = { corner }, Opacity = 0, IsHitTestVisible = false, OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) } };
        var (hover, focus) = (false, false);
        void Show() { layer.Opacity = hover || focus ? 1 : 0; layer.IsHitTestVisible = hover; over(hover); } // (a touch on the card never starts the game)
        play.GotFocus += (_, _) => { focus = true; Show(); };
        play.LostFocus += (_, _) => { focus = false; Show(); };
        cell.Children.Add(layer);
        Lift(cell, () => { hover = true; Show(); }, () => { hover = false; Show(); });
        return cell;
    }

    MenuFlyout SaveMenu(SaveLook l)
    {
        var menu = new MenuFlyout();
        var play = MenuItem(T("Play this save"), () => _ = PlayAsync(l.Save.Name, update: _settings.InstallUpdatesOnPlay));
        menu.Items.Add(play);
        menu.Items.Add(MenuItem(T("Open album"), () => OpenSave(l.Save, "mem")));
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(MenuItem(T("Back up this save"), () => _ = BackupAsync([l.Save])));
        menu.Items.Add(MenuItem(T("Open folder"), () => OpenFolder(l.Save.Dir)));
        menu.Opening += (_, _) => play.IsEnabled = !_busy && !Maintenance && !_running;
        return menu;
    }

    // a dashed card with the households that are not in the game
    FrameworkElement AwayTile(string title, string sub, List<SaveLook> looks, bool grey, Action go)
    {
        var g = new Grid { Height = 244 };
        g.Children.Add(new Rectangle { RadiusX = 16, RadiusY = 16, StrokeThickness = 1.4, StrokeDashArray = [4, 3], Stroke = new SolidColorBrush(Pick(0xFF3E3E3E, 0xFFB9B9B9)), IsHitTestVisible = false });
        var row = new Grid { Margin = new Thickness(16, 24, 16, 74), ColumnSpacing = 4, VerticalAlignment = VerticalAlignment.Bottom };
        foreach (var (l, i) in looks.Take(3).Select((l, i) => (l, i)))
        {
            row.ColumnDefinitions.Add(new ColumnDefinition());
            var pic = SaveImage(FamilyPicture(l, grey), grey ? 0.75 : 1);
            (pic.Height, pic.VerticalAlignment) = (new[] { 128, 140, 124 }[i], VerticalAlignment.Bottom);
            Grid.SetColumn(pic, i);
            row.Children.Add(pic);
        }
        g.Children.Add(row);
        var words = (FrameworkElement)Col(SaveText(title, 17, strong: true), SaveText(sub, 12, "ApexSoftTextBrush"));
        (words.VerticalAlignment, words.Margin) = (VerticalAlignment.Bottom, new Thickness(18, 0, 18, 16));
        g.Children.Add(words);
        var b = SaveButton(g, title + ". " + sub, 16, go);
        (b.BorderThickness, b.Background) = (new Thickness(0), new SolidColorBrush(Pick(0xFF111111, 0xFFF4F4F4)));
        ToolTipService.SetToolTip(b, title + "\n" + sub);
        var cell = new Grid { Children = { b } };
        Lift(cell);
        return cell;
    }

    // ---------- the households that are not in the game ----------

    void DrawAway(List<SaveLook> looks, bool lost)
    {
        var head = new Grid { ColumnSpacing = 16 };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        var back = new Button { Content = Lucide.Icon("chevron-left", 18, Res("TextFillColorPrimaryBrush")), Width = 44, Height = 44, Padding = new Thickness(0), CornerRadius = new CornerRadius(22), VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetName(back, T("Back to Saves"));
        AutomationProperties.SetAutomationId(back, "SavesBack");
        ToolTipService.SetToolTip(back, T("Back to Saves"));
        back.Click += (_, _) => SavesGo("");
        head.Children.Add(back);
        var title = Heading(lost ? T("Lost households") : T("Out of the game"));
        AutomationProperties.SetHeadingLevel(title, AutomationHeadingLevel.Level1);
        var words = (FrameworkElement)Col(title, Body(lost ? T("The save is gone, but the game kept the version before it. Bring back who you miss.") : T("The game doesn't see these saves. Put one back to play it.")));
        Grid.SetColumn(words, 1);
        head.Children.Add(words);
        SavesHost.Children.Add(head);
        var cards = new CardGrid { MinItemWidth = 300, Spacing = 18 };
        foreach (var l in looks) cards.Children.Add(AwayCard(l, lost));
        SavesHost.Children.Add(cards);
        DispatcherQueue.TryEnqueue(() => back.Focus(FocusState.Programmatic));
    }

    // a lost household is grey; back in the game, its colours return
    FrameworkElement AwayCard(SaveLook l, bool lost)
    {
        var g = new Grid { MinHeight = 560 };
        g.RowDefinitions.Add(new RowDefinition { MinHeight = 350 });
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var colours = new List<(UIElement Grey, UIElement Colour, double Dim, double Full)>();
        void Pair(Grid host, Func<bool, Task<ImageSource?>> read, double dim, double full, Action? none = null)
        {
            // (each in a layer of its own: the picture fades in by itself, the layers change over)
            var (grey, colour) = (new Grid(), new Grid());
            if (lost) grey.Children.Add(SaveImage(read(true)));
            colour.Children.Add(SaveImage(read(false), none: none));
            host.Children.Add(grey);
            host.Children.Add(colour);
            colours.Add((grey, colour, dim, full));
        }
        var lot = new Grid { Height = 380, Margin = new Thickness(0, 10, 0, 0), VerticalAlignment = VerticalAlignment.Top, IsHitTestVisible = false };
        Pair(lot, grey => LotPicture(l, grey), 0.4, 0.85);
        Grid.SetRowSpan(lot, 2);
        g.Children.Add(lot);
        var shade = new Border { IsHitTestVisible = false, Background = Fade(((SolidColorBrush)Res("ApexCardBrush")).Color, [(0, 0xFA), (0.3, 0xF2), (0.62, 0x4D), (1, 0)]) };
        Grid.SetRowSpan(shade, 2);
        g.Children.Add(shade);
        var family = new Grid { Height = 270, Margin = new Thickness(22, 40, 22, 14), VerticalAlignment = VerticalAlignment.Bottom, IsHitTestVisible = false };
        Pair(family, grey => FamilyPicture(l, grey), 1, 1, () => family.Children.Add(NoPicture(84)));
        g.Children.Add(family);

        var words = new StackPanel { Spacing = 12, Margin = new Thickness(22, 0, 22, 22) };
        var place = SaveText(SavePlace(l) + ", " + SpSize(l.Info.Bytes), 14, "ApexSoftTextBrush");
        ToolTipService.SetToolTip(place, place.Text);
        words.Children.Add(Col(SaveText(FamilyName(l), 22, strong: true), place));
        var about = l.Info.Description.Trim() is { Length: > 0 } d ? d : PlayedWhen(l.Save.LastPlayed);
        var text = SaveText(about, 13, "ApexSoftTextBrush", lines: 3);
        (text.LineHeight, text.Height) = (18, 54); // (the names and the buttons of a row of cards stand in line)
        ToolTipService.SetToolTip(text, about);
        words.Children.Add(text);
        var act = new Border { MinHeight = 48 };
        words.Children.Add(act);
        Grid.SetRow(words, 1);
        g.Children.Add(words);
        var card = new Border { Child = g, CornerRadius = new CornerRadius(18), BorderThickness = new Thickness(1), Background = Res("ApexCardBrush") };
        Show(_back.ContainsKey(l.Save.Dir), animate: false);
        return card;

        void Show(bool here, bool animate)
        {
            foreach (var (grey, colour, dim, full) in colours)
            {
                if (animate && AnimationsOn) foreach (var e in new[] { grey, colour }) e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(900) };
                grey.Opacity = here ? 0 : dim;
                colour.Opacity = here || !lost ? full : 0;
            }
            card.BorderBrush = Res(here ? "ApexAccentBrush" : "ApexCardBorderBrush");
            if (here)
            {
                var done = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                done.Children.Add(Lucide.Icon("check", 16, Res("ApexSuccessBrush")));
                done.Children.Add(new TextBlock { Text = T("Back in the game"), FontWeight = FontWeights.SemiBold, Foreground = Res("ApexSuccessBrush") });
                act.Child = new Border { Child = done, Height = 48, CornerRadius = new CornerRadius(8), Background = Res("ApexSuccessSoftBrush") };
                return;
            }
            var b = MainButton(lost ? T("Bring back") : T("Put back"), () => { });
            b.Height = 48;
            b.FontWeight = FontWeights.SemiBold;
            AutomationProperties.SetName(b, (string)b.Content + ", " + FamilyName(l));
            act.Child = WaitsForSaves(b, () => _ = BringBack(l, lost, b, () => Show(true, animate: true)));
        }
    }

    async Task BringBack(SaveLook l, bool lost, Button b, Action shown)
    {
        if (_savesBusy) return;
        var (docs, label) = (Docs, b.Content);
        SavesBusy(true);
        b.Content = new ProgressRing { IsActive = true, Width = 18, Height = 18 };
        try
        {
            var save = await Task.Run(() => lost ? SaveCopies.Rescue(docs, l.Save) : SaveCopies.Unstore(docs, l.Save));
            _back[l.Save.Dir] = save;
            shown();
            SetInfo(SavesInfo, InfoBarSeverity.Success, T("{0} is back", FamilyName(l)), save.Name, T("Undo"), () => _ = UndoBringBack(docs, save, lost));
            try { _savesNow = await Task.Run(() => ReadSaves(docs)); } catch (Exception e) { App.Log(e); } // (Saves opens on what is true now)
        }
        catch (Exception e)
        {
            App.Log(e);
            b.Content = label;
            SetInfo(SavesInfo, InfoBarSeverity.Error, T("The save did not come back"), ErrorText(e));
        }
        finally { SavesBusy(false); }
    }

    // the game's own copy of a lost save never left its place, and a save put back goes out again
    async Task UndoBringBack(string docs, Save save, bool lost)
    {
        if (_savesBusy) return;
        SavesBusy(true);
        try
        {
            await Task.Run(() =>
            {
                if (!lost) { SaveCopies.Store(docs, save); return; }
                if (SaveInfo.Of(save).LastPlayed != save.LastPlayed) throw new SetupException("This save was played since it came back, so it stays.");
                SaveCopies.Delete(save);
            });
            SavesInfo.IsOpen = false;
            _savesNow = await Task.Run(() => ReadSaves(docs));
            if (_step == SavesScreen && _saveView is null) SavesGo(_savesPage);
        }
        catch (Exception e) { App.Log(e); SetInfo(SavesInfo, InfoBarSeverity.Error, T("The save stayed as it is"), ErrorText(e)); }
        finally { SavesBusy(false); }
    }

    // ---------- copies ----------

    // a button that waits while a save is copied or moved
    Button WaitsForSaves(Button b, Action? click = null)
    {
        if (click is not null) b.Click += (_, _) => click();
        b.IsEnabled = !_savesBusy;
        _savesWork.Add(b);
        return b;
    }

    void SavesBusy(bool on)
    {
        _savesBusy = on;
        foreach (var c in _savesWork) c.IsEnabled = !on;
    }

    void BackupAll() => _ = BackupAsync(_savesNow?.Here.Select(l => l.Save).ToList() ?? Saves.List(Docs));

    async Task BackupAsync(List<Save> saves)
    {
        if (_savesBusy || saves.Count == 0) return;
        var docs = Docs;
        SavesBusy(true);
        try
        {
            for (int i = 0; i < saves.Count; i++)
            {
                var s = saves[i];
                SetInfo(SavesInfo, InfoBarSeverity.Informational, saves.Count == 1 ? T("Backing up…") : T("Backing up {0} of {1}…", i + 1, saves.Count), s.Name);
                await Task.Run(() => Saves.Backup(s, docs));
            }
            SetInfo(SavesInfo, InfoBarSeverity.Success, saves.Count == 1 ? T("Save backed up") : T("{0} saves backed up", saves.Count), Saves.BackupDir(docs), T("Open folder"), () => OpenFolder(Saves.BackupDir(docs)));
        }
        catch (Exception e) { App.Log(e); SetInfo(SavesInfo, InfoBarSeverity.Error, T("The backup did not finish"), ErrorText(e)); }
        finally { SavesBusy(false); if (_step == SavesScreen) FillSaves(); }
    }
}
