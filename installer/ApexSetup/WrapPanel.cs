using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace ApexSetup;

// Children left to right, on as many lines as they need (the Mods filter chips): WinUI has no wrap panel of its own
public sealed class WrapPanel : Panel
{
    public double Spacing { get; set; } = 6;

    protected override Size MeasureOverride(Size available)
    {
        double x = 0, y = 0, line = 0, width = 0;
        foreach (var c in Children)
        {
            c.Measure(new Size(available.Width, double.PositiveInfinity));
            var s = c.DesiredSize;
            if (x > 0 && x + s.Width > available.Width) { y += line + Spacing; x = 0; line = 0; }
            x += s.Width + Spacing;
            line = Math.Max(line, s.Height);
            width = Math.Max(width, x - Spacing);
        }
        return new Size(double.IsInfinity(available.Width) ? width : available.Width, y + line);
    }

    protected override Size ArrangeOverride(Size final)
    {
        double x = 0, y = 0, line = 0;
        foreach (var c in Children)
        {
            var s = c.DesiredSize;
            if (x > 0 && x + s.Width > final.Width) { y += line + Spacing; x = 0; line = 0; }
            c.Arrange(new Rect(x, y, s.Width, s.Height));
            x += s.Width + Spacing;
            line = Math.Max(line, s.Height);
        }
        return final;
    }
}
