using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace ApexSetup;

// Health's crystal: a hexagonal bipyramid with equal points, turning and floating, lit like glass (a key and a fill light,
// a sharp highlight, the back faces seen through the front ones) and coloured by the game's health. Drawn on the CPU as
// 24 polygons: no Direct3D or Win2D in the exe. It stops when hidden or when Windows' animation effects are off.
public sealed class Crystal : Grid
{
    // ready green, needs-you amber, blocked red (the palette chosen for the launcher)
    static readonly Vector3[] Palette = [new(0.2625f, 0.8425f, 0.245f), new(0.9625f, 0.6525f, 0.155f), new(0.9125f, 0.2675f, 0.2125f)];
    static readonly Vector3[] Verts;
    static readonly int[][] Faces;
    static readonly Vector3 L1 = Vector3.Normalize(new(-0.6f, 0.8f, 0.7f)), L2 = Vector3.Normalize(new(0.8f, 0.1f, -0.5f));

    static Crystal()
    {
        // 0 top, 1 bottom, 2..7 the ring
        var v = new List<Vector3> { new(0, 0.86f, 0), new(0, -0.86f, 0) };
        for (int i = 0; i < 6; i++) { var a = (i * 60 + 30) * MathF.PI / 180; v.Add(new(0.42f * MathF.Cos(a), 0, 0.42f * MathF.Sin(a))); }
        Verts = v.ToArray();
        var f = new List<int[]>();
        for (int i = 0; i < 6; i++) { int a = 2 + i, b = 2 + (i + 1) % 6; f.Add([0, b, a]); f.Add([1, a, b]); }
        Faces = f.ToArray();
    }

    readonly Canvas _canvas = new();
    readonly Polygon[] _back = new Polygon[12], _front = new Polygon[12];
    readonly Ellipse _glow = new(), _shadow = new();
    readonly double _size;
    Vector3 _color = Palette[0], _target = Palette[0];
    readonly DateTime _start = DateTime.Now;
    DateTime _last;
    bool _hooked;

    public Crystal(double size)
    {
        _size = size;
        Width = size;
        Height = size * 1.25;
        IsHitTestVisible = false;
        _glow.Width = size * 1.1; _glow.Height = size * 1.1;
        _glow.HorizontalAlignment = HorizontalAlignment.Center;
        _glow.VerticalAlignment = VerticalAlignment.Center;
        _glow.Margin = new Thickness(-size * 0.05, 0, -size * 0.05, size * 0.12);
        _shadow.Width = size * 0.42; _shadow.Height = size * 0.07;
        _shadow.HorizontalAlignment = HorizontalAlignment.Center;
        _shadow.VerticalAlignment = VerticalAlignment.Bottom;
        Children.Add(_glow);
        Children.Add(_shadow);
        Children.Add(_canvas);
        for (int i = 0; i < 12; i++)
        {
            _canvas.Children.Add(_back[i] = new Polygon());
        }
        for (int i = 0; i < 12; i++) _canvas.Children.Add(_front[i] = new Polygon());
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAccessibilityView(this, Microsoft.UI.Xaml.Automation.Peers.AccessibilityView.Raw);
        Loaded += (_, _) => Hook(true);
        Unloaded += (_, _) => Hook(false);
        SetState(HealthState.Ready, animate: false);
        Draw();
    }

    public void SetState(HealthState s, bool animate = true)
    {
        _target = Palette[(int)s];
        if (!animate) _color = _target;
        _glow.Fill = new RadialGradientBrush
        {
            GradientStops = { new GradientStop { Color = ToColor(_target, 0.13f), Offset = 0 }, new GradientStop { Color = ToColor(_target, 0), Offset = 0.7 } },
        };
        _shadow.Fill = new RadialGradientBrush
        {
            GradientStops = { new GradientStop { Color = ToColor(_target, 0.28f), Offset = 0 }, new GradientStop { Color = ToColor(_target, 0), Offset = 0.75 } },
        };
        if (!_hooked) { _color = _target; Draw(); }
    }

    static bool Animations => new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;

    void Hook(bool on)
    {
        on &= Animations;
        if (on == _hooked) return;
        _hooked = on;
        if (on) CompositionTarget.Rendering += OnFrame; else CompositionTarget.Rendering -= OnFrame;
        Draw();
    }

