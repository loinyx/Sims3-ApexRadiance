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
            await Message(install ? T("Apex Launcher could not install itself") : T("Apex Launcher could not switch to portable"),
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
        // one list, as the install's own lists: keep it on this PC (recommended, in the launcher's own folder) or a single file
        var list = new StackPanel();
        foreach (var (value, icon, title, text) in new[]
        {
            (true, "hard-drive", T("Keep Apex Launcher on this PC"), T("In the Start menu, on the desktop and in Installed apps. Start the game from it.")),
            (false, "folder", T("Keep it as a single file"), T("Runs from {0}. Nothing is added to Windows.", ShortPath(Path.GetDirectoryName(self) ?? self))),
        })
        {
            var on = value == _doneInstall;
            var g = new Grid { ColumnSpacing = 14 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(new Border { Width = 36, Height = 36, CornerRadius = new CornerRadius(10), Background = Res(on ? "ApexAccentSoftBrush" : "ApexPillBrush"), VerticalAlignment = VerticalAlignment.Center,
                Child = Lucide.Icon(icon, 17, Res(on ? "ApexAccentBrush" : "ApexSoftTextBrush")) });
            var s = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
            var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            head.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            if (value)
                head.Children.Add(new Border { Style = (Style)Application.Current.Resources["ApexPill"], Background = Res("ApexAccentSoftBrush"), VerticalAlignment = VerticalAlignment.Center,
                    Child = new TextBlock { Text = T("Recommended"), FontSize = 11, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexAccentTextBrush") } });
            s.Children.Add(head);
            s.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            // (where it goes: the launcher's own folder in the player's programs)
            if (value) s.Children.Add(new TextBlock { Text = Path.GetDirectoryName(Paths.InstalledExe) ?? "", FontFamily = new FontFamily("Consolas"), FontSize = 11, Foreground = Res("ApexMutedBrush"),
                TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 2, 0, 0) });
            Grid.SetColumn(s, 1);
            g.Children.Add(s);
            var check = new Border { Width = 18, Height = 18, CornerRadius = new CornerRadius(9), BorderThickness = new Thickness(1.5), VerticalAlignment = VerticalAlignment.Center,
                BorderBrush = Res(on ? "ApexAccentBrush" : "ApexMutedBrush"),
                Child = on ? new Border { Width = 8, Height = 8, CornerRadius = new CornerRadius(4), Background = Res("ApexAccentBrush") } : null };
            Grid.SetColumn(check, 2);
            g.Children.Add(check);
            var row = new Border { Child = g, Padding = new Thickness(16, 13, 16, 13), Background = on ? Res("ApexAccentSoftBrush") : null,
                BorderThickness = new Thickness(0, list.Children.Count > 0 ? 1 : 0, 0, 0), BorderBrush = Res("ApexHairlineBrush") };
            var b = new Button { Content = row, Padding = new Thickness(0), BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch };
            b.Click += (_, _) => { _doneInstall = value; FillDoneChoice(); UpdateFooter(); };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, title);
            list.Children.Add(b);
        }
        DoneChoice.Children.Add(new Border { Child = list, CornerRadius = new CornerRadius(14), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush"),
            Background = Res("ApexHairlineBrush"), Margin = new Thickness(0, 10, 0, 0) });
    }

    // Settings: this copy, installed or not, and the way to switch
    Border InstallModeRow()
    {
        var self = Environment.ProcessPath ?? "";
        var mode = LauncherInstall.Mode;
        var (text, action) = mode switch
        {
            LauncherMode.Installed => (T("Installed for this Windows account."), Button(T("Use without installing"), async () =>
            {
                var picker = new Microsoft.Windows.Storage.Pickers.FolderPicker(AppWindow.Id);
                if (await picker.PickSingleFolderAsync() is not { } f) return;
                if (await Confirm(T("Use without installing?"), T("Apex Launcher moves to {0} and leaves the Start menu, the desktop and Installed apps. Your settings stay.", f.Path), T("Move it")))
                    await ApplyInstallMode(false, self, f.Path);
            })),
            LauncherMode.Portable => (T("Not installed: running from {0}.", ShortPath(Path.GetDirectoryName(self) ?? self)), Button(T("Install it"), () => _ = ApplyInstallMode(true, self, null))),
            _ => (T("Running from {0}, not installed yet.", ShortPath(Path.GetDirectoryName(self) ?? self)), Button(T("Choose"), () => UpgradeShow(0))),
        };
        return Row("hard-drive", T("This copy of the launcher"), text, DryRun ? null : action);
    }
}
