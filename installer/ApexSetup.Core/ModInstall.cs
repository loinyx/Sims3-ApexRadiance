namespace ApexSetup;

// What the launcher checks before it adds a file (ModAdd), so a broken or wrong one never reaches the game: a package must
// be a Sims 3 one (DBPF 2.0; 2.1 is The Sims 4's), not a protected one, with an index that lies inside the file and
// resources that do too. Nothing here writes.
public static class ModInstall
{
    // DBPF header and index, by the format the game reads (and Dbpf.Index): what makes the game skip or choke on a package
    public static List<string> PackageProblems(string path, List<string>? warnings = null)
    {
        var p = new List<string>();
        using var f = File.OpenRead(path);
        if (f.Length < 96) { p.Add("Not a package: the file is too short."); return p; }
        var head = new byte[96];
        f.ReadExactly(head);
        var magic = System.Text.Encoding.ASCII.GetString(head, 0, 4);
        if (magic == "DBPP") { p.Add("A protected package: the game can't read it."); return p; }
        if (magic != "DBPF") { p.Add("Not a package: the file doesn't start like one."); return p; }
        var major = BitConverter.ToUInt32(head, 4);
        var minor = BitConverter.ToUInt32(head, 8);
        if (major == 2 && minor == 1) { p.Add("A package for The Sims 4: The Sims 3 can't use it."); return p; }
        if (major != 2 || minor != 0) { p.Add("A package for another game or version."); return p; }
        var count = BitConverter.ToUInt32(head, 0x24);
        var indexSize = BitConverter.ToUInt32(head, 0x2C);
        var at = BitConverter.ToUInt32(head, 0x40);
        if (count == 0) { p.Add("The package is empty."); return p; }
        if (at + (long)indexSize > f.Length || indexSize < 4 + count * 16L) { p.Add("The package is damaged: its index points outside the file."); return p; }
        f.Position = 0;
        var entries = Dbpf.Index(f);
        if (entries.Count != count) { p.Add("The package is damaged: its index can't be read."); return p; }
        if (entries.Any(e => e.Offset + (long)e.Size > f.Length)) p.Add("The package is damaged: part of it is missing. The download may have been cut short.");
        if (warnings is not null && entries.Any(e => e.Type == 0x073FAA07)) warnings.Add("A script mod: it runs code in the game. Install it only from a creator you trust.");
        return p;
    }
}
