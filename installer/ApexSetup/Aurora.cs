using System.Runtime.InteropServices.WindowsRuntime;
using ApexSetup.Design;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace ApexSetup;

// The background of the install and of the move to the launcher: wide, quiet pools of the theme's own hues, drifting into one another
// over the page's colour. They are summed as light (OKLab, so two hues meet in a clean one and never in grey), drawn small and scaled
// up, with a step of dither so no ring shows. Moves only while started, and only when Windows' animations are on.
public sealed class Aurora : Grid
{
    const int W = 192, H = 108;
    readonly WriteableBitmap _bmp = new(W, H);
    readonly byte[] _px = new byte[W * H * 4];
    readonly DispatcherTimer _tick = new() { Interval = TimeSpan.FromMilliseconds(33) };
    readonly System.Diagnostics.Stopwatch _clock = new();
    bool _on;

    // each pool: where its middle drifts (in shares of the window), how wide it is, how strong, how fast and from where in its round
    static readonly (float X, float Y, float Wide, float High, float Strong, float Speed, float Phase)[] Pools =
    [
        (0.16f, 0.14f, 0.42f, 0.46f, 1.00f, 0.11f, 0.0f), (0.84f, 0.10f, 0.40f, 0.44f, 0.95f, 0.09f, 2.1f), (0.52f, -0.04f, 0.46f, 0.38f, 0.80f, 0.07f, 4.0f),
        (0.92f, 0.70f, 0.34f, 0.46f, 0.62f, 0.10f, 1.2f), (0.06f, 0.78f, 0.36f, 0.44f, 0.58f, 0.08f, 3.3f), (0.50f, 1.06f, 0.50f, 0.34f, 0.42f, 0.06f, 5.2f),
    ];

    public Aurora()
    {
        IsHitTestVisible = false;
        Children.Add(new Image { Source = _bmp, Stretch = Stretch.Fill });
        _tick.Tick += (_, _) => Draw(_clock.Elapsed.TotalSeconds);
        ActualThemeChanged += (_, _) => Draw(_clock.Elapsed.TotalSeconds);
    }

    static (float L, float A, float B) Lab(double l, double c, double h) => ((float)l, (float)(c * Math.Cos(h * Math.PI / 180)), (float)(c * Math.Sin(h * Math.PI / 180)));

    void Draw(double seconds)
    {
        var t = (float)seconds;
        var light = ActualTheme == ElementTheme.Light;
        var sky = DesignSystem.Theme.Sky;
        var ground = Oklch.From(DesignSystem.Now["Page"]) is var g ? Lab(g.L, g.C, g.H) : default;
        var colours = Pools.Select((_, i) => Lab(light ? 0.86 : 0.58, light ? 0.09 : 0.17, sky[i % sky.Length])).ToArray();
        var most = light ? 0.55f : 0.42f;
        Span<(float X, float Y)> at = stackalloc (float, float)[Pools.Length];
        for (int i = 0; i < Pools.Length; i++)
        {
            var p = Pools[i];
            at[i] = (p.X + 0.09f * MathF.Sin(t * p.Speed + p.Phase), p.Y + 0.07f * MathF.Cos(t * p.Speed * 1.3f + p.Phase * 1.7f));
        }
        for (int py = 0; py < H; py++)
        {
            var y0 = py / (float)(H - 1);
            for (int px = 0; px < W; px++)
            {
                var x0 = px / (float)(W - 1);
                // (the pools' edges wave a little, so they read as light and not as discs)
                var x = x0 + 0.030f * MathF.Sin(y0 * 5.1f + t * 0.21f) + 0.016f * MathF.Sin(y0 * 11.3f - t * 0.17f);
                var y = y0 + 0.030f * MathF.Sin(x0 * 4.3f - t * 0.19f) + 0.016f * MathF.Sin(x0 * 9.7f + t * 0.23f);
                float sum = 0, sq = 0, la = 0, aa = 0, ba = 0;
                for (int i = 0; i < Pools.Length; i++)
                {
                    var p = Pools[i];
                    float dx = (x - at[i].X) / p.Wide, dy = (y - at[i].Y) / p.High;
                    var w = p.Strong * MathF.Exp(-1.6f * (dx * dx + dy * dy));
                    sum += w;
                    // (the nearest pool keeps its own colour: an even mix of four hues is grey)
                    var own = w * MathF.Sqrt(w);
                    sq += own;
                    la += own * colours[i].L; aa += own * colours[i].A; ba += own * colours[i].B;
                }
                // (how much light there is, never past the most it may be; the middle stays calm under the card)
                var k = most * (1 - MathF.Exp(-1.15f * sum));
                float mx = (x0 - 0.5f) / 0.42f, my = (y0 - 0.52f) / 0.46f;
                k *= 1 - 0.45f * MathF.Exp(-1.6f * (mx * mx + my * my));
                var inv = sq > 1e-9f ? 1 / sq : 0;
                float L = ground.L + (la * inv - ground.L) * k, A = ground.A + (aa * inv - ground.A) * k, B = ground.B + (ba * inv - ground.B) * k;
                float l = L + 0.39633778f * A + 0.21580376f * B, m = L - 0.10556135f * A - 0.06385417f * B, s = L - 0.08948418f * A - 1.29148555f * B;
                l = l * l * l; m = m * m * m; s = s * s * s;
                var dither = Noise(px, py);
                var o = (py * W + px) * 4;
                _px[o] = Out(-0.00419609f * l - 0.70341861f * m + 1.70761470f * s, dither);
                _px[o + 1] = Out(-1.26843800f * l + 2.60975740f * m - 0.34131940f * s, dither);
                _px[o + 2] = Out(4.07674166f * l - 3.30771159f * m + 0.23096993f * s, dither);
                _px[o + 3] = 255;
            }
        }
        using (var s = _bmp.PixelBuffer.AsStream()) s.Write(_px, 0, _px.Length);
        _bmp.Invalidate();
    }

    static byte Out(float v, float dither) => (byte)Math.Clamp(255 * (v <= 0.0031308f ? 12.92f * Math.Max(v, 0) : 1.055f * MathF.Pow(v, 1 / 2.4f) - 0.055f) + dither, 0, 255);

    // the same grain each time, so it does not boil
    static float Noise(int x, int y)
    {
        unchecked
        {
            var h = (uint)(x * 374761393 + y * 668265263);
            h = (h ^ (h >> 13)) * 1274126177;
            return (h ^ (h >> 16)) / (float)uint.MaxValue - 0.5f;
        }
    }

    // (as large as it is given: the picture inside asks for nothing)
    protected override Windows.Foundation.Size MeasureOverride(Windows.Foundation.Size available)
    {
        base.MeasureOverride(new Windows.Foundation.Size(1, 1));
        return new Windows.Foundation.Size(0, 0);
    }

    // shows and moves it (still when Windows' animation effects are off), or hides it and stops
    public void Run(bool show)
    {
        Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        if (show) Draw(_clock.Elapsed.TotalSeconds);
        var move = show && new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;
        if (move == _on) return;
        _on = move;
        if (move) { _clock.Start(); _tick.Start(); }
        else { _clock.Stop(); _tick.Stop(); }
    }
}
