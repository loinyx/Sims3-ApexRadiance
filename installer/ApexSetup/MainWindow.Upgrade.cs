using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// The move to Apex Launcher, in the install's own look (the aurora, one card, the header with the steps): an Apex Radiance
// Setup that updated itself says what changed and what came with it; any copy not installed yet asks where it lives; the
// installed copy it hands over to ends on Ready, which can delete the copy left in Downloads.
public sealed partial class MainWindow
{
    public const string MovedFromArg = "--moved-from=";
    int _upgradeStage;
    bool _upgradeInstall = true, _upgradeDelete = true;
    string? _upgradeFrom; // (Ready: the copy this one was installed from)

    string[] UpgradeSteps => Paths.MovedFromSetup || _upgradeWasSetup ? [T("What's new"), T("Where it lives"), T("Ready")] : [T("Where it lives"), T("Ready")];
    bool _upgradeWasSetup, _started, _upgradeAsked; // (asked once per run: Ask me later waits for the next start)

    // a copy not installed yet: the card instead of the dialog; the installed copy, started from one: Ready
    bool StartUpgrade()
    {
        var args = Environment.GetCommandLineArgs().Skip(1).ToList();
        if (args.FirstOrDefault(a => a.StartsWith(MovedFromArg, StringComparison.Ordinal)) is { } from && LauncherInstall.Mode == LauncherMode.Installed)
        {
            _upgradeFrom = from[MovedFromArg.Length..];
            _upgradeWasSetup = args.Contains("--was-setup");
            UpgradeShow(UpgradeSteps.Length - 1);
            return true;
        }
        if (Maintenance || FirstInstall || LauncherInstall.Mode != LauncherMode.NotChosen || Environment.ProcessPath is null) return false;
        UpgradeShow(0);
        return true;
    }

    void UpgradeShow(int stage)
    {
        _upgradeAsked = true;
        _upgradeStage = stage;
        var steps = UpgradeSteps;
        var name = steps[Math.Min(stage, steps.Length - 1)];
        var card = new StackPanel();
        if (name == T("What's new")) FillUpgradeNews(card);
        else if (name == T("Where it lives")) FillUpgradeWhere(card, steps);
        else FillUpgradeReady(card, steps);
        UpgradeCard.Child = card;
        UpgradeRun.Visibility = Visibility.Visible;
        UpgradeAurora.Run(true);
        Animate(card);
    }

    void UpgradeClose()
    {
        UpgradeRun.Visibility = Visibility.Collapsed;
        UpgradeAurora.Run(false);
        UpgradeCard.Child = null;
    }

    // ---------- the card's parts ----------

