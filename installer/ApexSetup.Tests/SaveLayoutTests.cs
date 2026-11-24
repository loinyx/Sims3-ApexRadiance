// Where things go on a save's page (SaveLayout)
using ApexSetup;

static class SaveLayoutTests
{
    public static void Run(Action<bool, string> check)
    {
        foreach (var (n, cols) in new[] { (0, 4), (1, 1), (30, 1), (60, 2), (541, 6), (283, 9) })
        {
            var cells = SaveLayout.Mosaic(n, cols);
            var used = cells.SelectMany(c => Enumerable.Range(0, c.Span * c.Span).Select(k => (c.Col + k % c.Span, c.Row + k / c.Span))).ToList();
            check(cells.Count == n && used.Distinct().Count() == used.Count && used.All(u => u.Item1 >= 0 && u.Item1 < cols)
                  && cells.Count(c => c.Span == 2) == (cols > 1 ? (n + 10) / 11 : 0), $"mosaic of {n} in {cols} columns: none over another, none outside");
        }
        check(SaveLayout.Mosaic(5, 4).SequenceEqual(new[] { (0, 0, 2), (2, 0, 1), (3, 0, 1), (2, 1, 1), (3, 1, 1) }), "mosaic: the small ones sit beside the large one");

        var share = SaveLayout.Share(new[] { 90.0, 9, 1 }, 300, 50);
        check(Math.Abs(share.Sum() - 300) < 1e-6 && share[1] == 50 && share[2] == 50 && share[0] == 200, "share: the small ones take the minimum, the rest the others");
        check(SaveLayout.Share(new[] { 1.0, 2 }, 60, 50).SequenceEqual(new[] { 30.0, 30 }) && SaveLayout.Share(new[] { 0.0, 0 }, 100, 10).SequenceEqual(new[] { 50.0, 50 }) && SaveLayout.Share([], 100, 10).Length == 0,
            "share: no room, no weight, nothing");
        foreach (var w in new[] { 1200.0, 700, 400, 100 })
        {
            var t = SaveLayout.Tiles(new[] { 94.0, 26.6, 23, 13, 0.8 }, w, 490, 8, 138, 130);
            var apart = t.All(a => t.All(b => a == b || a.X + a.W <= b.X + 1e-6 || b.X + b.W <= a.X + 1e-6 || a.Y + a.H <= b.Y + 1e-6 || b.Y + b.H <= a.Y + 1e-6));
            check(t.Count == 5 && apart && t[0] is { X: 0, Y: 0 } && Math.Abs(t[0].W - w) < 1e-6 && t.All(x => x.X + x.W <= w + 1e-6 && x.H >= 130 - 1e-6 && x.W >= Math.Min(w, 138) - 1e-6)
                  && t.Max(x => x.Y + x.H) >= 490 - 1e-6, $"tiles in {w}: apart, inside, none under the minimum");
        }
        check(SaveLayout.Tiles(new[] { 5.0 }, 300, 490, 8, 138, 130).SequenceEqual(new[] { (0.0, 0.0, 300.0, 490.0) }) && SaveLayout.Tiles([], 300, 490, 8, 138, 130).Count == 0, "tiles: one, none");

        bool Good(double[] x, double width, double gap) => x.All(v => v >= -1e-6 && v <= width + 1e-6) && Enumerable.Range(1, Math.Max(0, x.Length - 1)).All(i => x[i] - x[i - 1] >= gap - 1e-6);
        check(Good(SaveLayout.Spread(new[] { 0.0, 1, 2, 500 }, 500, 30), 500, 30) && Good(SaveLayout.Spread(new[] { 0.0, 498, 499, 500 }, 500, 30), 500, 30)
              && Good(SaveLayout.Spread(Enumerable.Repeat(500.0, 100).ToArray(), 500, 30), 500, 500.0 / 99 - 1e-6) && SaveLayout.Spread(new[] { 7.0 }, 500, 30)[0] == 7, "timeline points stay in order, apart and inside");
        check(SaveLayout.Between(new[] { 0.0, 10, 10, 20 }, new[] { 0.0, 100, 130, 200 }, 5) == 50 && SaveLayout.Between(new[] { 0.0, 10 }, new[] { 5.0, 9 }, -3) == 5 && SaveLayout.Between(new[] { 0.0, 10 }, new[] { 5.0, 9 }, 30) == 9
              && SaveLayout.Between(new[] { 4.0 }, new[] { 8.0 }, 6) == 8, "a time between two points sits between them");
    }
}
