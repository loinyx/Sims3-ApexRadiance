using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace ApexSetup;

// The install's background: soft bands of light in the logo's colours (green below, pink and violet above) drifting slowly over
// a near-black sky, fading out before the bottom. Moves only while started, and only when Windows' animations are on.
public sealed class Aurora : Grid
{
    readonly Canvas _sky = new();
    readonly Storyboard _move = new();
    readonly List<(Ellipse Band, double Top, double WidthShare)> _bands = new();
    bool _on;

    // (colour, height, top as a share of the height, width as a share of the width, drift in px, seconds of one drift)
    static readonly (uint Argb, double H, double Top, double W, double Drift, double Secs)[] Bands =
    [
        (0xFF7E5CDC, 220, -0.06, 1.0, 70, 41),
        (0xFFC96ED6, 240, 0.02, 1.1, 90, 29),
        (0xFF45E08F, 300, 0.12, 1.25, 110, 23),
        (0xFF3AA6C4, 220, 0.24, 1.0, 80, 37),
    ];

    public Aurora()
    {
        IsHitTestVisible = false;
        Children.Add(_sky);
        var fade = new Rectangle();
        var g = new LinearGradientBrush { StartPoint = new(0, 0), EndPoint = new(0, 1) };
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(0, 5, 6, 7), Offset = 0.35 });
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(255, 5, 6, 7), Offset = 0.85 });
        fade.Fill = g;
        Children.Add(fade);
        Background = new SolidColorBrush(Color.FromArgb(255, 5, 6, 7));
        foreach (var (argb, h, top, w, drift, secs) in Bands)
        {
            var c = Color.FromArgb(255, (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);
            var brush = new RadialGradientBrush();
            brush.GradientStops.Add(new GradientStop { Color = Color.FromArgb(120, c.R, c.G, c.B), Offset = 0 });
            brush.GradientStops.Add(new GradientStop { Color = Color.FromArgb(46, c.R, c.G, c.B), Offset = 0.5 });
            brush.GradientStops.Add(new GradientStop { Color = Color.FromArgb(0, c.R, c.G, c.B), Offset = 1 });
            var t = new CompositeTransform();
            var e = new Ellipse { Height = h, Fill = brush, RenderTransform = t, RenderTransformOrigin = new(0.5, 0.5), Opacity = 0.85 };
            _sky.Children.Add(e);
            _bands.Add((e, top, w));
            Animate(t, "TranslateX", -drift, drift, secs);
            Animate(t, "Rotation", -4, 4, secs * 1.3);
            Animate(t, "ScaleY", 0.85, 1.15, secs * 0.7);
            Animate(e, "Opacity", 0.55, 0.95, secs * 0.45);
        }
        SizeChanged += (_, a) =>
        {
            Clip = new RectangleGeometry { Rect = new(0, 0, a.NewSize.Width, a.NewSize.Height) };
            foreach (var (band, top, share) in _bands)
            {
                band.Width = a.NewSize.Width * share;
                Canvas.SetLeft(band, (a.NewSize.Width - band.Width) / 2);
                Canvas.SetTop(band, a.NewSize.Height * top);
            }
        };
    }

    void Animate(DependencyObject target, string property, double from, double to, double secs)
    {
        var a = new DoubleAnimation
        {
            From = from, To = to, Duration = TimeSpan.FromSeconds(secs), AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut },
        };
        Storyboard.SetTarget(a, target);
        Storyboard.SetTargetProperty(a, property);
        _move.Children.Add(a);
    }

    // shows and moves it (still when Windows' animation effects are off), or hides it and stops
    public void Run(bool show)
    {
        Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        var move = show && new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;
        if (move == _on) return;
        _on = move;
        if (move) _move.Begin(); else _move.Stop();
    }
}