    // the header: the launcher and a dot for each step
    FrameworkElement UpgradeHeader(string[] steps, int current)
    {
        var g = new Grid { Padding = new Thickness(32, 17, 32, 17), ColumnSpacing = 12, BorderThickness = new Thickness(0, 0, 0, 1), BorderBrush = Res("ApexHairlineBrush") };
        for (int i = 0; i < 3; i++) g.ColumnDefinitions.Add(new ColumnDefinition { Width = i == 2 ? new GridLength(1, GridUnitType.Star) : GridLength.Auto });
        g.Children.Add(new Image { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = 30, Height = 30, VerticalAlignment = VerticalAlignment.Center });
        var title = new TextBlock { Text = "Apex Launcher", FontSize = 16, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(title, 1);
        g.Children.Add(title);
        var dots = StepDots(steps, current);
        dots.HorizontalAlignment = HorizontalAlignment.Right;
        Grid.SetColumn(dots, 2);
        g.Children.Add(dots);
        return g;
    }

    // the step's name and a piece of a bar for each step, filled up to it
    static StackPanel StepDots(string[] steps, int current)
    {
        var name = current >= 0 && current < steps.Length ? steps[current] : "";
        var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X1, VerticalAlignment = VerticalAlignment.Center };
        for (int i = 0; i < steps.Length; i++)
            bar.Children.Add(new Border { Width = 24, Height = 4, CornerRadius = new CornerRadius(2), Background = i == current ? Design.DesignSystem.MainBrush : Res(i < current ? "ApexAccentLineBrush" : "ApexInputBorderBrush") });
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X3, VerticalAlignment = VerticalAlignment.Center };
        row.Children.Add(new TextBlock { Text = name, FontSize = 13, Foreground = Res("ApexSoftTextBrush"), VerticalAlignment = VerticalAlignment.Center });
        row.Children.Add(bar);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(row, $"{name}, {current + 1}/{steps.Length}");
        return row;
    }

    // the footer: the buttons on the right
    static FrameworkElement UpgradeFooter(params Button[] buttons)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right };
        foreach (var b in buttons) { b.Height = 40; b.FontWeight = b.Style == (Style)Application.Current.Resources["AccentButtonStyle"] ? FontWeights.SemiBold : FontWeights.Normal; b.MinWidth = b.Style == (Style)Application.Current.Resources["AccentButtonStyle"] ? 140 : 96; row.Children.Add(b); }
        return new Border { Height = 72, Padding = new Thickness(32, 0, 32, 0), BorderThickness = new Thickness(0, 1, 0, 0), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexFooterBrush"), Child = row };
    }

    static Image Logo(double size) => new() { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };

    // ---------- 1 What's new (an old Setup that updated itself) ----------

    // a card that opens or ends something: the logo in a soft light of the theme on the left, the words and the button on the right
    (Grid Card, StackPanel Words) SplitCard(bool done = false)
    {
        var g = new Grid { MinHeight = 440 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(3, GridUnitType.Star), MaxWidth = 300 });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(5, GridUnitType.Star) });
        var side = SplitSide();
        if (done)
            side.Children.Add(new Border { Width = 40, Height = 40, CornerRadius = new CornerRadius(20), Background = Res("ApexCardBrush"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(80, 80, 0, 0), Child = Lucide.Icon("check", 20, Res("ApexSuccessBrush"), 2.5) });
        g.Children.Add(side);
        var words = new StackPanel { Padding = new Thickness(40), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(words, 1);
        g.Children.Add(words);
        return (g, words);
    }
    static Grid SplitSide()
    {
        var light = new SoftGlow(Tone("Accent"), App.Light ? 0.16 : 0.20, 0.5) { Width = 420, Height = 420, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        return new Grid { CornerRadius = new CornerRadius(Design.Radius.Panel - 1, 0, 0, Design.Radius.Panel - 1), Background = Res("ApexFooterBrush"), BorderThickness = new Thickness(0, 0, 1, 0), BorderBrush = Res("ApexCardBorderBrush"),
            Children = { light, Logo(120) } };
    }
    static TextBlock SplitTitle(string t) => new() { Text = t.Replace("Apex Launcher", "Apex\u00A0Launcher"), FontSize = 28, LineHeight = 36, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
    static TextBlock SplitText(string t) => new() { Text = t, FontSize = 14, LineHeight = 22, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0) };
    static Grid SplitPoint(string icon, string title, string text)
    {
        var g = new Grid { ColumnSpacing = Design.Space.IconToText };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.Children.Add(Design.IconTile.Of(icon));
        var w = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        w.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
        w.Children.Add(new TextBlock { Text = text, FontSize = 13, LineHeight = 18, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(w, 1);
        g.Children.Add(w);
        return g;
    }
    static Button SplitButton(Button b)
    {
        (b.MinHeight, b.MinWidth, b.HorizontalAlignment, b.FontWeight, b.Margin) = (Design.Size.ControlLarge, 160, HorizontalAlignment.Left, FontWeights.SemiBold, new Thickness(0, 32, 0, 0));
        return b;
    }

    void FillUpgradeNews(StackPanel card)
    {
        var (split, words) = SplitCard();
        words.Children.Add(SplitTitle(T("Apex Radiance Setup is now Apex Launcher")));
        words.Children.Add(SplitText(T("It still installs and updates Apex Radiance, and now it does more.")));
        words.Children.Add(new StackPanel
        {
            Spacing = Design.Space.X5, Margin = new Thickness(0, 28, 0, 0),
            Children =
            {
                SplitPoint("play", T("Starts your game"), T("Straight into the save you choose")),
                SplitPoint("puzzle", T("Keeps your things in one place"), T("Mods, saves and pictures")),
                SplitPoint("archive", T("Everything came with it"), T("Your games, components, backups and language")),
            },
        });
        words.Children.Add(SplitButton(Button(T("Continue"), () => UpgradeShow(1), accent: true)));
        card.Children.Add(split);
    }

    // ---------- 2 Where it lives ----------

    void FillUpgradeWhere(StackPanel card, string[] steps)
    {
        var self = Environment.ProcessPath ?? "";
        card.Children.Add(UpgradeHeader(steps, Array.IndexOf(steps, T("Where it lives"))));
        var body = new StackPanel { Padding = new Thickness(32, 28, 32, 32), Spacing = 12 };
        body.Children.Add(new TextBlock { Text = T("Where should it live?"), FontSize = 28, FontWeight = FontWeights.SemiBold, OpticalMarginAlignment = OpticalMarginAlignment.None });
        body.Children.Add(new TextBlock { Text = T("Pick once; Settings can change it later."), FontSize = 14, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, -8, 0, 12) });
        var cards = new List<(Border Card, Border Check, bool Value)>();
        void Pick(bool v)
        {
            _upgradeInstall = v;
            foreach (var c in cards)
            {
                OptionLit(c.Card, c.Value == v);
                OptionOn(c.Check, c.Value == v);
            }
        }
        Button Option(bool value, string icon, string title, string text)
        {
            var g = new Grid { ColumnSpacing = 16 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(Design.IconTile.Of(icon));
            var s = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            s.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold });
            s.Children.Add(new TextBlock { Text = text, FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            Grid.SetColumn(s, 1);
            g.Children.Add(s);
            var check = OptionDot();
            check.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(check, 2);
            g.Children.Add(check);
            var c = new Border { Child = g, Padding = new Thickness(Design.Space.CardPad), CornerRadius = new CornerRadius(Design.Radius.Card), BorderThickness = new Thickness(1) };
            var b = new Button { Content = c, Padding = new Thickness(0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(12), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            b.Click += (_, _) => Pick(value);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, title);
            cards.Add((c, check, value));
            return b;
        }
        body.Children.Add(Option(true, "hard-drive", T("Install it"), T("Recommended. In the Start menu and on the desktop")));
        body.Children.Add(Option(false, "package", T("Keep it portable"), T("One file you can move anywhere")));
        Pick(_upgradeInstall);
        card.Children.Add(body);
        var buttons = new List<Button>();
        if (Array.IndexOf(steps, T("Where it lives")) > 0) buttons.Add(Button(T("Back"), () => UpgradeShow(0)));
        else buttons.Add(Button(T("Ask me later"), UpgradeClose));
        buttons.Add(Button(T("Continue"), async () => await UpgradeApply(self), accent: true));
        card.Children.Add(UpgradeFooter([.. buttons]));
    }

    async Task UpgradeApply(string self)
    {
        // (a dry run shows the way through and changes nothing)
        if (DryRun) { if (_upgradeInstall) { _upgradeFrom = self; UpgradeShow(UpgradeSteps.Length - 1); } else UpgradeClose(); return; }
        if (!_upgradeInstall)
        {
            try { LauncherInstall.MakePortable(self, null); UpgradeClose(); BuildSettingsIfShown(); }
            catch (Exception e) { App.Log(e); await Message(T("Apex Launcher could not switch to portable"), ErrorText(e) + "\n\n" + T("It keeps running from here.")); }
            return;
        }
        try
        {
            // the installed copy opens on Ready and can delete this one once it has closed
            var args = Environment.GetCommandLineArgs().Skip(1).Where(a => !a.StartsWith(MovedFromArg, StringComparison.Ordinal) && a != "--was-setup").Append(MovedFromArg + self);
            if (Paths.MovedFromSetup) args = args.Append("--was-setup");
            LauncherInstall.Install(self, args);
            Application.Current.Exit();
        }
        catch (Exception e)
        {
            App.Log(e);
            await Message(T("Apex Launcher could not install itself"), ErrorText(e) + "\n\n" + T("It keeps running from here."));
        }
    }

    // ---------- 3 Ready (the installed copy) ----------

    void FillUpgradeReady(StackPanel card, string[] steps)
    {
        var (split, words) = SplitCard(done: true);
        words.Children.Add(SplitTitle(T("Apex Launcher is installed")));
        words.Children.Add(SplitText(T("Open it from the Start menu or the desktop. Your game is ready to play.")));
        // the copy it came from (Downloads, usually): deleted on Open, ticked by default
        // (only a launcher exe is offered for deleting, whatever the command line names)
        var old = _upgradeFrom is { } f && File.Exists(f) && !LauncherInstall.IsInstalledCopy(f) && f.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            && SelfUpdate.Products.Contains(System.Diagnostics.FileVersionInfo.GetVersionInfo(f).ProductName) ? f : null;
        if (old is not null)
        {
            var box = new CheckBox { IsChecked = _upgradeDelete, MinWidth = 0, Margin = new Thickness(0, 24, 0, 0), Content = T("Delete the old copy in {0}", Path.GetFileName(Path.GetDirectoryName(old) ?? "")) };
            ToolTipService.SetToolTip(box, old);
            box.Checked += (_, _) => _upgradeDelete = true;
            box.Unchecked += (_, _) => _upgradeDelete = false;
            words.Children.Add(box);
        }
        words.Children.Add(SplitButton(Button(T("Open Apex Launcher"), async () =>
        {
            if (old is not null && _upgradeDelete && !DryRun) await DeleteOldCopy(old);
            UpgradeClose();
        }, accent: true)));
        card.Children.Add(split);
    }

    // (the old copy closes right after it starts this one: a few tries while Windows lets go of it)
    static async Task DeleteOldCopy(string file)
    {
        for (int i = 0; i < 20; i++)
        {
            try { if (File.Exists(file)) File.Delete(file); return; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { await Task.Delay(250); }
        }
        App.Log(new IOException("The old launcher copy could not be deleted: " + file));
    }
}
