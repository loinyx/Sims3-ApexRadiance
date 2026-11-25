using System.Numerics;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace ApexSetup.Design;

// The pieces screens are made of. A new screen takes them from here; a shape drawn twice becomes one of these.
// docs/launcher-design.md says when each one is used, and ApexSetup.Tests/DesignTests.cs keeps screens from drawing their own.
public static class Parts
{
    static Brush B(string name) => App.Brush("Apex" + name + "Brush");

    // ---------- buttons ----------

    // a button that shows its fill only under the pointer
    public static T Ghost<T>(T b) where T : Control
    {
        (b.Background, b.BorderThickness) = (B("Clear"), new Thickness(0));
        return b;
    }

    // an icon alone, named for the tooltip and the screen reader
    public static Button Icon(string icon, string name, double size = Size.ControlSmall, Brush? ink = null) =>
        Named(Ghost(new Button { Content = Lucide.Icon(icon, Size.IconSmall, ink), Width = size, Height = size, Padding = new Thickness(0) }), name);

    // the same with an outline, beside a larger button (the bin of a footer)
    public static Button IconBoxed(string icon, string name, double size = Size.ControlLarge, Brush? ink = null)
    {
        var b = Icon(icon, name, size, ink);
        (b.BorderThickness, b.BorderBrush) = (new Thickness(1), B("CardBorder"));
        return b;
    }

    // a round one over a picture, in a corner: as far from the top, or the bottom, as from the side
    public static Button OverPicture(Button b, VerticalAlignment at)
    {
        (b.Width, b.Height, b.CornerRadius, b.Background) = (Size.ControlSmall, Size.ControlSmall, new CornerRadius(Size.ControlSmall / 2), B("Veil"));
        (b.HorizontalAlignment, b.VerticalAlignment, b.Margin) = (HorizontalAlignment.Right, at, new Thickness(Space.X2));
        return b;
    }

    public static T Named<T>(T e, string name) where T : FrameworkElement
    {
        AutomationProperties.SetName(e, name);
        ToolTipService.SetToolTip(e, name);
        return e;
    }

    // ---------- surfaces ----------

    // a card inside a panel
    public static Border Card(UIElement child, Thickness padding = default) => new()
    {
        Child = child, Padding = padding, CornerRadius = new CornerRadius(Radius.Card), Background = B("Raised"), BorderBrush = B("CardBorder"), BorderThickness = new Thickness(1),
    };

    // the panel at the side of a screen: a rounded card with a body that scrolls and a footer for its main action
    public static (Border Panel, StackPanel Body, Grid Foot) SidePanel(Thickness margin)
    {
        var g = new Grid();
        g.RowDefinitions.Add(new RowDefinition());
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var body = new StackPanel { Spacing = Space.X6, Padding = new Thickness(Space.CardPad) };
        g.Children.Add(new ScrollViewer { Content = body });
        var foot = new Grid { Padding = new Thickness(Space.CardPad), ColumnSpacing = Space.X2, BorderBrush = B("CardBorder"), BorderThickness = new Thickness(0, 1, 0, 0) };
        Grid.SetRow(foot, 1);
        g.Children.Add(foot);
        return (PanelOf(g, margin), body, foot);
    }

    public static Border PanelOf(UIElement child, Thickness margin) => new()
    {
        Child = child, Width = Size.SidePanel, Margin = margin, CornerRadius = new CornerRadius(Radius.Panel), Background = B("Card"), BorderBrush = B("CardBorder"), BorderThickness = new Thickness(1),
    };

    // a footer's main action over the width left, what else it does after it
    public static void FillFoot(Grid foot, FrameworkElement main, params FrameworkElement[] more)
    {
        foot.Children.Clear();
        foot.ColumnDefinitions.Clear();
        foreach (var (e, i) in new[] { main }.Concat(more).Select((e, i) => (e, i)))
        {
            foot.ColumnDefinitions.Add(new ColumnDefinition { Width = i == 0 ? new GridLength(1, GridUnitType.Star) : GridLength.Auto });
            (e.Height, e.HorizontalAlignment) = (Size.ControlLarge, i == 0 ? HorizontalAlignment.Stretch : HorizontalAlignment.Right);
            Grid.SetColumn(e, i);
            foot.Children.Add(e);
        }
    }

    // ---------- words ----------

    // a panel's title and the line under it
    public static StackPanel Title(FrameworkElement title, string sub)
    {
        var s = new StackPanel { Spacing = Space.TitleToSub, Children = { title } };
        s.Children.Add(new TextBlock { Text = sub, FontSize = TextSize.Body, Foreground = B("Muted"), TextWrapping = TextWrapping.Wrap });
        return s;
    }

    public static TextBlock TitleText(string text) =>
        new() { Text = text, FontSize = TextSize.Title3, LineHeight = 28, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis };

    // a section inside a panel
    public static TextBlock Heading(string text) => new() { Text = text, FontSize = TextSize.Body, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };

