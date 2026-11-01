using System.Text;
using SharpCompress.Archives;

namespace FM4Launcher;

/// <summary>
/// Turning what the user hands the launcher into what the game reads: a game disc (an .iso, or the files already
/// extracted), and DLC (STFS packages, loose, in a folder or in a zip / 7z / rar). Dropped things arrive in a staging
/// folder under Documents\fm4\incoming, written by the page in pieces (the window can't hand over a dropped file's path);
/// picked ones are read where they are.
/// </summary>
public static class Import
{
    static string Incoming => Path.Combine(Paths.User, "incoming");
    static readonly object Gate = new();
    static int _next;

    // ---- staging (the page's drop)

    public static string Begin()
    {
        lock (Gate)
        {
            if (_next == 0 && Directory.Exists(Incoming)) { try { Directory.Delete(Incoming, true); } catch { } } // leftovers of a crashed run
            var id = (++_next).ToString();
            Directory.CreateDirectory(Path.Combine(Incoming, id));
            return id;
        }
    }

    static string StagePath(string id, string rel)
    {
        if (id.Any(c => !char.IsDigit(c)) || id.Length == 0) throw new ArgumentException("Unknown drop.");
        var root = Path.GetFullPath(Path.Combine(Incoming, id));
        var full = Path.GetFullPath(Path.Combine(root, rel.Replace('/', Path.DirectorySeparatorChar).TrimStart(Path.DirectorySeparatorChar)));
        if (!full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Not a file name.");
        return full;
    }

    public static void Write(string id, string rel, bool first, byte[] data)
    {
        var full = StagePath(id, rel);
        Directory.CreateDirectory(Path.GetDirectoryName(full)!);
        using var f = new FileStream(full, first ? FileMode.Create : FileMode.Append, FileAccess.Write);
        f.Write(data);
    }

    /// <summary>What was dropped: its top-level entries (one file, or the folders and files dropped together).</summary>
    public static List<string> StagedItems(string id)
    {
        var root = StagePath(id, "x")[..^1];
        return Directory.EnumerateFileSystemEntries(root).ToList();
    }

    public static void Discard(string id)
    {
        try { Directory.Delete(StagePath(id, "x")[..^1], true); } catch { }
    }

    // ---- helpers

    static bool IsArchive(string p) => Path.GetExtension(p).ToLowerInvariant() is ".zip" or ".7z" or ".rar";

    public static bool IsStfs(string p)
    {
        try
        {
            using var f = File.OpenRead(p);
            var m = new byte[4];
            return f.Read(m, 0, 4) == 4 && Encoding.ASCII.GetString(m) is "LIVE" or "PIRS" or "CON ";
        }
        catch { return false; }
    }

    static void ExtractArchive(string archive, string dest)
    {
        using var a = ArchiveFactory.Open(archive);
        var full = Path.GetFullPath(dest);
        foreach (var e in a.Entries.Where(e => !e.IsDirectory && e.Key is { Length: > 0 }))
        {
            var target = Path.GetFullPath(Path.Combine(full, e.Key!.Replace('/', Path.DirectorySeparatorChar)));
            if (!target.StartsWith(full + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using var s = e.OpenEntryStream();
            using var o = File.Create(target);
            s.CopyTo(o);
        }
    }

    static string Temp() => Directory.CreateDirectory(Path.Combine(Incoming, "work-" + Guid.NewGuid().ToString("N")[..8])).FullName;

    static void CopyTree(string from, string to)
    {
        foreach (var d in Directory.EnumerateDirectories(from, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, d)));
        Directory.CreateDirectory(to);
        foreach (var f in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
            File.Copy(f, Path.Combine(to, Path.GetRelativePath(from, f)), true);
    }

    // ---- game

    /// <summary>A disc image or a folder with the extracted disc (default.xex at its top, or one level inside). Goes into
    /// the chosen game folder (the launcher's default one when none is chosen). Returns the message to show.</summary>
    public static string Game(string path, Action<string>? status = null)
    {
        var settings = GameSettings.Load();
        var dest = Paths.GameDir;
        if (File.Exists(path))
        {
            if (!XIso.Looks(path)) throw new InvalidDataException($"{Path.GetFileName(path)} isn't a game disc image (.iso).");
            var n = XIso.Extract(path, dest, (i, done, total) => status?.Invoke($"Unpacking the disc: {done * 100 / Math.Max(1, total)}%"));
            if (!File.Exists(Path.Combine(dest, "default.xex"))) throw new InvalidDataException("The disc unpacked, but has no default.xex: is it Forza Motorsport 4?");
            return $"Disc unpacked: {n} files in {dest}.";
        }
        var root = FindUp(path, "default.xex") ?? throw new InvalidDataException("That folder has no default.xex: pick the extracted disc.");
        if (string.Equals(Path.GetFullPath(root).TrimEnd('\\', '/'), Path.GetFullPath(dest).TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase))
            return $"The game is already in {dest}.";
        if (path.StartsWith(Incoming, StringComparison.OrdinalIgnoreCase))
        {
            status?.Invoke("Moving the game files into place…");
            CopyTree(root, dest);
            return $"Game files are in {dest}.";
        }
        // A folder picked in place: use it where it is, nothing copied.
        settings.GameFolder = root;
        settings.Save();
        return $"Using {root} as the game folder.";
    }

    static string? FindUp(string dir, string file, int depth = 3)
    {
        if (!Directory.Exists(dir)) return null;
        if (File.Exists(Path.Combine(dir, file))) return dir;
        if (depth == 0) return null;
        foreach (var d in Directory.EnumerateDirectories(dir))
            if (FindUp(d, file, depth - 1) is { } hit) return hit;
        return null;
    }

    // ---- DLC

    /// <summary>Packages loose, in folders, or in zip / 7z / rar archives (any depth). Anything inside that isn't an FM4 package is skipped.</summary>
    public static (int Added, List<string> Errors) Dlc(IEnumerable<string> paths)
    {
        if (FM4Launcher.Game.Running) throw new InvalidOperationException("Close the game first: DLC is read while it runs.");
        Directory.CreateDirectory(Paths.Dlc);
        int added = 0;
        var errors = new List<string>();
        void One(string f)
        {
            if (IsArchive(f))
            {
                var work = Temp();
                try { ExtractArchive(f, work); foreach (var x in Directory.EnumerateFiles(work, "*", SearchOption.AllDirectories)) One(x); }
                catch (Exception ex) { errors.Add($"{Path.GetFileName(f)}: {ex.Message}"); }
                finally { try { Directory.Delete(work, true); } catch { } }
                return;
            }
            if (!IsStfs(f)) return;
            try { Stfs.Install(f, Paths.Dlc, false); added++; }
            catch (Exception ex) { errors.Add($"{Path.GetFileName(f)}: {ex.Message}"); }
        }
        foreach (var p in paths)
        {
            if (Directory.Exists(p)) foreach (var f in Directory.EnumerateFiles(p, "*", SearchOption.AllDirectories)) One(f);
            else if (File.Exists(p)) One(p);
        }
        if (added == 0 && errors.Count == 0) errors.Add("No Forza Motorsport 4 DLC packages found in that.");
        return (added, errors);
    }
}
