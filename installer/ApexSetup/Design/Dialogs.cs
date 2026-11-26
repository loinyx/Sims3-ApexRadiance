using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace ApexSetup.Design;

// Every dialog in one shape, whatever opens it (MainWindow.ShowDialog dresses it): an icon and the title, the words at reading size,
// and the buttons on the right with the one that goes on last, as the install's footer. A dialog says only what is its own: its icon
// in Tag (a Lucide name, "!" before it for what needs care), its body in one of the three widths.
public static class Dialogs
{
    public const double Small = 440, Medium = 560, Large = 680;

    public static void Dress(ContentDialog d)
    {
        var icon = d.Tag as string ?? (d.PrimaryButtonStyle is not null ? "!trash-2" : null);
        d.Tag = null;
        d.CornerRadius = new CornerRadius(Radius.Panel);
        if (d.Content is string text) d.Content = Words(text);
        // (a body that scrolls: the width is of what is in it)
        var width = d.Content is FrameworkElement { Width: > 0 and var w } ? w : d.Content is ScrollViewer { Content: FrameworkElement { Width: > 0 and var inside } } ? inside : Small;
        if (d.Content is FrameworkElement { Width: double.NaN } body and not ScrollViewer { Content: FrameworkElement { Width: > 0 } })
        {
            body.Width = width = Math.Clamp(Math.Max(body.MinWidth, double.IsInfinity(body.MaxWidth) ? Small : body.MaxWidth), Small, Small);
            if (body.MaxWidth < Small) body.MaxWidth = Small;
        }
        // (the body's width, its padding on both sides, the dialog's 1-unit border on both sides and 2 units the template's scroller
        // keeps: with less, a field's or card's right border at the body's edge was cut)
        var outer = width + 2 * Space.X6 + 4;
        (d.Resources["ContentDialogMinWidth"], d.Resources["ContentDialogMaxWidth"], d.Resources["ContentDialogMaxHeight"]) = (Math.Min(outer, 320), outer, 900.0);
        (d.Resources["ContentDialogPadding"], d.Resources["ContentDialogTitleMargin"]) = (new Thickness(Space.X6), new Thickness(0, 0, 0, Space.X4));
        Title(d, icon);
        // (some change their title while they are open)
        d.RegisterPropertyChangedCallback(ContentDialog.TitleProperty, (_, _) => Title(d, icon));
        // (as soon as its parts are there)
        var dressed = false;
        d.LayoutUpdated += (_, _) => { if (!dressed) dressed = Buttons(d, d.DefaultButton == ContentDialogButton.None); };
    }

    public static TextBlock Words(string text) => new()
    {
        Text = text, FontSize = TextSize.Body, LineHeight = 22, TextWrapping = TextWrapping.Wrap, Foreground = App.Brush("ApexSoftTextBrush"),
    };

    static void Title(ContentDialog d, string? icon)
    {
        if (d.Title is not string title) return;
        var words = new TextBlock { Text = title, FontSize = TextSize.Title3, LineHeight = 28, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center };
        var g = new Grid { ColumnSpacing = Space.IconToText };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        if (icon is not null)
        {
            var tile = icon[0] == '!' ? IconTile.Of(icon[1..], icon is "!trash-2" or "!circle-alert" ? 25 : 78, Size.Tile) : IconTile.Of(icon);
            tile.VerticalAlignment = VerticalAlignment.Top;
            g.Children.Add(tile);
            Grid.SetColumn(words, 1);
        }
        else Grid.SetColumnSpan(words, 2);
        g.Children.Add(words);
        AutomationProperties.SetName(d, title);
        d.Title = g;
    }

    // the template's buttons share the width: here each is as wide as its words, on the right, the one that goes on last
    // careful: the keyboard starts on the button that changes nothing
    static bool Buttons(ContentDialog d, bool careful)
    {
        var all = new Dictionary<string, Button>();
        Find(VisualTreeHelper.GetParent(d) ?? d);
        if (all.Count < 3 || VisualTreeHelper.GetParent(all["CloseButton"]) is not Grid row) return false;
        foreach (var b in all.Values) (b.MinHeight, b.MinWidth, b.HorizontalAlignment) = (Size.ControlLarge, 112, HorizontalAlignment.Stretch);
        row.HorizontalAlignment = HorizontalAlignment.Right;
        if (all.TryGetValue("PrimaryButton", out var go) && all.TryGetValue("CloseButton", out var close) && go.Visibility == Visibility.Visible && close.Visibility == Visibility.Visible)
        {
            var (first, last) = (Grid.GetColumn(go), Grid.GetColumn(close));
            if (first < last) { Grid.SetColumn(go, last); Grid.SetColumn(close, first); }
            (close.TabIndex, go.TabIndex) = (0, 2);
            if (all.TryGetValue("SecondaryButton", out var other)) other.TabIndex = 1;
            if (careful) close.Focus(FocusState.Programmatic);
        }
        return true;

        void Find(DependencyObject o)
        {
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(o); i++)
            {
                var c = VisualTreeHelper.GetChild(o, i);
                if (c is Button { Name: "PrimaryButton" or "SecondaryButton" or "CloseButton" } b) all[b.Name] = b;
                else Find(c);
            }
        }
    }
}
