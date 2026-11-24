using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// The install's own screens, as the install prototype draws them: one game with its checks (or the installs to choose from,
// or where to find the game).
public sealed partial class MainWindow
{
    bool _gameAllSet; // the game step found nothing in the way (its footer says so)
    readonly Dictionary<string, string> _packsText = new(StringComparer.OrdinalIgnoreCase);

    void FillGameStep()
    {
        var wizard = Wizard && _step == GameStep;
        GamePanel.Children.Clear();
        GamePanel.Visibility = Visibility.Collapsed;
        _gameAllSet = false;
        if (!wizard)
        {
            GameTitle.Text = T("Choose your game");
            GameSub.Text = T("The launcher found these installs. Pick the one you play.");
            GameList.Visibility = GameBrowse.Visibility = Visibility.Visible;
            GameDetails.Visibility = _state is null ? Visibility.Collapsed : Visibility.Visible;
            return;
        }
        NoGames.Visibility = Visibility.Collapsed;
        if (_games.Count == 0)
        {
            GameTitle.Text = T("Where is your game?");
            GameSub.Text = T("The launcher didn't find The Sims 3 in the usual folders.");
            GameList.Visibility = GameBrowse.Visibility = GameDetails.Visibility = Visibility.Collapsed;
            GamePanel.Children.Add(FindGameCard());
            GamePanel.Visibility = Visibility.Visible;
            return;
        }
        if (_games.Count > 1)
        {
            GameTitle.Text = T("Which is your game?");
            GameSub.Text = T("The launcher found {0} installs. Apex goes only into the one you choose.", _games.Count);
            GameList.Visibility = GameBrowse.Visibility = Visibility.Visible;
            return;
        }
        GameTitle.Text = T("Your game");
        GameSub.Text = T("Apex Radiance goes into this install only.");
        GameList.Visibility = GameBrowse.Visibility = Visibility.Collapsed;
        if (_game is { } g) GamePanel.Children.Add(GameCard(g));
        // (nothing in the way: the panel's checks say it; the "no incompatible settings" line below would repeat it)
        GameDetails.Visibility = _gameAllSet || _state is null ? Visibility.Collapsed : Visibility.Visible;
        GamePanel.Visibility = Visibility.Visible;
    }

    // one game: its name and packs, Change folder, then its folder and what the launcher checked
    Border GameCard(Game g)
    {
        var head = new Grid { ColumnSpacing = 14, Padding = new Thickness(18, 16, 18, 16) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(new Border { Width = 44, Height = 44, CornerRadius = new CornerRadius(12), Background = Res("ApexPillBrush"), VerticalAlignment = VerticalAlignment.Center,
            Child = Lucide.Icon("gamepad-2", 22, Res("ApexAccentBrush")) });
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = "The Sims 3", FontSize = 16, FontWeight = FontWeights.SemiBold });
        words.Children.Add(new TextBlock { Text = g.Label + " · " + Packs(g), FontSize = 13, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(words, 1); head.Children.Add(words);
        var change = new Button { Content = Labeled("folder", T("Change folder")), VerticalAlignment = VerticalAlignment.Center };
        change.Click += OnBrowse;
        Grid.SetColumn(change, 2); head.Children.Add(change);

        var body = new StackPanel { Padding = new Thickness(18, 4, 18, 10), BorderThickness = new Thickness(0, 1, 0, 0), BorderBrush = Res("ApexHairlineBrush") };
        var path = new TextBlock { Text = g.Root, FontFamily = new FontFamily("Consolas"), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 10, 0, 2) };
        ToolTipService.SetToolTip(path, g.Root);
        body.Children.Add(path);
        var findings = _state is { } s ? GpuFindings(s).Concat(_found.Where(x => x.Suggest is null)).ToList() : [];
        _gameAllSet = findings.Count == 0 && !_running;
        // the version, the card and the space: what the launcher read, each with what it means for the install
        body.Children.Add(Check(true, T("Version {0}", g.Version), findings.Any(f => f.Severity == Severity.Error) ? "" : T("the one Apex Radiance uses")));
        if (_vulkan is { Card: { } card } vk)
            body.Children.Add(Check(!OldCard && !OldDriver, card, OldCard || OldDriver ? BridgeDetail.Text : T("Vulkan {0}, ready for the bridge", vk.Version?.ToString(2) ?? "")));
        try
        {
            var free = Engine.FreeBytes(g.Bin);
            var need = Engine.DownloadEstimate(Plan().Install, c => Repo.GetValueOrDefault(c) is { } r ? _latest.GetValueOrDefault(r) : null);
            body.Children.Add(Check(free > Math.Max(need * 2, 200L << 20), T("{0} GB free", (free >> 30).ToString()), need > 0 ? T("the install uses about {0} MB", Math.Max(1, need >> 20).ToString()) : ""));
        }
        catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException) { }
        var all = new StackPanel();
        all.Children.Add(head);
        all.Children.Add(body);
        return new Border { Child = all, CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexHairlineBrush") };
    }

