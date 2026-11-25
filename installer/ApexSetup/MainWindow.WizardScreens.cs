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
            GameSub.Text = T("The launcher found these installs. Pick the one you play.");
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
            GameTitle.Text = T("Which is your game?");
            GameSub.Visibility = Visibility.Collapsed;
            GameList.Visibility = GameBrowse.Visibility = Visibility.Visible;
            return;
        }
        GameTitle.Text = T("Your game");
        GameSub.Visibility = Visibility.Collapsed;
        GameList.Visibility = GameBrowse.Visibility = Visibility.Collapsed;
        if (_game is { } g) GamePanel.Children.Add(GameCard(g));
        // (nothing in the way: the panel's checks say it; the "no incompatible settings" line below would repeat it)
        GameDetails.Visibility = _gameAllSet || _state is null ? Visibility.Collapsed : Visibility.Visible;
        GamePanel.Visibility = Visibility.Visible;
    }

    // one game: its name, Change folder, then that all is set or what stands in the way
    Border GameCard(Game g)
    {
        var head = new Grid { ColumnSpacing = 14, Padding = new Thickness(18, 16, 18, 16) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var mark = Lucide.Icon("gamepad-2", 22, Res("ApexSoftTextBrush"));
        mark.VerticalAlignment = VerticalAlignment.Center;
        head.Children.Add(mark);
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        words.Children.Add(new TextBlock { Text = "The Sims 3", FontSize = 16, FontWeight = FontWeights.SemiBold });
        words.Children.Add(new TextBlock { Text = (g.Label + " " + g.Version).Trim(), FontSize = 13, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(words, 1); head.Children.Add(words);
        var change = new Button { Content = T("Change folder"), VerticalAlignment = VerticalAlignment.Center };
        change.Click += OnBrowse;
        Grid.SetColumn(change, 2); head.Children.Add(change);

        var body = new StackPanel { Padding = new Thickness(18, 10, 18, 10), BorderThickness = new Thickness(0, 1, 0, 0), BorderBrush = Res("ApexHairlineBrush") };
        var findings = _state is { } s ? GpuFindings(s).Concat(_found.Where(x => x.Suggest is null)).ToList() : [];
        _gameAllSet = findings.Count == 0 && !_running;
        if (findings.Any(f => f.Severity == Severity.Error)) body.Children.Add(Check(false, T("Version {0}", g.Version)));
        if (_vulkan is { Card: not null } && (OldCard || OldDriver)) body.Children.Add(Check(false, BridgeDetail.Text));
        try
        {
            var free = Engine.FreeBytes(g.Bin);
            var need = Engine.DownloadEstimate(Plan().Install, c => Repo.GetValueOrDefault(c) is { } r ? _latest.GetValueOrDefault(r) : null);
            if (free <= Math.Max(need * 2, 200L << 20))
            {
                _gameAllSet = false;
                body.Children.Add(Check(false, T("{0} GB free", (free >> 30).ToString()) + " · " + T("the install uses about {0} MB", Math.Max(1, need >> 20).ToString())));
            }
        }
        catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException) { }
        if (_gameAllSet && body.Children.Count == 0) body.Children.Add(Check(true, T("All set to install.")));
        body.Visibility = body.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        var all = new StackPanel();
        all.Children.Add(head);
        all.Children.Add(body);
        return new Border { Child = all, CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexHairlineBrush") };
    }

    FrameworkElement Check(bool ok, string what)
    {
        var p = new Grid { ColumnSpacing = 10, Padding = new Thickness(0, 6, 0, 6) };
        p.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        p.ColumnDefinitions.Add(new ColumnDefinition());
        p.Children.Add(Lucide.Icon(ok ? "check" : "triangle-alert", 14, Res(ok ? "ApexSuccessBrush" : "ApexWarningBrush"), ok ? 2.5 : 2));
        var t = new TextBlock { Text = what, FontSize = 13, Foreground = Res("ApexSoftTextBrush"), VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
        Grid.SetColumn(t, 1);
        p.Children.Add(t);
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

    static StackPanel Labeled(string icon, string text)
    {
        var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        p.Children.Add(Lucide.Icon(icon, 15, App.Brush("ApexSoftTextBrush")));
        p.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
        return p;
    }
}
