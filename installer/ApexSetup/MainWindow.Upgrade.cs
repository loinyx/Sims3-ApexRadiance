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
        var g = new Grid { Padding = new Thickness(32, 18, 32, 18), ColumnSpacing = 12 };
        for (int i = 0; i < 3; i++) g.ColumnDefinitions.Add(new ColumnDefinition { Width = i == 2 ? new GridLength(1, GridUnitType.Star) : GridLength.Auto });
        g.Children.Add(new Image { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = 30, Height = 30, VerticalAlignment = VerticalAlignment.Center });
        var title = new TextBlock { Text = "Apex Launcher", FontSize = 15, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(title, 1);
        g.Children.Add(title);
        var dots = StepDots(steps, current);
        dots.HorizontalAlignment = HorizontalAlignment.Right;
        Grid.SetColumn(dots, 2);
        g.Children.Add(dots);
        return g;
    }

    // a dot for each step, the current one long; the names are read by screen readers and shown on hover
    static StackPanel StepDots(string[] steps, int current)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        for (int i = 0; i < steps.Length; i++)
            row.Children.Add(new Border { Width = i == current ? 20 : 6, Height = 6, CornerRadius = new CornerRadius(3), Background = Res(i == current ? "ApexAccentBrush" : i < current ? "ApexAccentTextBrush" : "ApexCardBorderBrush"), Opacity = i < current ? 0.6 : 1 });
        var name = current >= 0 && current < steps.Length ? steps[current] : "";
        ToolTipService.SetToolTip(row, name);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(row, $"{name}, {current + 1}/{steps.Length}");
        return row;
    }

    // the footer: the buttons on the right, one alone in the middle
    static FrameworkElement UpgradeFooter(params Button[] buttons)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = buttons.Length == 1 ? HorizontalAlignment.Center : HorizontalAlignment.Right };
        foreach (var b in buttons) { b.Height = 40; b.FontWeight = b.Style == (Style)Application.Current.Resources["AccentButtonStyle"] ? FontWeights.SemiBold : FontWeights.Normal; b.MinWidth = b.Style == (Style)Application.Current.Resources["AccentButtonStyle"] ? 140 : 96; row.Children.Add(b); }
        return new Border { Padding = new Thickness(32, 0, 32, 24), Child = row };
    }

    static FrameworkElement CheckLine(string text)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        var i = Lucide.Icon("check", 15, Res("ApexAccentBrush"), 2.6);
        i.VerticalAlignment = VerticalAlignment.Center;
        s.Children.Add(i);
        s.Children.Add(new TextBlock { Text = text, FontSize = 14, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap });
        return s;
    }

    static Image Logo(double size) => new() { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center };

    // ---------- 1 What's new (an old Setup that updated itself) ----------

    void FillUpgradeNews(StackPanel card)
    {
        var top = new StackPanel { Padding = new Thickness(32, 34, 32, 6), Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center };
        top.Children.Add(Logo(96));
        top.Children.Add(new TextBlock { Text = T("Apex Radiance Setup is now Apex Launcher"), FontSize = 30, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center });
        top.Children.Add(new TextBlock { Text = T("It now also starts the game and keeps your mods, saves and pictures in one place. Everything came with it."),
            FontSize = 14, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center, MaxWidth = 560, HorizontalAlignment = HorizontalAlignment.Center });
        card.Children.Add(top);
        // what came with it
        var lines = new StackPanel { Spacing = 10, Margin = new Thickness(0, 14, 0, 28), HorizontalAlignment = HorizontalAlignment.Center };
        lines.Children.Add(CheckLine(T("Your games and language")));
        lines.Children.Add(CheckLine(T("Apex Radiance and its components")));
        lines.Children.Add(CheckLine(T("Restore points and backups")));
        card.Children.Add(lines);
        var next = Button(T("Continue"), () => UpgradeShow(1), accent: true);
        card.Children.Add(UpgradeFooter(next));
    }

    // ---------- 2 Where it lives ----------

    void FillUpgradeWhere(StackPanel card, string[] steps)
    {
        var self = Environment.ProcessPath ?? "";
        card.Children.Add(UpgradeHeader(steps, Array.IndexOf(steps, T("Where it lives"))));
        var body = new StackPanel { Padding = new Thickness(32, 20, 32, 32), Spacing = 12 };
        body.Children.Add(new TextBlock { Text = T("Where should it live?"), FontSize = 28, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 12) });
        var cards = new List<(Border Card, Border Check, bool Value)>();
        void Pick(bool v)
        {
            _upgradeInstall = v;
            foreach (var c in cards)
            {
                var on = c.Value == v;
                c.Card.BorderBrush = Res(on ? "ApexAccentBrush" : "ApexCardBorderBrush");
                c.Card.Background = on ? Res("ApexAccentSoftBrush") : Res("ApexHairlineBrush");
                c.Check.Background = on ? Res("ApexAccentBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent);
                c.Check.BorderBrush = Res(on ? "ApexAccentBrush" : "ApexMutedBrush");
                c.Check.Child.Opacity = on ? 1 : 0;
            }
        }
        Button Option(bool value, string icon, string title, string text)
        {
            var g = new Grid { ColumnSpacing = 16 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var mark = Lucide.Icon(icon, 20, Res("ApexSoftTextBrush"));
            mark.VerticalAlignment = VerticalAlignment.Center;
            g.Children.Add(mark);
            var s = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            s.Children.Add(new TextBlock { Text = title, FontSize = 15, FontWeight = FontWeights.SemiBold });
            s.Children.Add(new TextBlock { Text = text, FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            Grid.SetColumn(s, 1);
            g.Children.Add(s);
            var check = new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), BorderThickness = new Thickness(1.5), VerticalAlignment = VerticalAlignment.Center,
                Child = Lucide.Icon("check", 12, new SolidColorBrush(Microsoft.UI.Colors.Black), 3.2) };
            Grid.SetColumn(check, 2);
            g.Children.Add(check);
            var c = new Border { Child = g, Padding = new Thickness(18, 16, 18, 16), CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1) };
            var b = new Button { Content = c, Padding = new Thickness(0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(14), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            b.Resources["ButtonBackgroundPointerOver"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            b.Resources["ButtonBackgroundPressed"] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            b.Click += (_, _) => Pick(value);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, title);
            cards.Add((c, check, value));
            return b;
        }
        body.Children.Add(Option(true, "hard-drive", T("Install it"), T("In the Start menu and on the desktop")));
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
        card.Children.Add(UpgradeHeader(steps, steps.Length - 1));
        var top = new StackPanel { Padding = new Thickness(32, 30, 32, 6), Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center };
        // the logo with a tick on its corner
        var badge = new Grid { Width = 96, Height = 96, HorizontalAlignment = HorizontalAlignment.Center };
        badge.Children.Add(Logo(96));
        badge.Children.Add(new Border { Width = 34, Height = 34, CornerRadius = new CornerRadius(17), Background = Res("ApexAccentBrush"), BorderBrush = Res("ApexCardBrush"), BorderThickness = new Thickness(4),
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, -4, -4),
            Child = Lucide.Icon("check", 15, Res("TextOnAccentFillColorPrimaryBrush"), 3.2) });
        top.Children.Add(badge);
        top.Children.Add(new TextBlock { Text = T("Apex Launcher is installed"), FontSize = 30, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 6, 0, 0) });
        top.Children.Add(new TextBlock { Text = T("Your game is ready to play."), FontSize = 14, Foreground = Res("ApexSoftTextBrush"),
            TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center });
        card.Children.Add(top);
        // the copy it came from (Downloads, usually): deleted on Open, ticked by default
        // (only a launcher exe is offered for deleting, whatever the command line names)
        var old = _upgradeFrom is { } f && File.Exists(f) && !LauncherInstall.IsInstalledCopy(f) && f.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            && SelfUpdate.Products.Contains(System.Diagnostics.FileVersionInfo.GetVersionInfo(f).ProductName) ? f : null;
        if (old is not null)
        {
            var box = new CheckBox { IsChecked = _upgradeDelete, MinWidth = 0 };
            box.HorizontalAlignment = HorizontalAlignment.Center;
            box.Content = T("Delete the old copy in {0}", Path.GetFileName(Path.GetDirectoryName(old) ?? ""));
            ToolTipService.SetToolTip(box, old);
            box.Checked += (_, _) => _upgradeDelete = true;
            box.Unchecked += (_, _) => _upgradeDelete = false;
            box.Margin = new Thickness(32, 12, 32, 22);
            card.Children.Add(box);
        }
        else top.Margin = new Thickness(0, 0, 0, 22);
        var open = Button(T("Open Apex Launcher"), async () =>
        {
            if (old is not null && _upgradeDelete && !DryRun) await DeleteOldCopy(old);
            UpgradeClose();
        }, accent: true);
        card.Children.Add(UpgradeFooter(open));
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
