using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace ApexSetup;

// The layout audit for UI tests (APEX_LAUNCHER_AUDIT=<file>): after a screen shows or the window changes size, every
// visible text is checked and problems are appended to the file, one per line: a text cut short (ellipsis), the text of a
// button, box or combo on two lines, or a control reaching past the window's edge. ui-smoke reads the file at each size.
public sealed partial class MainWindow
{
    static readonly string? AuditFile = Environment.GetEnvironmentVariable("APEX_LAUNCHER_AUDIT");
    bool _auditQueued;

    void QueueAudit()
    {
        if (AuditFile is null || _auditQueued) return;
        _auditQueued = true;
        // (after layout and the screen's animation settle)
        var t = DispatcherQueue.CreateTimer();
        t.Interval = TimeSpan.FromMilliseconds(900);
        t.IsRepeating = false;
        t.Tick += (_, _) => { _auditQueued = false; Audit(); };
        t.Start();
    }

    void Audit()
    {
        if (AuditFile is null || Content is not FrameworkElement root) return;
        var lines = new List<string>();
        var size = $"{root.ActualWidth:0}x{root.ActualHeight:0}";
        var screen = _step.ToString();
        void Walk(DependencyObject d, string path)
        {
            if (d is UIElement { Visibility: Visibility.Collapsed }) return;
            var name = d is FrameworkElement { Name.Length: > 0 } fe ? fe.Name : d.GetType().Name;
            if (d is TextBlock tb && tb.Text.Length > 0 && tb.ActualWidth > 0)
            {
                // (a long name cut in a list row is by design, with its full text in a tooltip)
                if (tb.IsTextTrimmed && !InListRow(tb)) lines.Add($"{screen}\t{size}\ttrimmed\t{path}/{name}\t{tb.Text}");
                var inControl = InSingleLineControl(tb);
                if (inControl && tb.ActualHeight > tb.FontSize * 2.0) lines.Add($"{screen}\t{size}\twrapped\t{path}/{name}\t{tb.Text}");
            }
            if (d is Control { ActualWidth: > 0 } c && c is Microsoft.UI.Xaml.Controls.Button or ComboBox or TextBox or AutoSuggestBox or ToggleSwitch)
            {
                try
                {
                    var p = c.TransformToVisual(root).TransformPoint(new Windows.Foundation.Point(0, 0));
                    if (p.X + c.ActualWidth > root.ActualWidth + 1 && p.X < root.ActualWidth) lines.Add($"{screen}\t{size}\toverflow\t{path}/{name}\t{(c as ContentControl)?.Content}");
                }
                catch (ArgumentException) { }
            }
            var n = VisualTreeHelper.GetChildrenCount(d);
            for (int i = 0; i < n; i++) Walk(VisualTreeHelper.GetChild(d, i), path.Length > 300 ? path : path + "/" + name);
        }
        Walk(root, "");
        try { File.AppendAllLines(AuditFile, lines.Count > 0 ? lines : [$"{screen}\t{size}\tok"]); } catch (IOException) { }
    }

    // every launcher screen at five window sizes (in the monitor's scale), then the app closes: ui-smoke reads the file
    async Task AuditTourAsync()
    {
        var scale = Content.XamlRoot.RasterizationScale;
        int[] screens = [Home, ComponentsStep, ModsScreen, WorldsScreen, SavesScreen, HealthScreen, NewsScreen, SettingsScreen];
        foreach (var (w, h) in new[] { (760, 560), (980, 680), (1180, 760), (1440, 900), (1920, 1080) })
        {
            AppWindow.Resize(new Windows.Graphics.SizeInt32((int)(w * scale), (int)(h * scale)));
            foreach (var s in screens)
            {
                Show(s);
                await Task.Delay(1600);
                Audit();
            }
        }
        File.AppendAllLines(AuditFile!, ["tour	done"]);
        Close();
    }

    static bool InListRow(DependencyObject d)
    {
        for (var p = VisualTreeHelper.GetParent(d); p is not null; p = VisualTreeHelper.GetParent(p))
            if (p is ListViewItem or GridViewItem) return true;
        return false;
    }

    // a text inside a button, combo, text box or toggle: one line is expected there
    static bool InSingleLineControl(DependencyObject d)
    {
        for (var p = VisualTreeHelper.GetParent(d); p is not null; p = VisualTreeHelper.GetParent(p))
        {
            if (p is Microsoft.UI.Xaml.Controls.Button or ComboBox or TextBox or AutoSuggestBox or ToggleSwitch or SelectorBarItem or NavigationViewItem) return true;
            if (p is ListViewItem or GridViewItem or ContentDialog or Page) return false;
        }
        return false;
    }
}