    void OnFrame(object? sender, object e)
    {
        if (DateTime.Now - _last < TimeSpan.FromMilliseconds(33)) return; // (30 frames a second is plenty)
        _last = DateTime.Now;
        if (Visibility != Visibility.Visible || XamlRoot is null) return;
        _color = Vector3.Lerp(_color, _target, 0.12f);
        Draw();
    }

    void Draw()
    {
        var t = (float)(DateTime.Now - _start).TotalSeconds;
        var still = !_hooked;
        var angle = still ? 0.5f : t * 0.75f;
        var bob = still ? 0 : MathF.Cos(t * 2 * MathF.PI / 3.2f) * 0.06f;
        var rot = Matrix4x4.CreateRotationY(angle) * Matrix4x4.CreateRotationX(0.12f); // (seen a little from above)
        var h = (float)(_size * 1.25);
        var scale = h * 0.78f / 1.72f;
        var cx = (float)_size / 2;
        var cy = h * 0.46f;
        var p = Verts.Select(v => Vector3.Transform(v + new Vector3(0, bob, 0), rot)).ToArray();
        // faces sorted far to near: the back ones show through the front ones
        var order = Enumerable.Range(0, 12).OrderBy(i => Faces[i].Sum(k => p[k].Z)).ToArray();
        int bi = 0, fi = 0;
        foreach (var i in order)
        {
            var f = Faces[i];
            var n = Vector3.Normalize(Vector3.Cross(p[f[1]] - p[f[0]], p[f[2]] - p[f[0]]));
            if (Vector3.Dot(n, (p[f[0]] + p[f[1]] + p[f[2]]) / 3) < 0) n = -n; // (outward)
            var front = n.Z > 0;
            var poly = front ? _front[fi++] : _back[bi++];
            var pts = new PointCollection();
            foreach (var k in f) pts.Add(new Windows.Foundation.Point(cx + p[k].X * scale, cy - p[k].Y * scale));
            poly.Points = pts;
            // each face a little lighter at the top: flat faces read as glass instead of plastic
            var c = Shade(n, front);
            poly.Fill = new LinearGradientBrush
            {
                StartPoint = new Windows.Foundation.Point(0, 0), EndPoint = new Windows.Foundation.Point(0, 1),
                GradientStops = { new GradientStop { Color = Scale(c, 1.18f), Offset = 0 }, new GradientStop { Color = Scale(c, 0.82f), Offset = 1 } },
            };
        }
    }

    // the same light as the canvas design: key and fill diffuse, a sharp highlight, a rim, back faces darker
    Color Shade(Vector3 n, bool front)
    {
        if (!front)
        {
            var nb = -n;
            var db = MathF.Max(Vector3.Dot(nb, L1), 0) * 0.8f + MathF.Max(Vector3.Dot(nb, L2), 0) * 0.3f + 0.18f;
            return ToColor(_color * (0.28f + db * 0.3f), 0.92f);
        }
        var view = Vector3.UnitZ;
        var d = MathF.Max(Vector3.Dot(n, L1), 0) * 0.8f + MathF.Max(Vector3.Dot(n, L2), 0) * 0.3f + 0.18f;
        var nh = MathF.Max(Vector3.Dot(n, Vector3.Normalize(L1 + view)), 0);
        // (a flat face carries its highlight whole: kept soft, or the face turns white)
        var s = MathF.Pow(nh, 40) * 0.35f + MathF.Pow(nh, 8) * 0.12f;
        var rim = MathF.Pow(1 - MathF.Max(Vector3.Dot(n, view), 0), 3);
        var c = _color * d + new Vector3(s) + _color * rim * 0.5f + new Vector3(0.85f, 1f, 0.9f) * rim * 0.2f;
        return ToColor(c, 0.84f);
    }

    static Color Scale(Color c, float k) => Color.FromArgb(c.A, (byte)Math.Clamp(c.R * k, 0, 255), (byte)Math.Clamp(c.G * k, 0, 255), (byte)Math.Clamp(c.B * k, 0, 255));

    static Color ToColor(Vector3 c, float a) => Color.FromArgb((byte)(Math.Clamp(a, 0, 1) * 255),
        (byte)(Math.Clamp(c.X, 0, 1) * 255), (byte)(Math.Clamp(c.Y, 0, 1) * 255), (byte)(Math.Clamp(c.Z, 0, 1) * 255));
}
