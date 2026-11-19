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
        if (Maintenance || DryRun || FirstInstall || LauncherInstall.Mode != LauncherMode.NotChosen || Environment.ProcessPath is not { } self) return;
        var install = new RadioButton { IsChecked = true, GroupName = "mode", HorizontalAlignment = HorizontalAlignment.Stretch };
        var portable = new RadioButton { GroupName = "mode", HorizontalAlignment = HorizontalAlignment.Stretch };
        FrameworkElement Option(RadioButton r, string icon, string title, string text)
        {
            var g = new Grid { ColumnSpacing = 12 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition());
            g.Children.Add(Lucide.Icon(icon, 20, Res("ApexAccentBrush")));
            var s = new StackPanel { Spacing = 2 };
            s.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold });
            s.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            Grid.SetColumn(s, 1);
            g.Children.Add(s);
            r.Content = g;
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(r, title);
            return new Border { Child = r, Padding = new Thickness(12, 8, 12, 8), CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), BorderBrush = Res("ApexCardBorderBrush") };
        }
        var body = new StackPanel { Spacing = 10, MaxWidth = 480 };
        body.Children.Add(new TextBlock { Text = T("Install it for this Windows account, or keep it as a single file you can run from any folder. Your settings and games are kept either way."), TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush") });
        body.Children.Add(Option(install, "hard-drive", T("Install (recommended)"), T("Adds Apex Launcher to the Start menu and Installed apps, easy to remove. Updates itself in place.")));
        body.Children.Add(Option(portable, "package", T("Keep portable"), T("Runs from {0}, nothing added to Windows. Updates itself in place.", Path.GetDirectoryName(self) ?? self)));
        var d = new ContentDialog
        {
            Title = T("How do you want to use Apex Launcher?"), Content = body, PrimaryButtonText = T("Continue"), CloseButtonText = T("Ask me later"),
            DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
        };
        if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        await ApplyInstallMode(install.IsChecked == true, self, null);
    }

    // install (this copy starts the installed one and closes) or make it portable (where it is, or in a folder chosen)
    async Task ApplyInstallMode(bool install, string self, string? folder)
    {
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
