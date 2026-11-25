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
        GameSub.Visibility = Visibility.Visible;
        _gameAllSet = false;
        if (!wizard)
        {
            BackOnce(GameHead);
            GameTitle.Text = T("Choose your game");
            GameSub.Text = T("The launcher found these installs. Choose the one you play.");
            GameList.Visibility = GameBrowse.Visibility = Visibility.Visible;
            GameDetails.Visibility = _state is null ? Visibility.Collapsed : Visibility.Visible;
            return;
        }
        if (GameHead.Children[0] is Button) GameHead.Children.RemoveAt(0);
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
            GameTitle.Text = T("Choose your game");
            GameSub.Text = T("The launcher found {0} installs. Apex Radiance goes only into the one you choose.", _games.Count);
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

    // one game: its name and packs, Change folder, then what the launcher checked, side by side
    Border GameCard(Game g)
    {
        var head = new Grid { ColumnSpacing = Design.Space.IconToText, Padding = new Thickness(Design.Space.CardPad) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(Design.IconTile.Of("gamepad-2"));
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = "The Sims 3", FontSize = 16, FontWeight = FontWeights.SemiBold });
        words.Children.Add(new TextBlock { Text = g.Label + " \u00b7 " + Packs(g), FontSize = 13, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(words, 1); head.Children.Add(words);
        var change = new Button { Content = T("Change folder"), MinHeight = Design.Size.Control, Padding = new Thickness(14, 0, 14, 0), VerticalAlignment = VerticalAlignment.Center };
        change.Click += OnBrowse;
        ToolTipService.SetToolTip(change, g.Root);
        Grid.SetColumn(change, 2); head.Children.Add(change);

        var findings = _state is { } s ? GpuFindings(s).Concat(_found.Where(x => x.Suggest is null)).ToList() : [];
        var checks = new List<(bool Ok, string What, string Value)> { (!findings.Any(f => f.Severity == Severity.Error), T("Version"), g.Version) };
        if (_vulkan is { Card: { } card }) checks.Add((!OldCard && !OldDriver, T("Graphics"), card.Replace("NVIDIA GeForce ", "").Replace("AMD Radeon ", "Radeon ")));
        try
        {
            var free = Engine.FreeBytes(g.Bin);
            var need = Engine.DownloadEstimate(Plan().Install, c => Repo.GetValueOrDefault(c) is { } r ? _latest.GetValueOrDefault(r) : null);
            checks.Add((free > Math.Max(need * 2, 200L << 20), T("Free space"), T("{0} GB", (free >> 30).ToString())));
        }
        catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException) { }
        _gameAllSet = findings.Count == 0 && !_running && checks.All(c => c.Ok);

        var facts = new StackPanel();
        facts.Children.Add(Fact(null, T("Folder"), g.Root));
        foreach (var (ok, what, value) in checks) facts.Children.Add(Fact(ok, what, value));
        var all = new StackPanel { Children = { head, facts } };
        if (OldCard || OldDriver) all.Children.Add(new TextBlock { Text = BridgeDetail.Text, FontSize = 13, LineHeight = 18, Foreground = Res("ApexWarningTextBrush"), TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(Design.Space.CardPad, 0, Design.Space.CardPad, Design.Space.CardPad) });
        return new Border { Child = all, CornerRadius = new CornerRadius(Design.Radius.Card), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexRaisedBrush") };
    }

    // something checked: its mark, what it is, and what was found on the right
    // (ok null: a fact, not a check, shown whole)
    static FrameworkElement Fact(bool? ok, string what, string value)
    {
        var g = new Grid { ColumnSpacing = Design.Space.X3, MinHeight = 52, Padding = new Thickness(Design.Space.CardPad, Design.Space.X3, Design.Space.CardPad, Design.Space.X3), BorderThickness = new Thickness(0, 1, 0, 0), BorderBrush = Res("ApexHairlineBrush") };
        foreach (var w in new[] { GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star) }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var mark = Lucide.Icon(ok switch { null => "folder", true => "circle-check", _ => "triangle-alert" }, 18, Res(ok switch { null => "ApexMutedBrush", true => "ApexSuccessBrush", _ => "ApexWarningBrush" }));
        mark.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(mark);
        var name = new TextBlock { Text = what, FontSize = 14, Foreground = Res("ApexSoftTextBrush"), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(name, 1);
        g.Children.Add(name);
        var found = FactText(value);
        if (ok is null) (found.TextWrapping, found.TextTrimming) = (TextWrapping.Wrap, TextTrimming.None);
        (found.FontSize, found.HorizontalAlignment) = (14, HorizontalAlignment.Right);
        Grid.SetColumn(found, 2);
        g.Children.Add(found);
        ToolTipService.SetToolTip(g, what + ": " + value);
        return g;
    }

    // no game found: where it is, said once, and the folder picker
    Border FindGameCard()
    {
        var p = new StackPanel { Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center, Padding = new Thickness(34) };
        var tile = Design.IconTile.Of("folder", 52);
        tile.HorizontalAlignment = HorizontalAlignment.Center;
        p.Children.Add(tile);
        p.Children.Add(new TextBlock { Text = T("Choose The Sims 3 folder"), FontSize = 16, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center });
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
