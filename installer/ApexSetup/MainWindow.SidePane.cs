using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// The pane at the side of Saves and the gallery, and the parts Mods' own is made of: a search over groups of places, one look on
// the three screens (docs/launcher-design.md). A narrow window has no room for it; the screen then offers a list in its place.
public sealed partial class MainWindow
{
    bool _sideOn;
    Action? _sideFitted; // the screen draws its narrow list, or takes it away
    bool SideFits => ContentFrame.ActualWidth >= 860;

    void ShowSide(FrameworkElement search, FrameworkElement list, Action fitted)
    {
        search.Margin = new Thickness(16, 0, 16, 4);
        var g = new Grid();
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        g.RowDefinitions.Add(new RowDefinition());
        Grid.SetRow(list, 1);
        g.Children.Add(search);
        g.Children.Add(list);
        SideHost.Child = g;
        (_sideOn, _sideFitted) = (true, fitted);
        FitSide();
    }

    void HideSide()
    {
        (_sideOn, _sideFitted, SideHost.Child) = (false, null, null);
        FitSide();
    }

    void FitSide()
    {
        var show = _sideOn && SideFits ? Visibility.Visible : Visibility.Collapsed;
        if (SideHost.Visibility == show) return;
        SideHost.Visibility = show;
        if (_sideOn) _sideFitted?.Invoke();
    }

    // a group's heading: the whole row folds it; what makes a new one in it sits on its right end
    FrameworkElement SideGroup(string key, string label, Action changed, (string Tip, Action Run)? add = null)
    {
        var g = new Grid { Tag = "-", Margin = new Thickness(4, 16, 4, 4), Height = 32 };
        var head = new Button
        {
            Background = Res("ApexClearBrush"), BorderThickness = new Thickness(0), Padding = new Thickness(12, 0, 8, 0),
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Stretch,
        };
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        s.Children.Add(new TextBlock { Text = label, FontSize = 12, FontWeight = FontWeights.SemiBold, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center });
        var fold = Lucide.Icon(_collapsed.Contains(key) ? "chevron-right" : "chevron-down", 12, Res("ApexMutedBrush"));
        fold.VerticalAlignment = VerticalAlignment.Center;
        s.Children.Add(fold);
        head.Content = s;
        AutomationProperties.SetName(head, label);
        AutomationProperties.SetItemStatus(head, _collapsed.Contains(key) ? T("Collapsed") : T("Expanded"));
        head.Click += (_, _) => { if (!_collapsed.Remove(key)) _collapsed.Add(key); SaveViewPrefs(); changed(); };
        g.Children.Add(head);
        if (add is { } a)
        {
            var plus = IconButton("plus", a.Tip);
            plus.HorizontalAlignment = HorizontalAlignment.Right;
            plus.Margin = new Thickness(0, 0, 6, 0);
            plus.Click += (_, _) => a.Run();
            g.Children.Add(plus);
        }
        return g;
    }

    // a place: its icon, its name and how many it holds
    static Grid SideLine(string icon, string label, string count)
    {
        var g = new Grid { ColumnSpacing = 12 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var mark = Lucide.Icon(icon, 16, Res("ApexMutedBrush"));
        mark.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(mark);
        var name = new TextBlock { Text = label, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(name, 1);
        g.Children.Add(name);
        var n = new TextBlock { Text = count, FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(n, 2);
        g.Children.Add(n);
        ToolTipService.SetToolTip(g, label);
        return g;
    }

    // the list itself: the rows with a Tag of their own are places, "-" is a heading
    ListView SideList(List<FrameworkElement> items, string name, string id, Action<string> pick)
    {
        var list = new ListView
        {
            SelectionMode = ListViewSelectionMode.Single, Padding = new Thickness(12, 0, 12, 12), ItemContainerStyleSelector = (StyleSelector)Root.Resources["SidePlaces"],
            ItemContainerTransitions = new Microsoft.UI.Xaml.Media.Animation.TransitionCollection(), ItemsSource = items,
        };
        AutomationProperties.SetName(list, name);
        AutomationProperties.SetAutomationId(list, id);
        list.SelectionChanged += (_, _) => { if (list.SelectedItem is FrameworkElement { Tag: string key } && key != "-") pick(key); };
        list.Loaded += (_, _) => { if (list.SelectedItem is { } at) list.ScrollIntoView(at); };
        return list;
    }
}
