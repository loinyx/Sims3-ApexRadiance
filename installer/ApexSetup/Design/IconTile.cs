using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace ApexSetup.Design;

// The icon at the start of a row or a card: the outline on a quiet neutral tile. Colour is kept for what has to be seen: a warning or
// an error (the overload with a hue), never for decoration.
public static class IconTile
{
    public static Border Of(string icon, double size = Size.Tile) => Make(icon, App.Brush("ApexTileBrush"), App.Brush("ApexSoftTextBrush"), size);

    public static Border Of(string icon, double hue, double size) => App.Light
        ? Make(icon, new SolidColorBrush(Oklch.Of(0.62, 0.15, hue, 0.12)), new SolidColorBrush(Oklch.Of(0.50, 0.15, hue)), size)
        : Make(icon, new SolidColorBrush(Oklch.Of(0.74, 0.13, hue, 0.13)), new SolidColorBrush(Oklch.Of(0.80, 0.12, hue)), size);

    static Border Make(string icon, Brush back, Brush ink, double size)
    {
        var mark = Lucide.Icon(icon, size / 2, ink, 1.75);
        (mark.HorizontalAlignment, mark.VerticalAlignment) = (HorizontalAlignment.Center, VerticalAlignment.Center);
        return new Border { Width = size, Height = size, CornerRadius = new CornerRadius(size / 4), VerticalAlignment = VerticalAlignment.Center, Background = back, Child = mark };
    }
}