    // what something is, in a quiet chip; warn: it has to be seen
    public static Border Chip(string icon, string text, bool warn = false)
    {
        var s = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space.X2 };
        s.Children.Add(Lucide.Icon(icon, 14, B(warn ? "Warning" : "Muted")));
        s.Children.Add(new TextBlock { Text = text, FontSize = TextSize.Small, Foreground = B(warn ? "WarningText" : "SoftText"), VerticalAlignment = VerticalAlignment.Center });
        return new Border { Child = s, Height = Size.ControlSmall, Padding = new Thickness(Space.X3, 0, Space.X3, 0), CornerRadius = new CornerRadius(Radius.Control), Background = B(warn ? "WarningSoft" : "Tile") };
    }

    // ---------- fields ----------

    // a value that can be typed over: a value like the others until the pointer or the keyboard is on it
    public static TextBox QuietField(TextBox t, bool editable)
    {
        var none = B("Clear");
        (t.FontSize, t.MinWidth, t.MinHeight, t.Height) = (TextSize.Small, 0, 0, Size.ControlSmall);
        (t.Padding, t.BorderThickness, t.BorderBrush, t.Background, t.IsReadOnly) = (new Thickness(7, 6, 7, 6), new Thickness(1), none, none, !editable);
        foreach (var k in new[] { "TextControlBackgroundPointerOver", "TextControlBackgroundFocused" }) t.Resources[k] = editable ? B("Tile") : none;
        t.Resources["TextControlBorderBrushPointerOver"] = none;
        t.Resources["TextControlBorderBrushFocused"] = editable ? B("Accent") : none;
        t.Resources["TextControlBorderThemeThicknessFocused"] = new Thickness(1);
        foreach (var k in new[] { "", "PointerOver", "Focused" }) t.Resources["TextControlPlaceholderForeground" + k] = B("Muted");
        return t;
    }
}

// Two or more choices of one thing, one of them on: the mark slides to the one chosen
public sealed partial class Segmented : Grid
{
    readonly Border _mark = new() { CornerRadius = new CornerRadius(Radius.Tag), HorizontalAlignment = HorizontalAlignment.Left };
    readonly List<Button> _all = [];
    int _on;
    bool _inked;

    // fill: the choices share the width there is; otherwise each is as wide as its words
    public Segmented(IReadOnlyList<(string? Icon, string Text)> items, int on, Action<int> pick, bool fill = false)
    {
        _on = on;
        (Height, Padding, CornerRadius, BorderThickness, VerticalAlignment) = (Size.Control, new Thickness(2), new CornerRadius(Radius.Control), new Thickness(1), VerticalAlignment.Center);
        HorizontalAlignment = fill ? HorizontalAlignment.Stretch : HorizontalAlignment.Left;
        ColumnSpacing = 2;
        SetColumnSpan(_mark, Math.Max(1, items.Count));
        Children.Add(_mark);
        for (int i = 0; i < items.Count; i++)
        {
            ColumnDefinitions.Add(new ColumnDefinition { Width = fill ? new GridLength(1, GridUnitType.Star) : GridLength.Auto });
            var s = new Grid { ColumnSpacing = items[i].Icon is null ? 0 : Space.X2 }; // (a grid: the words are cut short with an ellipsis)
            s.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            s.ColumnDefinitions.Add(new ColumnDefinition());
            if (items[i].Icon is { } icon) s.Children.Add(new LucideIcon { Kind = icon, Size = Size.IconSmall, StrokeWidth = 1.75, Follow = true, VerticalAlignment = VerticalAlignment.Center });
            var words = new TextBlock { Text = items[i].Text, FontSize = TextSize.Small, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
            SetColumn(words, 1);
            s.Children.Add(words);
            var b = Parts.Ghost(new Button { Content = s, Padding = new Thickness(fill ? Space.X1 : Space.X4, 0, fill ? Space.X1 : Space.X4, 0), CornerRadius = new CornerRadius(Radius.Tag), MinWidth = 0, HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch });
            AutomationProperties.SetName(b, items[i].Text);
            if (fill) ToolTipService.SetToolTip(b, items[i].Text);
            var at = i;
            b.Click += (_, _) => { if (_on == at) return; _on = at; Place(true); Tell(); pick(at); };
            SetColumn(b, i);
            Children.Add(b);
            _all.Add(b);
        }
        Tell();
        Paint(null);
        Loaded += (_, _) => Place(false);
        SizeChanged += (_, _) => Place(false);
    }

    // (an ink of its own over a picture or a board; null: the app's)
    public void Paint(Windows.UI.Color? ink)
    {
        _inked = ink is not null;
        Brush Of(byte a, string name) => ink is { } c ? new SolidColorBrush(Windows.UI.Color.FromArgb(a, c.R, c.G, c.B)) : App.Brush("Apex" + name + "Brush");
        (Background, BorderBrush, _mark.Background) = (Of(0x1A, "Card"), Of(0x30, "CardBorder"), Of(0x2E, "Selected"));
        foreach (var b in _all)
        {
            var fore = ink is { } c ? new SolidColorBrush(c) : App.Brush("TextFillColorPrimaryBrush");
            b.Foreground = fore;
            foreach (var state in new[] { "PointerOver", "Pressed" })
            {
                b.Resources["ButtonForeground" + state] = fore;
                b.Resources["ButtonBackground" + state] = App.Brush("ApexClearBrush");
            }
        }
        Dim();
    }

    void Dim() { for (int i = 0; i < _all.Count; i++) ((UIElement)_all[i].Content).Opacity = i == _on || _inked ? 1 : 0.7; } // (on a board the mark alone says which: faded ink would not read)

    // the keyboard on the choice that is on, for a switch built again in the place of the one pressed
    public void FocusWhenShown() => Loaded += (_, _) => _all.ElementAtOrDefault(_on)?.Focus(FocusState.Programmatic);

    void Tell()
    {
        for (int i = 0; i < _all.Count; i++) AutomationProperties.SetItemStatus(_all[i], i == _on ? Strings.T("Selected") : "");
        Dim();
    }

    void Place(bool slide)
    {
        if (_on < 0 || _on >= _all.Count || _all[_on].ActualWidth <= 0) return;
        _mark.TranslationTransition = slide ? new Vector3Transition { Duration = TimeSpan.FromMilliseconds(Motion.Base) } : null;
        _mark.Width = _all[_on].ActualWidth;
        _mark.Translation = new Vector3(_all[_on].ActualOffset.X - (float)Padding.Left - (float)BorderThickness.Left, 0, 0);
    }
}
