using System.IO.Compression;

namespace NewSuperPaperBros;

// Mod installation for both launchers: a mod is a folder holding files\ (replaces game files, riivolution.cpp) and/or
// textures\ (texture_replacement.cpp), installed as <UserData>\mods\<name>.
static class ModInstall
{
    // Extracts a zip into `dest`, refusing entries that would land outside it ("../" or absolute paths: zip slip).
    public static void ExtractZip(string zip, string dest)
    {
        var root = Path.GetFullPath(dest) + Path.DirectorySeparatorChar;
        Directory.CreateDirectory(root);
        using var archive = ZipFile.OpenRead(zip);
        foreach (var entry in archive.Entries)
        {
            var target = Path.GetFullPath(Path.Combine(root, entry.FullName.Replace('\\', '/')));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("The zip holds a file outside its own folder: " + entry.FullName);
            if (entry.FullName.EndsWith('/') || entry.FullName.EndsWith('\\')) { Directory.CreateDirectory(target); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            entry.ExtractToFile(target, overwrite: true);
        }
    }

    // The folder holding files\ or textures\: `dir` itself, or the first one found up to three levels down.
    public static string? FindRoot(string dir, int depth = 3)
    {
        if (Directory.Exists(Path.Combine(dir, "files")) || Directory.Exists(Path.Combine(dir, "textures"))) return dir;
        if (depth == 0) return null;
        foreach (var sub in Directory.GetDirectories(dir).OrderBy(d => d, StringComparer.Ordinal))
            if (FindRoot(sub, depth - 1) is { } found) return found;
        return null;
    }

    // A folder name safe on every system.
    public static string SafeName(string name)
    {
        var bad = Path.GetInvalidFileNameChars().Concat(new[] { '/', '\\', ':' }).ToHashSet();
        var clean = new string(name.Select(c => bad.Contains(c) ? '_' : c).ToArray()).Trim().TrimEnd('.');
        return clean.Length == 0 || clean == ".." ? "mod" : clean;
    }

    public static void CopyDir(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var file in Directory.GetFiles(from)) File.Copy(file, Path.Combine(to, Path.GetFileName(file)), overwrite: true);
        foreach (var dir in Directory.GetDirectories(from)) CopyDir(dir, Path.Combine(to, Path.GetFileName(dir)));
    }

    // --selftest check: a wrapped mod zip installs, and a zip slipping out of its folder is refused.
    public static bool SelfCheck()
    {
        var work = Path.Combine(Path.GetTempPath(), "nspb-modcheck-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(work);
            var good = Path.Combine(work, "good.zip");
            using (var z = ZipFile.Open(good, ZipArchiveMode.Create)) z.CreateEntry("My Mod/files/a.txt");
            var bad = Path.Combine(work, "bad.zip");
            using (var z = ZipFile.Open(bad, ZipArchiveMode.Create)) z.CreateEntry("../escape.txt");
            var outGood = Path.Combine(work, "out");
            ExtractZip(good, outGood);
            bool goodOk = FindRoot(outGood) is { } r && Path.GetFileName(r) == "My Mod";
            bool badRefused = false;
            try { ExtractZip(bad, Path.Combine(work, "out2")); } catch (InvalidDataException) { badRefused = true; }
            return goodOk && badRefused && !File.Exists(Path.Combine(work, "escape.txt")) && SafeName("a/b:c") == "a_b_c";
        }
        finally { try { Directory.Delete(work, true); } catch (Exception) { } }
    }
}
