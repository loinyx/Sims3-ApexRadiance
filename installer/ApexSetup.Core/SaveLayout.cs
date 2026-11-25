namespace ApexSetup;

// Where things go on a save's page: the memories' mosaic, the tiles of what weighs, the points of the timeline.
public static class SaveLayout
{
    // Square cells in columns; every each-th item takes two by two, and later items fill the holes that leaves (near the
    // end none is large: nothing would fill beside it).
    public static List<(int Col, int Row, int Span)> Mosaic(int count, int cols, int each = 11)
    {
        var cells = new List<(int, int, int)>(count);
        var rows = new List<bool[]>();
        bool Taken(int c, int r) => r < rows.Count && rows[r][c];
        var first = 0; // (the rows above are full)
        for (var i = 0; i < count; i++)
        {
            var span = cols > 1 && i % each == 0 && (i == 0 || count - i > cols * 2) ? 2 : 1;
            while (first < rows.Count && rows[first].All(x => x)) first++;
            var (col, row) = (0, first);
            while (col + span > cols || Taken(col, row) || (span == 2 && (Taken(col + 1, row) || Taken(col, row + 1) || Taken(col + 1, row + 1))))
                if (++col >= cols) (col, row) = (0, row + 1);
            while (rows.Count < row + span) rows.Add(new bool[cols]);
            for (var r = row; r < row + span; r++)
                for (var c = col; c < col + span; c++) rows[r][c] = true;
            cells.Add((col, row, span));
        }
        return cells;
    }

    // total shared out in proportion, none under min (when they all fit; else the same for each)
    public static double[] Share(IReadOnlyList<double> weights, double total, double min)
    {
        var n = weights.Count;
        var size = new double[n];
        if (n == 0) return size;
        if (min * n >= total) { Array.Fill(size, total / n); return size; }
        var small = new bool[n];
        while (true)
        {
            var left = total - min * small.Count(x => x);
            var sum = Enumerable.Range(0, n).Where(i => !small[i]).Sum(i => weights[i]);
            var more = false;
            for (var i = 0; i < n; i++) size[i] = small[i] ? min : sum > 0 ? left * weights[i] / sum : left / small.Count(x => !x);
            for (var i = 0; i < n; i++) if (!small[i] && size[i] < min) more = small[i] = true;
            if (!more) return size;
        }
    }

    // Tiles in proportion to what they weigh, largest first: the first on a row of its own, the others beside each other
    // under it, on more rows when the width does not hold them. Nothing is under the minimum: the height grows instead.
    public static List<(double X, double Y, double W, double H)> Tiles(IReadOnlyList<double> weights, double width, double height, double gap, double minW, double minH)
    {
        var tiles = new List<(double, double, double, double)>();
        if (weights.Count == 0 || width <= 0) return tiles;
        var fit = Math.Max(1, (int)((width + gap) / (minW + gap)));
        var rows = new List<double[]> { new[] { weights[0] } };
        rows.AddRange(weights.Skip(1).Chunk(fit));
        var gaps = gap * (rows.Count - 1);
        var heights = Share(rows.Select(r => r.Sum()).ToList(), Math.Max(height - gaps, rows.Count * minH), minH);
        double y = 0;
        for (var r = 0; r < rows.Count; r++)
        {
            double x = 0;
            foreach (var w in Share(rows[r], width - gap * (rows[r].Length - 1), minW)) { tiles.Add((x, y, w, heights[r])); x += w + gap; }
            y += heights[r] + gap;
        }
        return tiles;
    }

    // places along a line, in order and at least gap apart (evenly when there is not room for that)
    public static double[] Spread(IReadOnlyList<double> at, double width, double gap)
    {
        var x = at.ToArray();
        if (x.Length < 2) return x;
        gap = Math.Min(gap, width / (x.Length - 1));
        for (var i = 1; i < x.Length; i++) x[i] = Math.Max(x[i], x[i - 1] + gap);
        x[^1] = Math.Min(x[^1], width);
        for (var i = x.Length - 2; i >= 0; i--) x[i] = Math.Min(x[i], x[i + 1] - gap);
        return x;
    }

    // v of one scale on another: straight between the pairs, the ends beyond them
    public static double Between(IReadOnlyList<double> from, IReadOnlyList<double> to, double v)
    {
        if (v <= from[0]) return to[0];
        for (var i = 1; i < from.Count; i++)
            if (v <= from[i]) return from[i] > from[i - 1] ? to[i - 1] + (to[i] - to[i - 1]) * (v - from[i - 1]) / (from[i] - from[i - 1]) : to[i];
        return to[^1];
    }
}
