using System.Globalization;
using System.Numerics;
using System.Text;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;
using static ApexSetup.Design.Scrap;
using Shape = Microsoft.UI.Xaml.Shapes.Path;

namespace ApexSetup;

// The scrapbook's frames, drawn the same way on the board and small in Customise. Each is a handful of shapes (paths, gradients,
// one system shadow), so a board of hundreds of pictures stays light. Grain, fibres and perforations are one path per card.
public sealed partial class MainWindow
{
    // What a frame made: the card, the tag its caption sits on (if any), whether it is taped on, whether its caption lies on the board
    sealed record Dressed(FrameworkElement Body, FrameworkElement? CapBox, bool Tape, bool CaptionOnBoard);

    // photo(width, height, corner) gives the picture; k is the board's scale; seed varies washi, doodles and stamps per picture
    static Dressed Dress(string frame, Func<double, double, double, FrameworkElement> photo, double w, double h, double k, int style, int seed,
        Brush ink, Color accent, TextBlock caption, bool bare, bool shadows = true)
    {
        var a = w / 150;   // (the drawings' unit: a picture 150 wide)
        var rnd = new Random(seed);
        var stack = new StackPanel();
        Brush S(Color c) => new SolidColorBrush(c);
        T Lift<T>(T e) where T : UIElement { if (shadows) e.Shadow = new ThemeShadow(); return e; }
        switch (frame)
        {
            case "rounded":
            {
                stack.Children.Add(photo(w, h, 20));
                caption.Foreground = S(Design.Scrap.Ink);
                var tag = new Border { Child = caption, Margin = new Thickness(10, -18, 0, 0), Padding = new Thickness(12, 2, 12, 2), CornerRadius = new CornerRadius(12), HorizontalAlignment = HorizontalAlignment.Left,
                    Background = S(Color.FromArgb(0xEB, PaperTop.R, PaperTop.G, PaperTop.B)) };
                stack.Children.Add(tag);
                return new(Lift(stack), tag, false, false);
            }
            case "print":
            {
                // (a glossy print: a thin white border, a diagonal sheen over the picture, the caption written on the board)
                var pic = new Grid { Width = w, Height = h };
                pic.Children.Add(photo(w, h, 3));
                pic.Children.Add(new Border { CornerRadius = new CornerRadius(3), IsHitTestVisible = false, Background = Linear(new(0, 0), new(1, 1), (0.25, Clear), (0.42, Sheen), (0.5, SheenFaint), (0.62, Clear)) });
                stack.Children.Add(Lift(new Border { BorderBrush = S(Print), BorderThickness = new Thickness(6), CornerRadius = new CornerRadius(8), Child = pic }));
                caption.Foreground = ink;
                caption.Margin = new Thickness(4, 10, 0, 0);
                stack.Children.Add(caption);
                return new(stack, null, false, true);
            }
            case "washi":
            {
                // (a white border held by washi tape: a long strip and a short one, their places, colours and patterns changing from picture to picture)
                var g = new Grid();
                g.Children.Add(Lift(new Border { Background = S(White), Padding = new Thickness(8), Child = photo(w, h, 0) }));
                if (!bare)
                {
                    var (th, span) = (24 * k, w + 16);
                    var first = rnd.Next(Washi.Length);
                    var second = (first + 1 + rnd.Next(Washi.Length - 1)) % Washi.Length;
                    switch (rnd.Next(3))
                    {
                        case 0:
                            g.Children.Add(WashiTape(span * 0.55, th, first, rnd, HorizontalAlignment.Center, VerticalAlignment.Top, rnd.Next(-6, 7), span * (rnd.NextDouble() * 0.2 - 0.1), -th * 0.5));
                            g.Children.Add(WashiTape(span * 0.26, th * 0.7, second, rnd, HorizontalAlignment.Left, VerticalAlignment.Bottom, rnd.Next(18, 32), -th * 0.4, th * 0.3));
                            break;
                        case 1:
                            g.Children.Add(WashiTape(span * 0.5, th, first, rnd, HorizontalAlignment.Left, VerticalAlignment.Top, rnd.Next(-8, 3), span * 0.08, -th * 0.5));
                            g.Children.Add(WashiTape(span * 0.26, th * 0.7, second, rnd, HorizontalAlignment.Right, VerticalAlignment.Bottom, -rnd.Next(18, 32), th * 0.4, th * 0.3));
                            break;
                        default:
                            g.Children.Add(WashiTape(span * 0.32, th, first, rnd, HorizontalAlignment.Left, VerticalAlignment.Top, -35, -th * 0.6, -th * 0.1));
                            g.Children.Add(WashiTape(span * 0.32, th, second, rnd, HorizontalAlignment.Right, VerticalAlignment.Bottom, -35, th * 0.6, th * 0.1));
                            break;
                    }
                }
                stack.Children.Add(g);
                caption.Foreground = ink;
                caption.Margin = new Thickness(4, 12, 0, 0);
                stack.Children.Add(caption);
                return new(stack, null, false, true);
            }
            case "clip":
            {
                // (a white print and a steel paper clip over its top edge: a wire with its shadow, its body and a highlight)
                var card = new StackPanel { Background = PaperBrush(), Padding = new Thickness(9, 9, 9, 6) };
                card.Children.Add(photo(w, h, 0));
                caption.Foreground = S(Design.Scrap.Ink);
                caption.Margin = new Thickness(3, 8, 0, 2);
                card.Children.Add(caption);
                var g = new Grid();
                g.Children.Add(Lift(card));
                var s = 1.5 * k;
                const string Gem = "M4 60 L4 16 A6 6 0 0 1 16 16 L16 70 A8 8 0 0 1 0 70 L0 10 A10 10 0 0 1 20 10 L20 54";
                Shape Wire(Brush b, double t, double dx, double dy) => new() { Data = Geo(Gem), Stroke = b, StrokeThickness = t, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
                    RenderTransform = new TranslateTransform { X = dx, Y = dy } };
                g.Children.Add(new Grid
                {
                    HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0.2 * w, -30 * s, 0, 0), IsHitTestVisible = false,
                    RenderTransform = new CompositeTransform { ScaleX = s, ScaleY = s, Rotation = 8 },
                    Children = { Wire(S(WireShadow), 3, 1.5, 2.5), Wire(Linear(new(0, 0), new(1, 0), (0, SteelDark), (0.45, SteelLight), (0.6, SteelMid), (1, SteelEdge)), 2.6, 0, 0), Wire(S(WireLight), 0.7, -0.5, -0.4) },
                });
                return new(g, null, false, false);
            }
            case "kraft":
            {
                // (brown paper with dark fibres and light flecks, a pressed line inside its edge, taped on)
                var g = new Grid();
                var (pw, ph) = (w + 24, h + 61);
                g.Children.Add(new Shape { Data = Geo(Specks(pw, ph, 40, seed, 0.4 * k, 1.1 * k)), Fill = S(KraftFibre), Opacity = 0.45, Margin = new Thickness(-12, -12, -12, -13), IsHitTestVisible = false });
                g.Children.Add(new Shape { Data = Geo(Specks(pw, ph, 24, seed + 1, 0.4 * k, 0.9 * k)), Fill = S(KraftFleck), Opacity = 0.5, Margin = new Thickness(-12, -12, -12, -13), IsHitTestVisible = false });
                g.Children.Add(new Border { Margin = new Thickness(-6, -6, -6, -7), BorderBrush = S(KraftLine), BorderThickness = new Thickness(1), IsHitTestVisible = false });
                stack.Children.Add(photo(w, h, 0));
                caption.Foreground = S(KraftInk);
                caption.Margin = new Thickness(2, 8, 2, 0);
                stack.Children.Add(caption);
                g.Children.Add(stack);
                return new(Lift(new Border { Background = Linear(new(0, 0), new(0.4, 1), (0, KraftTop), (1, KraftBottom)), Padding = new Thickness(12, 12, 12, 13), Child = g }), null, true, false);
            }
            case "deckle":
            {
                // (an old print: a cream border scalloped all round, age spots, the picture in sepia with a dark vignette)
                var g = new Grid();
                var edge = new Shape { Fill = Linear(new(0, 0), new(0.3, 1), (0, CreamTop), (1, CreamBottom)) };
                var spots = new Shape { Fill = S(AgeSpot), Opacity = 0.18, IsHitTestVisible = false };
                var r = 2.6 * Math.Max(0.6, k);
                g.SizeChanged += (_, e) =>
                {
                    var (gw, gh) = (e.NewSize.Width, e.NewSize.Height);
                    edge.Data = Geo(Scallop(r, r, gw - 2 * r, gh - 2 * r, r, false));
                    spots.Data = Geo(Specks(gw, gh, 14, seed, 0.8 * k, 2.2 * k));
                };
                g.Children.Add(new Canvas { IsHitTestVisible = false, Children = { edge, spots } });   // (in a canvas: the edge drawn to the card's size never changes that size)
                var tinted = new Grid { Width = w, Height = h };
                tinted.Children.Add(photo(w, h, 0));
                tinted.Children.Add(new Border { Background = S(Sepia), IsHitTestVisible = false });
                tinted.Children.Add(new Border { IsHitTestVisible = false, Background = new RadialGradientBrush { RadiusX = 0.75, RadiusY = 0.75,
                    GradientStops = { new GradientStop { Color = Clear, Offset = 0.55 }, new GradientStop { Color = Vignette, Offset = 1 } } } });
                stack.Padding = new Thickness(14, 14, 14, 10);
                stack.Children.Add(tinted);
                caption.Foreground = S(SepiaInk);
                caption.Margin = new Thickness(2, 6, 0, 0);
                stack.Children.Add(caption);
                g.Children.Add(stack);
                return new(Lift(g), null, false, false);
            }
            case "doodle":
            {
                // (no paper: a border sketched twice by hand round the picture, and a sparkle, a small one and a heart, in the board's own ink)
                var g = new Grid { Padding = new Thickness(14), Width = w + 28, Height = h + 28 };
                g.Children.Add(photo(w, h, 0));
                var j = Math.Max(1, a);
                g.Children.Add(new Shape { Data = Geo(Sketch(5, 5, w + 18, h + 18, seed, 1.4 * j, 5 * j)), Stroke = ink, StrokeThickness = 2, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round, Margin = new Thickness(-14), IsHitTestVisible = false });
                g.Children.Add(new Shape { Data = Geo(Sketch(8, 8, w + 12, h + 12, seed + 7, 1.6 * j, 3 * j)), Stroke = ink, StrokeThickness = 1.1, Opacity = 0.55, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round, Margin = new Thickness(-14), IsHitTestVisible = false });
                var corners = Enumerable.Range(0, 4).OrderBy(_ => rnd.Next()).ToArray();
                (string D, double Size, bool Filled, double Opacity)[] marks = [(Sparkle, 18 * k, true, 1), (Sparkle, 11 * k, true, 0.7), (Heart, 15 * k, false, 1)];
                var shown = rnd.Next(3) == 0 ? 2 : 3;
                for (int m = 0; m < shown; m++)
                {
                    var (d, size, filled, op) = marks[m];
                    var c = corners[m];
                    var o = -size * 0.95 - 4;
                    g.Children.Add(new Shape
                    {
                        Data = Geo(d), Width = size, Height = size, Stretch = Stretch.Uniform, Opacity = op, IsHitTestVisible = false,
                        Fill = filled ? ink : null, Stroke = filled ? null : ink, StrokeThickness = filled ? 0 : 1.4, StrokeLineJoin = PenLineJoin.Round,
                        HorizontalAlignment = c % 2 == 0 ? HorizontalAlignment.Left : HorizontalAlignment.Right, VerticalAlignment = c < 2 ? VerticalAlignment.Top : VerticalAlignment.Bottom,
                        Margin = new Thickness(c % 2 == 0 ? o : 0, c < 2 ? o : 0, c % 2 == 0 ? 0 : o, c < 2 ? 0 : o),
                    });
                }
                stack.Children.Add(g);
                caption.Foreground = ink;
                caption.HorizontalAlignment = HorizontalAlignment.Center;
                caption.Margin = new Thickness(0, 4, 0, 0);
                stack.Children.Add(caption);
                return new(stack, null, false, true);
            }
            case "stamp":
            {
                // (an airmail stamp: perforated paper, a border of slanted stripes in one of a few colour pairs, the picture and its caption)
                var g = new Grid();
                var perf = new Shape { Fill = S(StampPaper) };
                var r = 3 * Math.Max(0.7, k);
                g.SizeChanged += (_, e) => perf.Data = Geo(Scallop(0, 0, e.NewSize.Width, e.NewSize.Height, r, true));
                g.Children.Add(new Canvas { IsHitTestVisible = false, Children = { perf } });
                var (c1, c2) = Airmail[rnd.Next(Airmail.Length)];
                var band = 14 * Math.Max(0.7, k);
                var stripes = Bands(w + 80, h + 140, band * Math.Sqrt(2), rnd.Next(2) == 0 ? (1, 1) : (1, -1), (c1, 0.25), (StampPaper, 0.25), (c2, 0.25), (StampPaper, 0.25));
                stack.Children.Add(photo(w, h, 0));
                caption.Foreground = S(Design.Scrap.Ink);
                caption.Margin = new Thickness(2, 6, 2, 0);
                stack.Children.Add(caption);
                g.Children.Add(new Border { Margin = new Thickness(r + 5 * k), Background = stripes, Padding = new Thickness(5 * k), Child = new Border { Background = S(StampPaper), Padding = new Thickness(5, 5, 5, 3), Child = stack } });
                return new(Lift(g), null, false, false);
            }
            case "film":
            {
                // (a strip of 35 mm film: dark base with real sprocket holes, the frame numbers printed on its edge, the caption on the board)
                var (band, side) = (20 * a, 12 * a);
                var (fw, fh) = (w + 2 * side, h + 2 * band);
                var (hw, hh, rr) = (7 * a, 6 * a, 1.2 * a);
                var d = new StringBuilder("F0 ").Append(CultureInfo.InvariantCulture, $"M0 0 H{fw:0.#} V{fh:0.#} H0Z");
                void Hole(double x, double y) => d.Append(CultureInfo.InvariantCulture, $" M{x + rr:0.##} {y:0.##} H{x + hw - rr:0.##} A{rr:0.##} {rr:0.##} 0 0 1 {x + hw:0.##} {y + rr:0.##} V{y + hh - rr:0.##} A{rr:0.##} {rr:0.##} 0 0 1 {x + hw - rr:0.##} {y + hh:0.##} H{x + rr:0.##} A{rr:0.##} {rr:0.##} 0 0 1 {x:0.##} {y + hh - rr:0.##} V{y + rr:0.##} A{rr:0.##} {rr:0.##} 0 0 1 {x + rr:0.##} {y:0.##}Z");
                for (double x = side * 0.5; x + hw < fw - side * 0.3; x += 13 * a) { Hole(x, band * 0.3); Hole(x, fh - band * 0.3 - hh); }
                var g = new Grid { Width = fw, Height = fh };
                g.Children.Add(new Shape { Data = Geo(d.ToString()), Fill = Linear(new(0, 0), new(0, 1), (0, FilmEdge), (0.5, FilmBase), (1, FilmEdge)) });
                var pic = photo(w, h, 0);
                pic.Margin = new Thickness(side, band, side, band);
                g.Children.Add(pic);
                TextBlock Edge(string text, bool top) => new()
                {
                    Text = text, FontSize = Design.TextSize.Micro, FontWeight = FontWeights.SemiBold, CharacterSpacing = 60, Foreground = S(FilmPrint), IsHitTestVisible = false,
                    HorizontalAlignment = top ? HorizontalAlignment.Left : HorizontalAlignment.Right, VerticalAlignment = top ? VerticalAlignment.Top : VerticalAlignment.Bottom,
                    Margin = top ? new Thickness(side, band * 0.3 + hh + a, 0, 0) : new Thickness(0, 0, side, band * 0.3 + hh + a),
                    RenderTransform = new ScaleTransform { ScaleX = 0.5 * a, ScaleY = 0.5 * a }, RenderTransformOrigin = top ? new(0, 0) : new(1, 1),
                };
                var frameNo = 12 + style % 24;
                g.Children.Add(Edge($"APEX 400   ▸ {frameNo}A", true));
                g.Children.Add(Edge($"{frameNo + 1} ▸", false));
                stack.Children.Add(Lift(g));
                caption.Foreground = ink;
                caption.Margin = new Thickness(4, 10, 0, 0);
                stack.Children.Add(caption);
                return new(stack, null, false, true);
            }
            case "walnut":
            case "gallery":
            {
                // (a real picture frame: a mitred moulding lit from the top left, a bevelled mat, the lip's shadow falling inwards; walnut
                // with its grain, or a thin black gallery frame with a gilt fillet and a wide mat. The caption is written on the board.)
                var wood = frame == "walnut";
                var (t, m, bev) = wood ? (15 * a, 10 * a, 3 * a) : (10 * a, 17 * a, 2.6 * a);
                var (fw, fh) = (w + 2 * (t + m), h + 2 * (t + m));
                var g = new Grid { Width = fw, Height = fh };
                Shape Flat(string d, Brush b) => new() { Data = Geo(d), Fill = b, IsHitTestVisible = false };
                // (the four sides of the moulding, each with the profile across it and its share of light)
                (string D, Windows.Foundation.Point From, Windows.Foundation.Point To, Color Light)[] sides =
                [
                    ($"M0 0 H{F(fw)} L{F(fw - t)} {F(t)} H{F(t)}Z", new(0, 0), new(0, t), SideLight),
                    ($"M0 0 L{F(t)} {F(t)} V{F(fh - t)} L0 {F(fh)}Z", new(0, 0), new(t, 0), SideFaint),
                    ($"M{F(fw)} 0 V{F(fh)} L{F(fw - t)} {F(fh - t)} V{F(t)}Z", new(fw, 0), new(fw - t, 0), SideShade),
                    ($"M0 {F(fh)} H{F(fw)} L{F(fw - t)} {F(fh - t)} H{F(t)}Z", new(0, fh), new(0, fh - t), SideDeep),
                ];
                foreach (var (d, from, to, light) in sides)
                {
                    var profile = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = from, EndPoint = to };
                    foreach (var (at, c) in wood ? Walnut : Ebony) profile.GradientStops.Add(new GradientStop { Color = c, Offset = at });
                    g.Children.Add(Flat(d, profile));
                    g.Children.Add(Flat(d, S(light)));
                }
                if (wood)
                {
                    // (the grain runs along each side)
                    var grain = new StringBuilder();
                    for (int i = 0; i < 9; i++)
                    {
                        var (y, x) = (t * (0.1 + 0.8 * rnd.NextDouble()), t * (0.1 + 0.8 * rnd.NextDouble()));
                        grain.Append($"M{F(y)} {F(y)} Q{F(fw / 2)} {F(y + (rnd.NextDouble() - 0.5) * 3 * a)} {F(fw - y)} {F(y)} ");
                        grain.Append($"M{F(x)} {F(fh - x)} Q{F(fw / 2)} {F(fh - x + (rnd.NextDouble() - 0.5) * 3 * a)} {F(fw - x)} {F(fh - x)} ");
                        grain.Append($"M{F(x)} {F(x)} Q{F(x + (rnd.NextDouble() - 0.5) * 3 * a)} {F(fh / 2)} {F(x)} {F(fh - x)} ");
                        grain.Append($"M{F(fw - y)} {F(y)} Q{F(fw - y + (rnd.NextDouble() - 0.5) * 3 * a)} {F(fh / 2)} {F(fw - y)} {F(fh - y)} ");
                    }
                    g.Children.Add(new Shape { Data = Geo(grain.ToString()), Stroke = S(WalnutGrain), StrokeThickness = 0.5 * a, IsHitTestVisible = false });
                }
                g.Children.Add(new Shape { Data = Geo($"M0 0 L{F(t)} {F(t)} M{F(fw)} 0 L{F(fw - t)} {F(t)} M0 {F(fh)} L{F(t)} {F(fh - t)} M{F(fw)} {F(fh)} L{F(fw - t)} {F(fh - t)}"),
                    Stroke = S(Seam), StrokeThickness = 0.6 * a, IsHitTestVisible = false });
                // (the mat, its bevelled window round the picture, and the shadow of the frame's lip on it)
                g.Children.Add(new Border { Margin = new Thickness(t), Background = S(wood ? MatWarm : MatCool), IsHitTestVisible = false });
                var (x0, y0, x1, y1) = (t + m, t + m, t + m + w, t + m + h);
                g.Children.Add(Flat($"M{F(x0 - bev)} {F(y0 - bev)} H{F(x1 + bev)} L{F(x1)} {F(y0)} H{F(x0)}Z", S(BevelTop)));
                g.Children.Add(Flat($"M{F(x0 - bev)} {F(y0 - bev)} L{F(x0)} {F(y0)} V{F(y1)} L{F(x0 - bev)} {F(y1 + bev)}Z", S(BevelLeft)));
                g.Children.Add(Flat($"M{F(x1 + bev)} {F(y0 - bev)} V{F(y1 + bev)} L{F(x1)} {F(y1)} V{F(y0)}Z", S(BevelRight)));
                g.Children.Add(Flat($"M{F(x0 - bev)} {F(y1 + bev)} H{F(x1 + bev)} L{F(x1)} {F(y1)} H{F(x0)}Z", S(BevelBottom)));
                var pic = photo(w, h, 0);
                pic.Margin = new Thickness(x0, y0, x0, y0);
                g.Children.Add(pic);
                g.Children.Add(new Border { Margin = new Thickness(t, t, t, 0), Height = 7 * a, VerticalAlignment = VerticalAlignment.Top, IsHitTestVisible = false, Background = Linear(new(0, 0), new(0, 1), (0, LipShadow), (1, Clear)) });
                g.Children.Add(new Border { Margin = new Thickness(t, t, 0, t), Width = 6 * a, HorizontalAlignment = HorizontalAlignment.Left, IsHitTestVisible = false, Background = Linear(new(0, 0), new(1, 0), (0, LipShadow), (1, Clear)) });
                stack.Children.Add(Lift(g));
                caption.Foreground = ink;
                caption.HorizontalAlignment = HorizontalAlignment.Center;
                caption.Margin = new Thickness(0, 10, 0, 0);
                stack.Children.Add(caption);
                return new(stack, null, false, true);
            }
            case "neon":
            {
                // (a neon tube round the picture in the app's accent: a wide faint halo, a narrower one, the tube and its white-hot core)
                var (gap, radius) = (9 * k, 12 * k);
                var g = new Grid { Padding = new Thickness(gap + 6 * k) };
                g.Children.Add(photo(w, h, 6));
                var hot = Shade(accent, 0.75);
                foreach (var (t, c) in new[] { (12 * k, Color.FromArgb(0x1C, accent.R, accent.G, accent.B)), (6 * k, Color.FromArgb(0x40, accent.R, accent.G, accent.B)), (2.6 * k, accent), (0.9 * k, hot) })
                    g.Children.Add(new Border { Margin = new Thickness(-(gap + t / 2)), CornerRadius = new CornerRadius(radius + t / 2), BorderThickness = new Thickness(t), BorderBrush = S(c), IsHitTestVisible = false });
                stack.Children.Add(g);
                caption.Foreground = S(accent);
                caption.HorizontalAlignment = HorizontalAlignment.Center;
                caption.Margin = new Thickness(0, 4, 0, 0);
                stack.Children.Add(caption);
                return new(stack, null, false, true);
            }
            default:
            {
                // (an instant print: warm white paper, a fine line round the picture, a wide bottom for the caption, taped on)
                var pic = new Grid();
                pic.Children.Add(photo(w, h, 0));
                pic.Children.Add(new Border { BorderBrush = S(PhotoEdge), BorderThickness = new Thickness(1), IsHitTestVisible = false });
                stack.Children.Add(pic);
                caption.Foreground = S(Design.Scrap.Ink);
                caption.Margin = new Thickness(2, 8, 2, 0);
                stack.Children.Add(caption);
                return new(Lift(new Border { Background = PaperBrush(), Padding = new Thickness(11, 11, 11, 12), CornerRadius = new CornerRadius(1.5), Child = stack }), null, true, false);
            }
        }
    }

    static LinearGradientBrush Linear(Windows.Foundation.Point from, Windows.Foundation.Point to, params (double At, Color Color)[] stops)
    {
        var b = new LinearGradientBrush { StartPoint = from, EndPoint = to };
        foreach (var (at, c) in stops) b.GradientStops.Add(new GradientStop { Color = c, Offset = at });
        return b;
    }
    // Stripes across a w by h area: one period of colours (each with its share) along a direction, repeated by hand with plain
    // stops. GradientSpreadMethod.Repeat is never used: a repeating gradient left other parts of the window drawn half-faded.
    static LinearGradientBrush Bands(double w, double h, double period, (double X, double Y) dir, params (Color Color, double Share)[] pattern)
    {
        var len = Math.Sqrt(dir.X * dir.X + dir.Y * dir.Y);
        var (dx, dy) = (dir.X / len, dir.Y / len);
        double[] across = [0, w * dx, h * dy, w * dx + h * dy];
        var (from, to) = (across.Min(), across.Max());
        var b = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new(from * dx, from * dy), EndPoint = new(to * dx, to * dy) };
        var span = to - from;
        for (double t = 0; t < span; t += period)
        {
            var at = t;
            foreach (var (c, share) in pattern)
            {
                b.GradientStops.Add(new GradientStop { Color = c, Offset = Math.Min(1, at / span) });
                at += period * share;
                b.GradientStops.Add(new GradientStop { Color = c, Offset = Math.Min(1, at / span) });
            }
        }
        return b;
    }
    static LinearGradientBrush PaperBrush() => Linear(new(0, 0), new(0.3, 1), (0, PaperTop), (1, PaperBottom));

    // a strip of washi tape with torn ends, in one of three patterns, turned and moved from where it is aligned
    static Grid WashiTape(double len, double th, int colour, Random rnd, HorizontalAlignment h, VerticalAlignment v, double angle, double dx, double dy)
    {
        var (c1, c2) = Washi[colour];
        var g = new Grid { Width = len, Height = th, Opacity = 0.86, HorizontalAlignment = h, VerticalAlignment = v, IsHitTestVisible = false,
            RenderTransform = new CompositeTransform { Rotation = angle, TranslateX = dx, TranslateY = dy }, RenderTransformOrigin = new(0.5, 0.5) };
        var shape = Torn(len, th, rnd);
        Shape Layer(Brush b) => new() { Data = Geo(shape), Fill = b };
        var step = th * 0.5;
        switch (rnd.Next(3))
        {
            case 0: g.Children.Add(Layer(Bands(len, th, step * Math.Sqrt(2), (1, 1), (c1, 0.5), (c2, 0.5)))); break;   // (diagonal stripes)
            case 1:   // (gingham: see-through bands both ways over the pale colour, darker where they cross)
                var see = Color.FromArgb(0x90, c1.R, c1.G, c1.B);
                g.Children.Add(Layer(new SolidColorBrush(c2)));
                g.Children.Add(Layer(Bands(len, th, step, (1, 0), (see, 0.5), (Clear, 0.5))));
                g.Children.Add(Layer(Bands(len, th, step, (0, 1), (see, 0.5), (Clear, 0.5))));
                break;
            default: g.Children.Add(Layer(Bands(len, th, step * 0.6, (1, 0), (c1, 0.25), (c2, 0.75)))); break;   // (pinstripes)
        }
        return g;
    }

    static string F(double v) => v.ToString("0.##", CultureInfo.InvariantCulture);

    // a tape's outline: straight sides, ragged short ends
    static string Torn(double len, double th, Random rnd)
    {
        var s = new StringBuilder($"M3 0 L{F(len - 3)} 0");
        for (int i = 1; i <= 5; i++) s.Append($" L{F(len - 3 + (i % 2 == 1 ? 2.4 : -0.6) * (rnd.NextDouble() + 0.4))} {F(th * i / 5)}");
        s.Append($" L3 {F(th)}");
        for (int i = 4; i >= 1; i--) s.Append($" L{F(3 + (i % 2 == 1 ? -2.4 : 0.6) * (rnd.NextDouble() + 0.4))} {F(th * i / 5)}");
        return s.Append('Z').ToString();
    }

    // dots scattered over a size: fibres, flecks, age spots (one figure each, one path for all)
    static string Specks(double w, double h, int n, int seed, double r0, double r1)
    {
        var rnd = new Random(seed);
        var s = new StringBuilder();
        for (int i = 0; i < n; i++)
        {
            var (x, y, r) = (rnd.NextDouble() * w, rnd.NextDouble() * h, r0 + rnd.NextDouble() * (r1 - r0));
            s.Append($"M{F(x - r)} {F(y)} A{F(r)} {F(r)} 0 1 0 {F(x + r)} {F(y)} A{F(r)} {F(r)} 0 1 0 {F(x - r)} {F(y)}Z ");
        }
        return s.Length == 0 ? "M0 0" : s.ToString();
    }

    // a rectangle edged with half-circles: bumps outwards (a deckled print) or notches inwards (a stamp's perforation)
    static string Scallop(double x, double y, double w, double h, double r, bool inward)
    {
        if (w <= 2 * r || h <= 2 * r) return "M0 0";
        var sweep = inward ? 0 : 1;
        var (n, m) = (Math.Max(1, (int)Math.Round(w / (2 * r))), Math.Max(1, (int)Math.Round(h / (2 * r))));
        var (sx, sy) = (w / n, h / m);
        var s = new StringBuilder($"M{F(x)} {F(y)}");
        for (int i = 1; i <= n; i++) s.Append($" A{F(sx / 2)} {F(r)} 0 0 {sweep} {F(x + sx * i)} {F(y)}");
        for (int i = 1; i <= m; i++) s.Append($" A{F(r)} {F(sy / 2)} 0 0 {sweep} {F(x + w)} {F(y + sy * i)}");
        for (int i = 1; i <= n; i++) s.Append($" A{F(sx / 2)} {F(r)} 0 0 {sweep} {F(x + w - sx * i)} {F(y + h)}");
        for (int i = 1; i <= m; i++) s.Append($" A{F(r)} {F(sy / 2)} 0 0 {sweep} {F(x)} {F(y + h - sy * i)}");
        return s.Append('Z').ToString();
    }

    // a rectangle drawn by hand: four strokes that bow a little and run past the corners
    static string Sketch(double x, double y, double w, double h, int seed, double jitter, double over)
    {
        var rnd = new Random(seed);
        double J() => (rnd.NextDouble() - 0.5) * 2 * jitter;
        (double X, double Y)[] p =
        [
            (x - over * rnd.NextDouble(), y + J()), (x + w + over * rnd.NextDouble(), y + J()),
            (x + w + J(), y - over * rnd.NextDouble()), (x + w + J(), y + h + over * rnd.NextDouble()),
            (x + w + over * rnd.NextDouble(), y + h + J()), (x - over * rnd.NextDouble(), y + h + J()),
            (x + J(), y + h + over * rnd.NextDouble()), (x + J(), y - over * rnd.NextDouble()),
        ];
        var s = new StringBuilder();
        for (int i = 0; i < 8; i += 2)
        {
            var (s0, s1) = (p[i], p[i + 1]);
            s.Append($"M{F(s0.X)} {F(s0.Y)} Q{F((s0.X + s1.X) / 2 + J() * 1.4)} {F((s0.Y + s1.Y) / 2 + J() * 1.4)} {F(s1.X)} {F(s1.Y)} ");
        }
        return s.ToString();
    }

    const string Sparkle = "M7 0 C8 5 9 6 14 7 C9 8 8 9 7 14 C6 9 5 8 0 7 C5 6 6 5 7 0Z";
    const string Heart = "M7 13 C1 8 2 3 5 3 C6 3 7 4 7 5 C7 4 8 3 9 3 C12 3 13 8 7 13Z";

    // a picture's own number for the frames' variety, the same every time it is drawn
    static int Seed(string path)
    {
        unchecked
        {
            var h = (int)2166136261;
            foreach (var ch in path.ToLowerInvariant()) h = (h ^ ch) * 16777619;
            return h & 0x7FFFFFFF;
        }
    }

    // a small drawing of each frame for its card in Customise: the board's own drawing, on a swatch of the album's own board, round a
    // small landscape, so the card shows the frame as it will look
    UIElement FrameIcon(string key, AlbumLook look)
    {
        FrameworkElement Photo(double w, double h, double r) => new Border { Width = w, Height = h, CornerRadius = new CornerRadius(r),
            Background = Linear(new(0, 0), new(0, 1), (0, IconSky), (0.5, IconHorizon), (0.56, IconField), (1, IconGround)) };
        var cap = new TextBlock { FontSize = Design.TextSize.Title3, MinHeight = 28 };
        var accent = (Res("ApexAccentBrush") as SolidColorBrush)?.Color ?? IconSky;
        var d = Dress(key, Photo, 150, 100, 1, 1, 7, Ink(look), accent, cap, false, shadows: false);
        if (d.CaptionOnBoard) cap.Visibility = Visibility.Collapsed;
        if (d.CapBox is not null) d.CapBox.Visibility = Visibility.Collapsed;   // (the rounded frame's empty caption tag)
        var card = new Grid { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        if (key is not ("doodle" or "neon"))
        {
            // (a soft shadow under the paper: the system shadow is not drawn this small)
            var r = new CornerRadius(key == "rounded" ? 16 : 2);
            card.Children.Add(new Border { Background = new SolidColorBrush(IconShadowFar), CornerRadius = r, Margin = new Thickness(-1, 5, -1, -7) });
            card.Children.Add(new Border { Background = new SolidColorBrush(IconShadowNear), CornerRadius = r, Margin = new Thickness(0, 1, 0, -3) });
        }
        card.Children.Add(d.Body);
        if (d.Tape) card.Children.Add(new Border { Width = 64, Height = 18, Background = TapeBrush(look, 1), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, -10, 0, 0),
            RenderTransform = new RotateTransform { Angle = -3 }, RenderTransformOrigin = new(0.5, 0.5) });
        return new Border { Width = 240, Height = 186, CornerRadius = new CornerRadius(16), Background = BoardBrush(look), Child = card };
    }
}
