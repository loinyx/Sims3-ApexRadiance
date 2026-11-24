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
    bool _upgradeWasSetup;

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

    // the header: the launcher, its version and the steps, the one shown in the accent colour and the ones done ticked
    FrameworkElement UpgradeHeader(string[] steps, int current)
    {
        var g = new Grid { Padding = new Thickness(28, 18, 28, 18), ColumnSpacing = 12, BorderThickness = new Thickness(0, 0, 0, 1), BorderBrush = Res("ApexHairlineBrush") };
        for (int i = 0; i < 4; i++) g.ColumnDefinitions.Add(new ColumnDefinition { Width = i == 3 ? new GridLength(1, GridUnitType.Star) : GridLength.Auto });
        g.Children.Add(new Image { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = 30, Height = 30, VerticalAlignment = VerticalAlignment.Center });
        var title = new TextBlock { Text = "Apex Launcher", FontSize = 15, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(title, 1);
        g.Children.Add(title);
        var pill = new Border { CornerRadius = new CornerRadius(10), Padding = new Thickness(9, 2, 9, 3), VerticalAlignment = VerticalAlignment.Center, BorderThickness = new Thickness(1),
            Background = Res("ApexAccentSoftBrush"), BorderBrush = Res("ApexAccentLineBrush"),
            Child = new TextBlock { Text = System.Reflection.Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "", FontSize = 12, Foreground = Res("ApexAccentTextBrush") } };
        Grid.SetColumn(pill, 2);
        g.Children.Add(pill);
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
        for (int i = 0; i < steps.Length; i++)
        {
            if (i > 0) row.Children.Add(new Border { Width = 40, Height = 1, VerticalAlignment = VerticalAlignment.Center, Background = Res("ApexCardBorderBrush") });
            var done = i < current;
            var on = i == current;
            row.Children.Add(new Border
            {
                Width = 22, Height = 22, CornerRadius = new CornerRadius(11), VerticalAlignment = VerticalAlignment.Center,
                Background = on ? Res("ApexAccentBrush") : done ? Res("ApexSuccessSoftBrush") : null,
                BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(on || done ? 0 : 1),
                Child = done ? new FontIcon { Glyph = "", FontSize = 11, Foreground = Res("ApexSuccessBrush") }
                    : new TextBlock { Text = (i + 1).ToString(), FontSize = 11, FontWeight = FontWeights.Bold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
                                      Foreground = on ? Res("TextOnAccentFillColorPrimaryBrush") : Res("ApexMutedBrush") },
            });
            row.Children.Add(new TextBlock { Text = steps[i], FontSize = 13, VerticalAlignment = VerticalAlignment.Center, FontWeight = on ? FontWeights.SemiBold : FontWeights.Normal,
                Foreground = Res(done ? "ApexSuccessBrush" : on ? "ApexSoftTextBrush" : "ApexMutedBrush") });
        }
        Grid.SetColumn(row, 3);
        g.Children.Add(row);
        return g;
    }

    // the footer: a note on the left, the buttons on the right
    static FrameworkElement UpgradeFooter(FrameworkElement note, params Button[] buttons)
    {
        var g = new Grid { Padding = new Thickness(40, 16, 40, 16), ColumnSpacing = 12, BorderThickness = new Thickness(0, 1, 0, 0), BorderBrush = Res("ApexHairlineBrush"), Background = Res("ApexFooterBrush") };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        note.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(note);
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var b in buttons) { b.Height = 46; b.FontSize = 15; b.MinWidth = b.Style == (Style)Application.Current.Resources["AccentButtonStyle"] ? 170 : 110; row.Children.Add(b); }
        Grid.SetColumn(row, 1);
        g.Children.Add(row);
        return g;
    }

    static FrameworkElement Note(string text, string? icon = null)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (icon is not null) { var i = Lucide.Icon(icon, 15, Res("ApexSuccessBrush")); i.VerticalAlignment = VerticalAlignment.Center; s.Children.Add(i); }
        s.Children.Add(new TextBlock { Text = text, FontSize = 13, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center });
        return s;
    }

    static FrameworkElement CheckLine(string text, string? detail = null)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        var i = Lucide.Icon("check", 15, Res("ApexAccentBrush"), 2.6);
        i.VerticalAlignment = VerticalAlignment.Center;
        s.Children.Add(i);
        var t = new TextBlock { FontSize = 14, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
        t.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = text });
        if (detail is not null) t.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = " · " + detail, Foreground = Res("ApexMutedBrush") });
        s.Children.Add(t);
        return s;
    }

    static Image Logo(double size) => new() { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center };

    // ---------- 1 What's new (an old Setup that updated itself) ----------

    void FillUpgradeNews(StackPanel card)
    {
        var top = new StackPanel { Padding = new Thickness(40, 34, 40, 6), Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center };
        top.Children.Add(Logo(96));
        top.Children.Add(new TextBlock { Text = T("Apex Radiance Setup is now Apex Launcher"), FontSize = 30, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center });
        top.Children.Add(new TextBlock { Text = T("It still installs and updates Apex Radiance. Now it also starts the game, checks it, and keeps your mods, saves and pictures in one place."),
            FontSize = 14, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center, MaxWidth = 560, HorizontalAlignment = HorizontalAlignment.Center });
        card.Children.Add(top);
        // what came with it: the game, Apex Radiance as it is, the restore points
        var came = new StackPanel();
        var head = new Grid { ColumnSpacing = 14, Padding = new Thickness(16, 13, 16, 13), BorderThickness = new Thickness(0, 0, 0, 1), BorderBrush = Res("ApexHairlineBrush") };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition());
        head.Children.Add(new Border { Width = 36, Height = 36, CornerRadius = new CornerRadius(10), Background = Res("ApexPillBrush"), Child = Lucide.Icon("sparkles", 18, Res("ApexAccentBrush")) });
        var ht = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        ht.Children.Add(new TextBlock { Text = T("What came with it"), FontSize = 14, FontWeight = FontWeights.SemiBold });
        ht.Children.Add(new TextBlock { Text = T("Nothing to set up again."), FontSize = 13, Foreground = Res("ApexMutedBrush") });
        Grid.SetColumn(ht, 1);
        head.Children.Add(ht);
        came.Children.Add(head);
        var lines = new StackPanel { Spacing = 10, Padding = new Thickness(18, 14, 18, 16) };
        lines.Children.Add(_game is { } g ? CheckLine(T("Your game"), "The Sims 3, " + g.Label + (g.Version.Length > 0 ? " " + g.Version : "")) : CheckLine(T("Your games and language")));
        lines.Children.Add(_state?.ApexVersion is { } av ? CheckLine(T("Apex Radiance {0} and its components", av), T("as they are")) : CheckLine(T("Apex Radiance and its components"), T("as they are")));
        lines.Children.Add(CheckLine(T("Restore points, backups and your language")));
        came.Children.Add(lines);
        card.Children.Add(new Border { Margin = new Thickness(40, 18, 40, 22), CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"),
            Background = Res("ApexHairlineBrush"), Child = came });
        var next = Button(T("Continue"), () => UpgradeShow(1), accent: true);
        card.Children.Add(UpgradeFooter(Note(T("Nothing in your game changes in this step."), "shield-check"), next));
    }

    // ---------- 2 Where it lives ----------

    void FillUpgradeWhere(StackPanel card, string[] steps)
    {
        var self = Environment.ProcessPath ?? "";
        card.Children.Add(UpgradeHeader(steps, Array.IndexOf(steps, T("Where it lives"))));
        var body = new StackPanel { Padding = new Thickness(32, 26, 32, 28), Spacing = 12 };
        body.Children.Add(new TextBlock { Text = T("Where should it live?"), FontSize = 28, FontWeight = FontWeights.SemiBold });
        body.Children.Add(new TextBlock { Text = T("Right now it runs from {0}. Pick once; Settings can change it later.", Path.GetFileName(Path.GetDirectoryName(self) ?? "") is { Length: > 0 } d ? d : self),
            FontSize = 14, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 6) });
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
        Button Option(bool value, string icon, string title, string? badge, string text, string where)
        {
            var g = new Grid { ColumnSpacing = 16 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(new Border { Width = 40, Height = 40, CornerRadius = new CornerRadius(10), Background = Res("ApexPillBrush"), VerticalAlignment = VerticalAlignment.Top,
                Child = Lucide.Icon(icon, 20, Res(value ? "ApexAccentBrush" : "ApexSoftTextBrush")) });
            var s = new StackPanel { Spacing = 4 };
            var t = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
            t.Children.Add(new TextBlock { Text = title, FontSize = 15, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            if (badge is not null) t.Children.Add(Chip(badge, "ApexSuccessBrush"));
            s.Children.Add(t);
            s.Children.Add(new TextBlock { Text = text, FontSize = 13, Foreground = Res("ApexSoftTextBrush"), TextWrapping = TextWrapping.Wrap });
            s.Children.Add(new TextBlock { Text = where, FontSize = 12, FontFamily = new FontFamily("Cascadia Mono, Consolas"), Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            Grid.SetColumn(s, 1);
            g.Children.Add(s);
            var check = new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(11), BorderThickness = new Thickness(1.5), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 2, 0, 0),
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
        body.Children.Add(Option(true, "hard-drive", T("Install it"), T("Recommended"), T("In the Start menu, on the desktop and in Installed apps, where you can remove it. Updates itself."), Paths.InstallDir));
        body.Children.Add(Option(false, "package", T("Keep it portable"), null, T("Stays as one file you can move, nothing added to Windows. Updates itself too."), Path.GetDirectoryName(self) ?? self));
        Pick(_upgradeInstall);
        card.Children.Add(body);
        var buttons = new List<Button>();
        if (Array.IndexOf(steps, T("Where it lives")) > 0) buttons.Add(Button(T("Back"), () => UpgradeShow(0)));
        else buttons.Add(Button(T("Ask me later"), UpgradeClose));
        buttons.Add(Button(T("Continue"), async () => await UpgradeApply(self), accent: true));
        card.Children.Add(UpgradeFooter(Note(T("Takes a few seconds. Your game isn't touched.")), [.. buttons]));
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
        var top = new StackPanel { Padding = new Thickness(40, 30, 40, 6), Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center };
        // the logo with a tick on its corner
        var badge = new Grid { Width = 96, Height = 96, HorizontalAlignment = HorizontalAlignment.Center };
        badge.Children.Add(Logo(96));
        badge.Children.Add(new Border { Width = 34, Height = 34, CornerRadius = new CornerRadius(17), Background = Res("ApexAccentBrush"), BorderBrush = Res("ApexCardBrush"), BorderThickness = new Thickness(4),
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, -4, -4),
            Child = Lucide.Icon("check", 15, Res("TextOnAccentFillColorPrimaryBrush"), 3.2) });
        top.Children.Add(badge);
        top.Children.Add(new TextBlock { Text = T("Apex Launcher is installed"), FontSize = 30, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 6, 0, 0) });
        top.Children.Add(new TextBlock { Text = T("Open it from the Start menu or the desktop. Your game is ready to play."), FontSize = 14, Foreground = Res("ApexSoftTextBrush"),
            TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center });
        var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 4, 0, 0) };
        var places = new List<string> { T("Start menu") };
        if (LauncherSettings.Load().DesktopShortcut) places.Add(T("Desktop"));
        places.Add(T("Installed apps"));
        foreach (var p in places)
        {
            var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            s.Children.Add(Lucide.Icon("check", 13, Res("ApexAccentBrush"), 2.6));
            s.Children.Add(new TextBlock { Text = p, FontSize = 13 });
            chips.Children.Add(new Border { Child = s, Padding = new Thickness(12, 6, 12, 6), CornerRadius = new CornerRadius(15), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexHairlineBrush") });
        }
        top.Children.Add(chips);
        card.Children.Add(top);
        // the copy it came from (Downloads, usually): deleted on Open, ticked by default
        var old = _upgradeFrom is { } f && File.Exists(f) && !LauncherInstall.IsInstalledCopy(f) ? f : null;
        if (old is not null)
        {
            var box = new CheckBox { IsChecked = _upgradeDelete, MinWidth = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
            var label = new StackPanel { Spacing = 2 };
            label.Children.Add(new TextBlock { Text = T("Delete the old copy in {0}", Path.GetFileName(Path.GetDirectoryName(old) ?? "")), FontSize = 14 });
            label.Children.Add(new TextBlock { Text = old, FontSize = 12, FontFamily = new FontFamily("Cascadia Mono, Consolas"), Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            box.Content = label;
            box.Checked += (_, _) => _upgradeDelete = true;
            box.Unchecked += (_, _) => _upgradeDelete = false;
            card.Children.Add(new Border { Margin = new Thickness(40, 18, 40, 22), Padding = new Thickness(16, 12, 16, 12), CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1),
                BorderBrush = Res("ApexCardBorderBrush"), Background = Res("ApexHairlineBrush"), Child = box });
        }
        else top.Margin = new Thickness(0, 0, 0, 22);
        var open = Button(T("Open Apex Launcher"), async () =>
        {
            if (old is not null && _upgradeDelete && !DryRun) await DeleteOldCopy(old);
            UpgradeClose();
        }, accent: true);
        card.Children.Add(UpgradeFooter(Note(T("Everything else stays where it was.")), open));
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
