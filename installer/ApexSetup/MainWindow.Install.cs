using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// Installed or portable: asked once on a copy that is neither (a new download, or an old Setup that updated itself into
// the launcher), and changeable in Settings. LauncherInstall does the work; the window closes when another copy takes over.
public sealed partial class MainWindow
{
    async Task AskInstallModeAsync()
    {
        if (Maintenance || FirstInstall || LauncherInstall.Mode != LauncherMode.NotChosen || Environment.ProcessPath is not { } self) return;
        var install = true;
        var cards = new List<(Button Button, Border Card, Border Check, bool Value)>();
        void Pick(bool v)
        {
            install = v;
            foreach (var c in cards)
            {
                var on = c.Value == v;
                c.Card.BorderBrush = Res(on ? "ApexAccentBrush" : "ApexCardBorderBrush");
                c.Card.Background = on ? Res("ApexAccentSoftBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent);
                c.Check.Background = on ? Res("ApexAccentBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent);
                c.Check.BorderBrush = Res(on ? "ApexAccentBrush" : "ApexMutedBrush");
                c.Check.Child.Opacity = on ? 1 : 0;
            }
        }
        // one choice = one card: the whole card is the target, the chosen one in the accent colour with a check
        Button Option(bool value, string icon, string title, string text)
        {
            var g = new Grid { ColumnSpacing = 14 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(new Border { Width = 36, Height = 36, CornerRadius = new CornerRadius(8), Background = Res("ApexPillBrush"), VerticalAlignment = VerticalAlignment.Top,
                Child = Lucide.Icon(icon, 18, Res("ApexAccentBrush")) });
            var s = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            s.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold });
            s.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            Grid.SetColumn(s, 1);
            g.Children.Add(s);
            var check = new Border { Width = 20, Height = 20, CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1.5), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 8, 0, 0),
                Child = Lucide.Icon("check", 12, new SolidColorBrush(Microsoft.UI.Colors.White), 3) };
            Grid.SetColumn(check, 2);
            g.Children.Add(check);
            var card = new Border { Child = g, Padding = new Thickness(14), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1) };
            var b = new Button { Content = card, Padding = new Thickness(0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            b.Click += (_, _) => Pick(value);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, title);
            cards.Add((b, card, check, value));
            return b;
        }
        var body = new StackPanel { Spacing = 8, Width = 460 };
        body.Children.Add(new TextBlock { Text = T("Install it for this Windows account, or keep it as a single file you can run from any folder. Your settings and games are kept either way."), TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush"), Margin = new Thickness(0, 0, 0, 8) });
        body.Children.Add(Option(true, "hard-drive", T("Install (recommended)"), T("Adds Apex Launcher to the Start menu and Installed apps, easy to remove. Updates itself in place.")));
        body.Children.Add(Option(false, "package", T("Keep portable"), T("Runs from {0}, nothing added to Windows. Updates itself in place.", ShortPath(Path.GetDirectoryName(self) ?? self))));
        Pick(true);
        var d = new ContentDialog
        {
            Title = T("How do you want to use Apex Launcher?"), Content = body, PrimaryButtonText = T("Continue"), CloseButtonText = T("Ask me later"),
            DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
        };
        if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        await ApplyInstallMode(install, self, null);
    }

    // install (this copy starts the installed one and closes) or make it portable (where it is, or in a folder chosen)
    async Task ApplyInstallMode(bool install, string self, string? folder)
    {
        if (DryRun) return; // (a dry run shows the question, changes nothing)
        try
        {
            if (install)
            {
                LauncherInstall.Install(self, Environment.GetCommandLineArgs().Skip(1));
                Application.Current.Exit();
                return;
            }
            var to = LauncherInstall.MakePortable(self, folder);
            if (!to.Equals(self, StringComparison.OrdinalIgnoreCase)) { Application.Current.Exit(); return; }
            BuildSettingsIfShown();
        }
        catch (Exception e)
        {
            App.Log(e);
            await Message(install ? T("Apex Launcher could not install itself") : T("Apex Launcher could not become portable"),
                ErrorText(e) + "\n\n" + T("It keeps running from here."));
        }
    }

    // a long folder as its drive and last two folders ("C:\…\Downloads\Apex"), so it fits one line
    static string ShortPath(string p)
    {
        var parts = p.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries);
        return parts.Length <= 3 ? p : $"{parts[0]}{Path.DirectorySeparatorChar}\u2026{Path.DirectorySeparatorChar}{parts[^2]}{Path.DirectorySeparatorChar}{parts[^1]}";
    }

