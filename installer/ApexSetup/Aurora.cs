using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace ApexSetup;

// The install's background: an aurora as it is usually drawn (domain-warped fractal noise, stretched into vertical curtains,
// a bright lower edge fading upward, green below and the logo's pink and violet above) over a near-black sky. It is drawn at a
// small size on the CPU and scaled up by the image's own smooth filtering, which keeps it soft; a little dither keeps the
// gradients from banding. Moves only while started, and only when Windows' animations are on.
public sealed class Aurora : Grid
{
    const int W = 240, H = 135; // (drawn small and scaled up: soft by construction, and cheap)
    readonly WriteableBitmap _bmp = new(W, H);
    readonly byte[] _px = new byte[W * H * 4];
    readonly DispatcherTimer _tick = new() { Interval = TimeSpan.FromMilliseconds(33) };
    readonly System.Diagnostics.Stopwatch _clock = new();
    bool _on;

    public Aurora()
    {
        IsHitTestVisible = false;
        Background = new SolidColorBrush(Color.FromArgb(255, 5, 6, 7));
        Children.Add(new Image { Source = _bmp, Stretch = Stretch.Fill });
        _tick.Tick += (_, _) => Draw(_clock.Elapsed.TotalSeconds);
        Draw(0);
    }

    // value noise and its fractal sum (smooth, tileless; the hash is a plain integer mix)
    static float Hash(int x, int y)
    {
        unchecked
        {
            var h = (uint)(x * 374761393 + y * 668265263);
            h = (h ^ (h >> 13)) * 1274126177;
            return (h ^ (h >> 16)) / (float)uint.MaxValue;
        }
    }

    static float Noise(float x, float y)
    {
        int xi = (int)MathF.Floor(x), yi = (int)MathF.Floor(y);
        float xf = x - xi, yf = y - yi;
        float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
        float a = Hash(xi, yi), b = Hash(xi + 1, yi), c = Hash(xi, yi + 1), d = Hash(xi + 1, yi + 1);
        return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
    }

    static float Fbm(float x, float y, int octaves = 4)
    {
        float s = 0, amp = 0.5f;
        for (int i = 0; i < octaves; i++) { s += amp * Noise(x, y); x = x * 2.03f + 7.1f; y = y * 2.03f + 3.7f; amp *= 0.5f; }
        return s;
    }

    // one curtain's light at (x, y), height 0 at the top: its lower edge waves along a warped line, the light falls off upward
    static float Curtain(float x, float y, float t, float seed, float line, float reach)
    {
        var warp = Fbm(x * 1.4f + seed, t * 0.05f + seed) - 0.5f;
        var edge = line + 0.10f * (Fbm(x * 1.1f + t * 0.03f + seed, seed) - 0.5f) + 0.06f * warp;
        var d = y - edge; // above the edge: d < 0
        var fall = d < 0 ? MathF.Exp(d / reach) : MathF.Exp(-d * d / 0.006f);
        // the rays: noise stretched along the height, so it reads as folds of a curtain, not clouds
        var rays = Fbm(x * 2.2f + warp * 1.5f + t * 0.03f, y * 0.4f + seed, 2);
        rays = MathF.Max(0, rays - 0.15f) * 1.4f;
        return fall * rays;
    }

    // a curtain's colour by its height: the lower edge green, higher up the logo's pink, the top violet
    static void Tint(float i, float line, float y, ref float r, ref float g, ref float b)
    {
        var up = Math.Clamp((line - y) / 0.38f, 0, 1);
        float cr = 69 + (201 - 69) * up, cg = 224 + (110 - 224) * up, cb = 143 + (214 - 143) * up;
        if (up > 0.6f) { var k = (up - 0.6f) / 0.4f; cr += (126 - cr) * k; cg += (92 - cg) * k; cb += (220 - cb) * k; }
        r += cr * i * 0.8f; g += cg * i * 0.8f; b += cb * i * 0.8f;
    }

    void Draw(double seconds)
    {
        var t = (float)seconds;
        for (int py = 0; py < H; py++)
        {
            var y = py / (float)H;
            for (int px = 0; px < W; px++)
            {
                var x = px / (float)W * 1.78f; // (the window's shape: the noise is not squashed)
                var a = Curtain(x, y, t, 0f, 0.46f, 0.30f);
                var b = Curtain(x + 3.3f, y, t * 0.8f, 11f, 0.34f, 0.26f) * 0.9f;
                float r = 5, g = 6, bl = 7;
                Tint(a, 0.46f, y, ref r, ref g, ref bl);
                Tint(b, 0.34f, y, ref r, ref g, ref bl);
                // fade out under the card, and a fixed dither of a fraction of a step against banding
                var keep = Math.Clamp((0.9f - y) / 0.35f, 0, 1);
                var dither = Hash(px, py) - 0.5f;
                var o = (py * W + px) * 4;
                _px[o] = (byte)Math.Clamp(7 + (bl - 7) * keep + dither, 0, 255);
                _px[o + 1] = (byte)Math.Clamp(6 + (g - 6) * keep + dither, 0, 255);
                _px[o + 2] = (byte)Math.Clamp(5 + (r - 5) * keep + dither, 0, 255);
                _px[o + 3] = 255;
            }
        }
        Blur(_px); Blur(_px); Blur(_px); Blur(_px);
        using (var s = _bmp.PixelBuffer.AsStream()) s.Write(_px, 0, _px.Length);
        _bmp.Invalidate();
    }

    // a 3x3 box blur of the colour channels, in place (keeps the scaled-up picture free of blocks)
    readonly byte[] _tmp = new byte[W * H * 4];
    void Blur(byte[] p)
    {
        Buffer.BlockCopy(p, 0, _tmp, 0, p.Length);
        for (int y = 1; y < H - 1; y++)
            for (int x = 1; x < W - 1; x++)
                for (int c = 0; c < 3; c++)
                {
                    int sum = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) sum += _tmp[((y + dy) * W + x + dx) * 4 + c];
                    p[(y * W + x) * 4 + c] = (byte)(sum / 9);
                }
    }

    // shows and moves it (still when Windows' animation effects are off), or hides it and stops
    public void Run(bool show)
    {
        Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        var move = show && new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;
        if (move == _on) return;
        _on = move;
        if (move) { _clock.Start(); _tick.Start(); }
        else { _clock.Stop(); _tick.Stop(); }
    }
}
