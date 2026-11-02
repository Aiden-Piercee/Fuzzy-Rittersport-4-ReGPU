using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace FM4Launcher;

/// <summary>
/// An Xbox 360 game disc image (XDVDFS, the .iso from a disc dump) unpacked to a folder. The volume descriptor sits
/// 32 sectors (2048 bytes each) into the game partition, which starts at 0 in a bare image and at 0x0FD90000,
/// 0x02080000 or 0x18300000 in a full-disc one (XGD2, XGD3, XGD1). A directory is a binary tree of entries
/// { left u16, right u16 (both in 4-byte units), sector u32, size u32, attributes u8, name length u8, name }.
/// </summary>
public static class XIso
{
    const int Sector = 2048;
    static readonly long[] Bases = [0, 0x0FD90000, 0x02080000, 0x18300000];
    static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

    public static bool Looks(string path)
    {
        try { using var f = File.OpenRead(path); return FindBase(f, out _, out _) ; }
        catch { return false; }
    }

    static bool FindBase(Stream f, out long baseOffset, out (uint Sector, uint Size) root)
    {
        var buf = new byte[28];
        foreach (var b in Bases)
        {
            var at = b + 32L * Sector;
            if (at + buf.Length > f.Length) continue;
            f.Seek(at, SeekOrigin.Begin);
            f.ReadExactly(buf);
            if (!buf.AsSpan(0, Magic.Length).SequenceEqual(Magic)) continue;
            baseOffset = b;
            root = (BinaryPrimitives.ReadUInt32LittleEndian(buf.AsSpan(20)), BinaryPrimitives.ReadUInt32LittleEndian(buf.AsSpan(24)));
            return true;
        }
        baseOffset = 0; root = default;
        return false;
    }

    /// <summary>Unpacks every file of the image into dest. progress gets (files done, bytes done, bytes total).</summary>
    public static int Extract(string iso, string dest, Action<int, long, long>? progress = null)
    {
        using var f = File.OpenRead(iso);
        if (!FindBase(f, out var b, out var root)) throw new InvalidDataException($"{Path.GetFileName(iso)} isn't an Xbox 360 disc image.");
        var files = new List<(string Path, long Offset, uint Size)>();
        Walk(f, b, root.Sector, root.Size, "", files, 0);
        long total = files.Sum(x => (long)x.Size), done = 0;
        int n = 0;
        var buf = new byte[1 << 20];
        var full = Path.GetFullPath(dest);
        foreach (var (rel, off, size) in files)
        {
            var target = Path.GetFullPath(Path.Combine(full, rel));
            if (!target.StartsWith(full + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            f.Seek(off, SeekOrigin.Begin);
            using var o = File.Create(target);
            for (long left = size; left > 0;)
            {
                int k = f.Read(buf, 0, (int)Math.Min(buf.Length, left));
                if (k <= 0) throw new EndOfStreamException($"{Path.GetFileName(iso)} ends inside {rel}.");
                o.Write(buf, 0, k);
                left -= k; done += k;
            }
            progress?.Invoke(++n, done, total);
        }
        return n;
    }

    /// <summary>The sha256 (upper-case hex) of one file at the top of the image, read from the image without unpacking it;
    /// null when the image has no such file.</summary>
    public static string? FileSha256(string iso, string name)
    {
        using var f = File.OpenRead(iso);
        if (!FindBase(f, out var b, out var root)) throw new InvalidDataException($"{Path.GetFileName(iso)} isn't an Xbox 360 disc image.");
        var files = new List<(string Path, long Offset, uint Size)>();
        Walk(f, b, root.Sector, root.Size, "", files, 0);
        foreach (var (rel, off, size) in files)
        {
            if (!string.Equals(rel, name, StringComparison.OrdinalIgnoreCase)) continue;
            using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buf = new byte[1 << 20];
            f.Seek(off, SeekOrigin.Begin);
            for (long left = size; left > 0;)
            {
                int k = f.Read(buf, 0, (int)Math.Min(buf.Length, left));
                if (k <= 0) throw new EndOfStreamException($"{Path.GetFileName(iso)} ends inside {rel}.");
                h.AppendData(buf, 0, k);
                left -= k;
            }
            return Convert.ToHexString(h.GetHashAndReset());
        }
        return null;
    }

    static void Walk(FileStream f, long b, uint sector, uint size, string prefix, List<(string, long, uint)> files, int depth)
    {
        if (size == 0 || size > 64 << 20 || depth > 32) return;
        var dir = new byte[size];
        f.Seek(b + (long)sector * Sector, SeekOrigin.Begin);
        f.ReadExactly(dir);
        var seen = new HashSet<int>();
        void Node(int at)
        {
            if (at < 0 || at + 14 > dir.Length || !seen.Add(at)) return;
            var left = BinaryPrimitives.ReadUInt16LittleEndian(dir.AsSpan(at));
            if (left == 0xFFFF) return; // padding: nothing here
            var right = BinaryPrimitives.ReadUInt16LittleEndian(dir.AsSpan(at + 2));
            var sec = BinaryPrimitives.ReadUInt32LittleEndian(dir.AsSpan(at + 4));
            var len = BinaryPrimitives.ReadUInt32LittleEndian(dir.AsSpan(at + 8));
            var attr = dir[at + 12];
            int nl = dir[at + 13];
            if (at + 14 + nl > dir.Length) return;
            var name = Encoding.Latin1.GetString(dir, at + 14, nl);
            if (left != 0) Node(left * 4);
            if (name.Length > 0 && name.IndexOfAny(['/', '\\', ':']) < 0 && name != ".." && name != ".")
            {
                var rel = prefix + name;
                if ((attr & 0x10) != 0) Walk(f, b, sec, len, rel + Path.DirectorySeparatorChar, files, depth + 1);
                else files.Add((rel, b + (long)sec * Sector, len));
            }
            if (right != 0) Node(right * 4);
        }
        Node(0);
    }
}