    void BuildSettingsIfShown() { if (_step == SettingsScreen) BuildSettings(); }

    // the end of an install on a copy that is neither installed nor portable: the same two choices as cards, Install first
    bool _doneInstall = true;
    void FillDoneChoice()
    {
        DoneChoice.Children.Clear();
        var ask = !Maintenance && !DryRun && LauncherInstall.Mode == LauncherMode.NotChosen && Environment.ProcessPath is not null && _lastInstall.Count > 0;
        DoneChoice.Visibility = ask ? Visibility.Visible : Visibility.Collapsed;
        if (!ask) return;
        var self = Environment.ProcessPath!;
        foreach (var (value, icon, title, text) in new[]
        {
            (true, "hard-drive", T("Keep Apex Launcher on this PC"), T("In the Start menu, on the desktop and in Installed apps. Start the game from it.")),
            (false, "folder", T("Keep it as a single file"), T("Runs from {0}, nothing added to Windows.", ShortPath(Path.GetDirectoryName(self) ?? self))),
        })
        {
            var on = value == _doneInstall;
            var g = new Grid { ColumnSpacing = 14 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(new Border { Width = 36, Height = 36, CornerRadius = new CornerRadius(10), Background = Res("ApexPillBrush"), VerticalAlignment = VerticalAlignment.Center,
                Child = Lucide.Icon(icon, 17, Res(value ? "ApexAccentBrush" : "ApexMutedBrush")) });
            var s = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            s.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold });
            s.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            Grid.SetColumn(s, 1);
            g.Children.Add(s);
            var check = new Border { Width = 20, Height = 20, CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1.5), VerticalAlignment = VerticalAlignment.Center,
                Background = on ? Res("ApexAccentBrush") : null, BorderBrush = Res(on ? "ApexAccentBrush" : "ApexMutedBrush"),
                Child = on ? Lucide.Icon("check", 12, Res("TextOnAccentFillColorPrimaryBrush"), 3) : null };
            Grid.SetColumn(check, 2);
            g.Children.Add(check);
            var card = new Border { Child = g, Padding = new Thickness(16, 14, 16, 14), CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(on ? 1.5 : 1),
                BorderBrush = Res(on ? "ApexAccentBrush" : "ApexCardBorderBrush"), Background = on ? Res("ApexAccentSoftBrush") : null };
            var b = new Button { Content = card, Padding = new Thickness(0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(14), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            b.Click += (_, _) => { _doneInstall = value; FillDoneChoice(); UpdateFooter(); };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, title);
            DoneChoice.Children.Add(b);
        }
    }

    // Settings: this copy, installed or not, and the way to switch
    Border InstallModeRow()
    {
        var self = Environment.ProcessPath ?? "";
        var mode = LauncherInstall.Mode;
        var (text, action) = mode switch
        {
            LauncherMode.Installed => (T("Installed for this Windows account."), Button(T("Use without installing…"), async () =>
            {
                var picker = new Microsoft.Windows.Storage.Pickers.FolderPicker(AppWindow.Id);
                if (await picker.PickSingleFolderAsync() is not { } f) return;
                if (await Confirm(T("Use without installing?"), T("Apex Launcher moves to {0} and leaves the Start menu, the desktop and Installed apps. Your settings stay.", f.Path), T("Move it")))
                    await ApplyInstallMode(false, self, f.Path);
            })),
            LauncherMode.Portable => (T("Not installed: running from {0}.", ShortPath(Path.GetDirectoryName(self) ?? self)), Button(T("Install it"), () => _ = ApplyInstallMode(true, self, null))),
            _ => (T("Running from {0}, not installed yet.", ShortPath(Path.GetDirectoryName(self) ?? self)), Button(T("Choose…"), () => _ = AskInstallModeAsync())),
        };
        return Row(T("This copy of the launcher"), text, DryRun ? null : action);
    }
}
