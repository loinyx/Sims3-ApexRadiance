using Windows.UI;

namespace ApexSetup.Design;

// Colours by lightness, strength and hue as the eye sees them (OKLCH): the same lightness reads as the same lightness in every hue,
// so a theme is only its hues, and every surface, line and text follows.
public static class Oklch
{
    public static Color Of(double l, double c, double h, double alpha = 1)
    {
        // (a colour the screen cannot show: less of it, the same hue and lightness)
        while (c > 0 && Linear(l, c, h) is var t && (Out(t.R) || Out(t.G) || Out(t.B))) c -= 0.004;
        var (r, g, b) = Linear(l, Math.Max(0, c), h);
        return Color.FromArgb((byte)Math.Round(alpha * 255), Byte(r), Byte(g), Byte(b));
        static bool Out(double v) => v is < -0.0005 or > 1.0005;
    }

    static (double R, double G, double B) Linear(double l, double c, double h)
    {
        double a = c * Math.Cos(h * Math.PI / 180), b = c * Math.Sin(h * Math.PI / 180);
        double x = Math.Pow(l + 0.3963377774 * a + 0.2158037573 * b, 3), y = Math.Pow(l - 0.1055613458 * a - 0.0638541728 * b, 3), z = Math.Pow(l - 0.0894841775 * a - 1.2914855480 * b, 3);
        return (4.0767416621 * x - 3.3077115913 * y + 0.2309699292 * z, -1.2684380046 * x + 2.6097574011 * y - 0.3413193965 * z, -0.0041960863 * x - 0.7034186147 * y + 1.7076147010 * z);
    }

    static byte Byte(double v) => (byte)Math.Clamp(Math.Round(255 * (v <= 0.0031308 ? 12.92 * v : 1.055 * Math.Pow(Math.Max(v, 0), 1 / 2.4) - 0.055)), 0, 255);
    static double Back(byte v) => v / 255.0 is var s && s <= 0.04045 ? s / 12.92 : Math.Pow((s + 0.055) / 1.055, 2.4);

    public static double Luminance(Color c) => 0.2126 * Back(c.R) + 0.7152 * Back(c.G) + 0.0722 * Back(c.B);

    // WCAG's contrast of two colours, 1 to 21
    public static double Contrast(Color a, Color b)
    {
        double x = Luminance(a), y = Luminance(b);
        return (Math.Max(x, y) + 0.05) / (Math.Min(x, y) + 0.05);
    }

    // a screen colour's own lightness, strength and hue (the theme that follows Windows' accent)
    public static (double L, double C, double H) From(Color k)
    {
        double r = Back(k.R), g = Back(k.G), b = Back(k.B);
        double x = Math.Cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b), y = Math.Cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b), z = Math.Cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
        double l = 0.2104542553 * x + 0.7936177850 * y - 0.0040720468 * z, a = 1.9779984951 * x - 2.4285922050 * y + 0.4505937099 * z, o = 0.0259040371 * x + 0.7827717662 * y - 0.8086757660 * z;
        return (l, Math.Sqrt(a * a + o * o), (Math.Atan2(o, a) * 180 / Math.PI + 360) % 360);
    }
}
