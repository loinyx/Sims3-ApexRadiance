using System.Security.Cryptography;

namespace ApexSetup;

// Which of a save's pictures the Saves screen shows. Behind the household: a memory of a place with no Sim, when one is
// easy to tell. The game frames a memory on its Sim, so one with a Sim has the plumbob, a thought bubble or a glow near the
// middle; a view of the town has none of them and is full of detail from edge to edge.
public static class SaveCover
{
    const int Cells = 8, Tries = 120;

    // the best view among the save's memories; null when none is clearly one
    public static SaveItem? Best(SaveContents c, CancellationToken ct = default)
    {
        (double Score, SaveItem Item)? best = null; // (seen on real saves: views score 12 to 22, rooms and parties 13 at most)
        foreach (var i in c.Sample(SaveKind.Memory, Tries))
        {
            ct.ThrowIfCancellationRequested();
            if (SaveContents.Picture(i, 64) is { } p && Score(p) is var s and > 0 && (best is null || s > best.Value.Score)) best = (s, i);
        }
        return best?.Item;
    }

    // up to count memories worth a look: not a black one, and no picture twice
    public static List<SaveItem> Fan(SaveContents c, int count, CancellationToken ct = default)
    {
        var seen = new HashSet<string>();
        var fan = new List<SaveItem>();
        foreach (var i in c.Sample(SaveKind.Memory, count * 5))
        {
            ct.ThrowIfCancellationRequested();
            if (fan.Count == count) break;
            if (SaveContents.Picture(i, 64) is { } p && p.Bgra.Where((_, at) => at % 4 == 1).Average(g => (int)g) >= 45
                && seen.Add(Convert.ToHexString(SHA256.HashData(p.Bgra)))) fan.Add(i);
        }
        return fan;
    }

    // the lots with the most in them, the fullest first (a picture of bare ground packs far smaller)
    public static List<SaveItem> Lots(SaveContents c, int count) => c.Items(SaveKind.LotThumbnail).OrderByDescending(i => i.Size).Take(count).ToList();

    // how much of a view without Sims a picture is; 0: not one
    public static double Score(SavePicture p)
    {
        int w = p.Width, h = p.Height, n = 0, bubble = 0;
        if (w < 32 || h < 32) return 0;
        var sum = new double[Cells * Cells];
        var squares = new double[Cells * Cells];
        var count = new int[Cells * Cells];
        double light = 0;
        for (var y = 0; y < h; y++)
            for (var x = 0; x < w; x++, n++)
            {
                var at = (y * w + x) * 4;
                int b = p.Bgra[at], g = p.Bgra[at + 1], r = p.Bgra[at + 2];
                double l = (2 * r + 5 * g + b) / 8.0;
                light += l;
                var cell = y * Cells / h * Cells + x * Cells / w;
                sum[cell] += l;
                squares[cell] += l * l;
                count[cell]++;
                if (x < w / 8 || x >= w - w / 8 || y >= h - h / 8) continue;
                if (g > 100 && g > 1.6 * r && g > 1.6 * b) return 0;   // the plumbob's green
                if (b > 170 && b > 1.35 * g) return 0;                 // a mood's glow
                if (r > 225 && g > 225 && b > 225) bubble++;           // a thought bubble, or a moment's points
            }
        light /= n;
        if (light is < 50 or > 170 || bubble > n / 300) return 0;
        // (a wall or a lawn fills whole cells with one colour: what counts is how much the quieter part of the picture holds)
        var detail = new double[Cells * Cells];
        for (var i = 0; i < detail.Length; i++) detail[i] = Math.Sqrt(Math.Max(0, squares[i] / count[i] - sum[i] / count[i] * (sum[i] / count[i])));
        Array.Sort(detail);
        return detail[Cells * Cells / 5] is var d and >= 12 ? d : 0;
    }
}
