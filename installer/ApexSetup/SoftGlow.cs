using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace ApexSetup;

// A very soft light behind the top of a screen, in the colour given: one wide pool with a faint heart, gone well before its
// edges. A bitmap with a dither of one step, since a gradient this dark shows its steps as rings. It breathes slowly, and is
// still when Windows' animation effects are off.
public sealed class SoftGlow : Grid
{
    const int W = 640, H = 360;

    // strength: the light's share at its brightest, 0 to 1. at: where its middle is, as a share of the height
    public SoftGlow(Color color, double strength, double at = 0.34)
    {
        IsHitTestVisible = false;
        VerticalAlignment = VerticalAlignment.Top;
        Children.Add(new Image { Source = Light(color, strength, at), Stretch = Stretch.Fill });
        Loaded += (_, _) => Breathe();
    }

    // (a screen built again shows the same picture)
    static readonly Dictionary<(Color, double, double), WriteableBitmap> Made = new();
    static WriteableBitmap Light(Color color, double strength, double at)
    {
        if (Made.TryGetValue((color, strength, at), out var made)) return made;
        var px = new byte[W * H * 4];
        var noise = new Random(7); // (the same grain each time: a rebuilt screen does not flicker)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                double dx = (x / (double)W - 0.5) / 0.5, dy = (y / (double)H - at) / (y < at * H ? at * 1.25 : 1 - at);
                var d = Math.Sqrt(dx * dx + dy * dy);
                var wide = Fall(d);
                var heart = Fall(d * 2.4);
                var a = strength * (0.72 * wide + 0.28 * heart);
                // (premultiplied: the colour's share never over its alpha's)
                var grain = noise.NextDouble();
                byte Step(double v) => (byte)Math.Clamp(v * 255 + grain, 0, 255);
                var alpha = Step(a);
                var o = (y * W + x) * 4;
                px[o] = Math.Min(alpha, Step(a * color.B / 255));
                px[o + 1] = Math.Min(alpha, Step(a * color.G / 255));
                px[o + 2] = Math.Min(alpha, Step(a * color.R / 255));
                px[o + 3] = alpha;
            }
        var bmp = new WriteableBitmap(W, H);
        using (var s = bmp.PixelBuffer.AsStream()) s.Write(px);
        bmp.Invalidate();
        return Made[(color, strength, at)] = bmp;
    }

    // 1 in the middle to 0 at the edge, flat at both ends
    static double Fall(double d) => d >= 1 ? 0 : 1 - d * d * d * (d * (d * 6 - 15) + 10) is var t ? t * t : 0;

    void Breathe()
    {
        if (!new Windows.UI.ViewManagement.UISettings().AnimationsEnabled) return;
        var v = ElementCompositionPreview.GetElementVisual(this);
        var a = v.Compositor.CreateScalarKeyFrameAnimation();
        var ease = v.Compositor.CreateCubicBezierEasingFunction(new(0.45f, 0), new(0.55f, 1));
        a.InsertKeyFrame(0, 1);
        a.InsertKeyFrame(0.5f, 0.86f, ease);
        a.InsertKeyFrame(1, 1, ease);
        a.Duration = TimeSpan.FromSeconds(9);
        a.IterationBehavior = AnimationIterationBehavior.Forever;
        v.StartAnimation("Opacity", a);
    }
}