    FrameworkElement Check(bool ok, string what, string meaning)
    {
        var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Padding = new Thickness(0, 8, 0, 8) };
        p.Children.Add(Lucide.Icon(ok ? "check" : "triangle-alert", 14, Res(ok ? "ApexSuccessBrush" : "ApexWarningBrush"), ok ? 2.5 : 2));
        p.Children.Add(new TextBlock { Text = what, FontSize = 13, Foreground = Res("ApexSoftTextBrush"), VerticalAlignment = VerticalAlignment.Center });
        if (meaning.Length > 0) p.Children.Add(new TextBlock { Text = "· " + meaning, FontSize = 13, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap });
        return p;
    }

    // no game found: where it is, said once, and the folder picker
    Border FindGameCard()
    {
        var p = new StackPanel { Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center, Padding = new Thickness(34) };
        p.Children.Add(new Border { Width = 52, Height = 52, CornerRadius = new CornerRadius(14), Background = Res("ApexPillBrush"), HorizontalAlignment = HorizontalAlignment.Center,
            Child = Lucide.Icon("folder", 24, Res("ApexSoftTextBrush")) });
        p.Children.Add(new TextBlock { Text = T("Choose the The Sims 3 folder"), FontSize = 15, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center });
        p.Children.Add(new TextBlock { Text = T("The one with Game\\Bin\\TS3W.exe inside. In Steam: Library › right-click the game › Manage › Browse local files."),
            FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center, MaxWidth = 440, HorizontalAlignment = HorizontalAlignment.Center });
        var pick = new Button { Content = Labeled("folder", T("Choose folder")), Style = (Style)Application.Current.Resources["AccentButtonStyle"], HorizontalAlignment = HorizontalAlignment.Center };
        pick.Click += OnBrowse;
        p.Children.Add(pick);
        var dashed = new Microsoft.UI.Xaml.Shapes.Rectangle { RadiusX = 16, RadiusY = 16, Stroke = Res("ApexCardBorderBrush"), StrokeThickness = 1.5, StrokeDashArray = [4, 3] };
        var g = new Grid();
        g.Children.Add(dashed);
        g.Children.Add(p);
        return new Border { Child = g, CornerRadius = new CornerRadius(16), Background = Res("ApexHairlineBrush") };
    }

    // "11 expansions and 9 stuff packs", read once per game
    string Packs(Game g)
    {
        if (_packsText.TryGetValue(g.Bin, out var t)) return t;
        var found = Expansions.Installed(g);
        var ep = found.Count(x => x.Pack.Expansion);
        var sp = found.Count - ep;
        string Ep() => ep == 1 ? T("1 expansion") : T("{0} expansions", ep);
        string Sp() => sp == 1 ? T("1 stuff pack") : T("{0} stuff packs", sp);
        t = ep > 0 && sp > 0 ? T("{0} and {1}", Ep(), Sp()) : ep > 0 ? Ep() : sp > 0 ? Sp() : T("Base game");
        return _packsText[g.Bin] = t;
    }

    static StackPanel Labeled(string icon, string text)
    {
        var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        p.Children.Add(Lucide.Icon(icon, 15, App.Brush("ApexSoftTextBrush")));
        p.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
        return p;
    }
}
