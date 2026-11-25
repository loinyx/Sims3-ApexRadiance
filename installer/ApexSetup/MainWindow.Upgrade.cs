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
    bool _upgradeDelete = true;
    string? _upgradeFrom; // (Ready: the copy this one was installed from)

    string[] UpgradeSteps => Paths.MovedFromSetup || _upgradeWasSetup ? [T("What's new"), T("Where to keep it"), T("Ready")] : [T("Where to keep it"), T("Ready")];
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
        var card = new Grid();
        if (name == T("What's new")) FillUpgradeNews(card);
        else if (name == T("Where to keep it")) FillUpgradeWhere(card, steps);
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

    static Image Logo(double size) => new() { Source = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri("ms-appx:///Assets/apex_logo.png")), Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };

    // ---------- 1 What's new (an old Setup that updated itself) ----------

    // a card that opens or ends something: the logo in a soft light of the theme on the left, the words and the button on the right
    (Grid Card, StackPanel Words) SplitCard(bool done = false)
    {
        var g = new Grid { MinHeight = 440 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(Design.Size.InstallSide) });
        g.ColumnDefinitions.Add(new ColumnDefinition());
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
    // the install's side: the logo in its light, smaller, over the steps, what is done checked and the one open lit
    void FillWizardSide(int current)
    {
        string[] steps = [T("Game"), T("Components"), T("Game settings"), T("Install")];
        var key = $"{current}|{Lang}|{App.Light}|{Design.DesignSystem.Theme.Id}|{FirstInstall}";
        if (WizardSide.Children.Count > 0 && (string?)WizardSide.DataContext == key) return;
        WizardSide.DataContext = key;
        WizardSide.Children.Clear();
        var list = new StackPanel { Spacing = Design.Space.X1, Margin = new Thickness(0, Design.Space.X8, 0, 0) };
        for (int i = 0; i < steps.Length && FirstInstall; i++)
        {
            var mark = new Border { Width = 24, Height = 24, CornerRadius = new CornerRadius(12), VerticalAlignment = VerticalAlignment.Center };
            (mark.Background, mark.Child) = i == current ? (Design.DesignSystem.MainBrush, (UIElement)Number(i, "ApexInkBrush"))
                : i < current ? (Res("ApexAccentSoftBrush"), Centred(Lucide.Icon("check", 14, Res("ApexAccentBrush"), 2.5))) : (Res("ApexTileBrush"), Number(i, "ApexMutedBrush"));
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X3, Padding = new Thickness(Design.Space.X3, 0, Design.Space.X3, 0), Height = 44 };
            row.Children.Add(mark);
            row.Children.Add(new TextBlock { Text = steps[i], FontSize = 14, FontWeight = i == current ? FontWeights.SemiBold : FontWeights.Normal, Foreground = Res(i == current ? "TextFillColorPrimaryBrush" : i < current ? "ApexSoftTextBrush" : "ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
            list.Children.Add(new Border { Child = row, CornerRadius = new CornerRadius(Design.Radius.Control), Background = i == current ? Res("ApexTileBrush") : null });
        }
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(list, $"{steps[Math.Clamp(current, 0, steps.Length - 1)]}, {current + 1}/{steps.Length}");
        var glow = new SoftGlow(Tone("Accent"), App.Light ? 0.16 : 0.20, 0.5) { Width = 320, Height = 320, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, -72, 0, 0) };
        var words = new StackPanel { Padding = new Thickness(Design.Space.X6, Design.Space.X10, Design.Space.X6, Design.Space.X6) };
        words.Children.Add(Logo(72));
        words.Children.Add(new TextBlock { Text = "Apex Radiance", FontSize = 20, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, Design.Space.X4, 0, 0) });
        words.Children.Add(list);
        WizardSide.Children.Add(new Grid { CornerRadius = new CornerRadius(Design.Radius.Panel - 1, 0, 0, Design.Radius.Panel - 1), Background = Res("ApexFooterBrush"), BorderThickness = new Thickness(0, 0, 1, 0), BorderBrush = Res("ApexCardBorderBrush"), Children = { glow, words } });

        static TextBlock Number(int i, string ink) => new() { Text = (i + 1).ToString(), FontSize = 12, FontWeight = FontWeights.SemiBold, Foreground = Res(ink), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        static UIElement Centred(FrameworkElement e) { (e.HorizontalAlignment, e.VerticalAlignment) = (HorizontalAlignment.Center, VerticalAlignment.Center); return e; }
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

    void FillUpgradeNews(Panel card)
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

    bool _upgradeStartup;
    // installing is the way on; running without it is a line under the buttons
    void FillUpgradeWhere(Panel card, string[] steps)
    {
        var self = Environment.ProcessPath ?? "";
        var (split, words) = SplitCard();
        words.Children.Add(SplitTitle(T("Install Apex Launcher")));
        words.Children.Add(SplitText(T("It opens from the Start menu and keeps itself up to date.")));
        words.Children.Add(new StackPanel
        {
            Margin = new Thickness(0, Design.Space.X6, 0, 0),
            Children =
            {
                FolderRow(),
                Toggle("monitor", T("Desktop shortcut"), "", _settings.DesktopShortcut, on => { _settings.DesktopShortcut = on; if (!DryRun) _settings.Save(); }),
                Toggle("power", T("Start with Windows"), T("Opens quietly in the notification area"), _upgradeStartup, on => _upgradeStartup = on),
            },
        });
        var install = SplitButton(Button(T("Install"), () => _ = UpgradeApply(self, true), accent: true));
        var other = SplitButton(Array.IndexOf(steps, T("Where to keep it")) > 0 ? Button(T("Back"), () => UpgradeShow(0)) : Button(T("Ask me later"), UpgradeClose));
        (other.MinWidth, other.FontWeight, install.Margin, other.Margin) = (104, FontWeights.Normal, new Thickness(0), new Thickness(0));
        words.Children.Add(new StackPanel { Orientation = Orientation.Horizontal, Spacing = Design.Space.X2, Margin = new Thickness(0, Design.Space.X6, 0, 0), Children = { install, other } });
        var portable = new HyperlinkButton { Content = T("Use without installing"), FontSize = 13, Padding = new Thickness(0) };
        portable.Click += (_, _) => _ = UpgradeApply(self, false);
        words.Children.Add(new WrapPanel
        {
            Spacing = 6, Margin = new Thickness(0, Design.Space.X4, 0, 0),
            Children = { new TextBlock { Text = T("Prefer a single file you can move anywhere?"), FontSize = 13, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center }, portable },
        });
        card.Children.Add(split);
    }

    async Task UpgradeApply(string self, bool install)
    {
        // (a dry run shows the way through and changes nothing)
        if (DryRun) { if (install) { _upgradeFrom = self; UpgradeShow(UpgradeSteps.Length - 1); } else UpgradeClose(); return; }
        if (!install)
        {
            try { LauncherInstall.MakePortable(self, null); UpgradeClose(); BuildSettingsIfShown(); }
            catch (Exception e) { App.Log(e); await Message(T("Apex Launcher could not switch to portable"), ErrorText(e) + "\n\n" + T("It keeps running from where it is.")); }
            return;
        }
        try
        {
            // the installed copy opens on Ready and can delete this one once it has closed
            var args = Environment.GetCommandLineArgs().Skip(1).Where(a => !a.StartsWith(MovedFromArg, StringComparison.Ordinal) && a != "--was-setup").Append(MovedFromArg + self);
            if (Paths.MovedFromSetup) args = args.Append("--was-setup");
            if (_busy || _savesBusy) { SayBusy(); return; }
            LauncherInstall.Install(self, args, folder: InstallTo);
            if (_upgradeStartup) LauncherInstall.SetRunAtStartup(Paths.InstalledExe, true);
            Leave();
        }
        catch (Exception e)
        {
            App.Log(e);
            await Message(T("Apex Launcher could not install itself"), ErrorText(e) + "\n\n" + T("It keeps running from where it is."));
        }
    }

    // ---------- 3 Ready (the installed copy) ----------

    void FillUpgradeReady(Panel card, string[] steps)
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
