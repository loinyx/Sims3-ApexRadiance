using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace ApexSetup;

// Cards in as many equal columns as fit, stretched to the full width: no empty strip on the right at any window size
// (a wrap grid of fixed-width items leaves one). Each row is as tall as its tallest card.
public sealed class CardGrid : Panel
{
    public double MinItemWidth { get; set; } = 240;
    public double Spacing { get; set; } = 16;

    public static int Columns(double width, double min, double gap) =>
        double.IsInfinity(width) || width <= 0 ? 1 : Math.Max(1, (int)((width + gap) / (min + gap)));

    double ItemWidth(double width, int cols) => double.IsInfinity(width) ? MinItemWidth : (width - Spacing * (cols - 1)) / cols;

    protected override Size MeasureOverride(Size available)
    {
        var cols = Columns(available.Width, MinItemWidth, Spacing);
        var w = ItemWidth(available.Width, cols);
        double height = 0, row = 0;
        for (int i = 0; i < Children.Count; i++)
        {
            Children[i].Measure(new Size(w, double.PositiveInfinity));
            row = Math.Max(row, Children[i].DesiredSize.Height);
            if (i % cols == cols - 1 || i == Children.Count - 1) { height += row + (height > 0 ? Spacing : 0); row = 0; }
        }
        return new Size(double.IsInfinity(available.Width) ? cols * w : available.Width, height);
    }

    protected override Size ArrangeOverride(Size final)
    {
        var cols = Columns(final.Width, MinItemWidth, Spacing);
        var w = ItemWidth(final.Width, cols);
        double y = 0;
        for (int start = 0; start < Children.Count; start += cols)
        {
            var end = Math.Min(start + cols, Children.Count);
            double row = 0;
            for (int i = start; i < end; i++) row = Math.Max(row, Children[i].DesiredSize.Height);
            for (int i = start; i < end; i++) Children[i].Arrange(new Rect((i - start) * (w + Spacing), y, w, row));
            y += row + Spacing;
        }
        return final;
    }
}
