using ApexSetup;

// The game's content cache: the record list reads and writes back unchanged, and removing an item changes exactly what
// the game's own removal changes (checked against the game on a player's cache; here on a made-up one).
static class DcCacheTests
{
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "apex-dccache-" + Guid.NewGuid().ToString("N"));
        try
        {
            var docs = Path.Combine(root, "Docs");
            Directory.CreateDirectory(Path.Combine(docs, "DCCache"));
            Directory.CreateDirectory(Path.Combine(docs, "Library"));
            byte[] Key(uint type, ulong inst) { var k = new byte[16]; System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(k, type); System.Buffers.Binary.BinaryPrimitives.WriteUInt64BigEndian(k.AsSpan(8), inst); return k; }
            byte[] Pairs(params byte[][] keys) => keys.SelectMany(k => k.Concat(k)).ToArray();
            var lot = new DcCache.Record
            {
                Id = DcCache.StoredId("0xa7e6b0d106f518b841b3aba09cd79153"), Flag = 1,
                Deps = [Convert.FromHexString("050cffe800000000050cffe800000000"), DcCache.StoredId("0xa7e6b0d106f518b841b3aba09cd79153")],
                ContentType = 0x436FEE4C, AssetVersion = 6500, Pairs = Pairs(new byte[16], Key(0x319E4F1D, 1), Key(0x0341ACC9, 2)),
                Name = "Riverside", Thumb = Key(0xD84E7FC5, 3),
            };
            var item = new DcCache.Record
            {
                Id = DcCache.StoredId("0x00000000000000000123456789abcdef"), Flag = 1, Deps = [Convert.FromHexString("050cffe800000000050cffe800000000")],
                ContentType = 0x05F8F5D9, Pairs = Pairs(Key(0x319E4F1D, 9)), Name = "Chair", Thumb = Key(0x0580A2B5, 9),
                Handler = "<?xml version=\"1.0\"?><handler/>"u8.ToArray(), Last = 0,
            };
            var list = DcCache.Build([item, lot]);
            check(DcCache.Build(DcCache.Parse(list)).AsSpan().SequenceEqual(list) && DcCache.Parse(list)[1].FullId == "0xa7e6b0d106f518b841b3aba09cd79153",
                "content cache: a record list reads and writes back unchanged, ids as the game names them");

            // a cache file and missingdeps.idx as the game lays them out; then the lot removed
            var dcdb = Path.Combine(docs, "DCCache", "dcdb0.dbc");
            File.WriteAllBytes(dcdb, DcCache.MissingFile(list));
            var missing = Path.Combine(docs, "DCCache", DcCache.Missing);
            File.WriteAllBytes(missing, DcCache.MissingFile(DcCache.Build([])));
            var before = (File.ReadAllBytes(dcdb), File.ReadAllBytes(missing));
            var res = DcCache.Remove(docs, ["0xa7e6b0d106f518b841b3aba09cd79153", "0xffffffffffffffffffffffffffffffff"], Path.Combine(root, "backup"));
            var after = DcCache.ReadList(dcdb)!;
            var gone = DcCache.ReadList(missing)!;
            check(res.Removed.SequenceEqual(["0xa7e6b0d106f518b841b3aba09cd79153"]) && res.NotFound.Count == 1
                && after.Count == 1 && after[0].Name == "Chair" && after[0].Handler is { Length: > 0 },
                "content cache: removing takes out only that entry; an id not in the cache is reported");
            check(gone.Count == 1 && gone[0].FullId == "0xa7e6b0d106f518b841b3aba09cd79153" && gone[0].Name == "" && gone[0].Deps.Count == 0 && gone[0].ContentType == 0
                && gone[0].Pairs.AsSpan().SequenceEqual(Pairs(Key(0x0341ACC9, 2), Key(0x319E4F1D, 1), new byte[16])),
                "content cache: the removed entry goes to missingdeps.idx cleared, its key pairs in reverse order (as the game writes it)");
            check(Dbpf.Index(dcdb).Count == 1, "content cache: the rewritten file still has one record list in its index");
            DcCache.Restore(docs, Path.Combine(root, "backup"));
            check(File.ReadAllBytes(dcdb).AsSpan().SequenceEqual(before.Item1) && File.ReadAllBytes(missing).AsSpan().SequenceEqual(before.Item2),
                "content cache: Restore puts both files back byte for byte");
        }
        catch (Exception e) { check(false, "content cache: exception " + e); }
        finally { try { Directory.Delete(root, true); } catch (Exception) { } }
    }
}
