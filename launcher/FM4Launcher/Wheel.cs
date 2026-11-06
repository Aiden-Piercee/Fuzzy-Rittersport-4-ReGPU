using System.Diagnostics;
using System.Text;

namespace FM4Launcher;

/// <summary>
/// The Wheel stuff tab's backend: the racing wheel settings (the wheel_* keys of settings.ini, which fm4.exe reads at
/// start, src/input/wheel/wheel_config.h) and the live view of every wheel, pedal set, shifter and handbrake. The live
/// view comes from fm4.exe --wheel-probe, which runs the game's own wheel code: the devices, their axis and button
/// numbers and what the game would get are exactly the game's. FM4_WHEEL_PROBE can name another probe (the standalone
/// tools/wheelprobe build).
/// </summary>
public static class Wheel
{
    public const string Prefix = "wheel_";
    static readonly object Gate = new();
    static Process? _probe;

    // ---- settings

    /// <summary>The wheel_* keys of settings.ini, as written.</summary>
    public static Dictionary<string, string> Load()
    {
        var d = new Dictionary<string, string>();
        if (!File.Exists(Paths.SettingsFile)) return d;
        foreach (var raw in File.ReadAllLines(Paths.SettingsFile))
        {
            var line = raw.Trim();
            int eq = line.IndexOf('=');
            if (line.StartsWith('#') || eq <= 0) continue;
            var k = line[..eq].Trim();
            if (k.StartsWith(Prefix, StringComparison.Ordinal)) d[k] = line[(eq + 1)..].Trim();
        }
        return d;
    }

    /// <summary>Replaces every wheel_* key of settings.ini with these (an empty value drops the key: the game's default);
    /// the other keys are kept as they are.</summary>
    public static void Save(IDictionary<string, string> values)
    {
        lock (Gate)
        {
            var lines = File.Exists(Paths.SettingsFile) ? File.ReadAllLines(Paths.SettingsFile).ToList() : ["# Forza Motorsport 4 (fm4) settings; the launcher edits this file."];
            lines.RemoveAll(l => l.TrimStart().StartsWith(Prefix, StringComparison.Ordinal));
            foreach (var (k, v) in values.OrderBy(kv => kv.Key, StringComparer.Ordinal))
            {
                if (!k.StartsWith(Prefix, StringComparison.Ordinal) || !k.All(c => char.IsAsciiLetterOrDigit(c) || c == '_')) continue;
                var clean = new string(v.Where(c => !char.IsControl(c)).ToArray()).Trim();
                if (clean.Length > 0) lines.Add($"{k}={clean}");
            }
            Directory.CreateDirectory(Paths.User);
            File.WriteAllText(Paths.SettingsFile, string.Join("\n", lines) + "\n");
            SendSettingsLocked(values);
        }
    }

    // ---- the live view

    static string ProbeExe => Environment.GetEnvironmentVariable("FM4_WHEEL_PROBE") is { Length: > 0 } p ? p : Paths.Exe;

    /// <summary>Starts the probe (if it isn't running) with the saved settings; each line it prints goes to onLine.</summary>
    public static void Start(Action<string> onLine)
    {
        lock (Gate)
        {
            if (_probe is { HasExited: false }) return;
            if (Platform.Linux) throw new InvalidOperationException("The live wheel view needs Windows for now. The settings on this page still apply to the game.");
            var exe = ProbeExe;
            if (!File.Exists(exe)) throw new InvalidOperationException($"fm4.exe isn't at {exe}: the live wheel view runs it to read the wheels.");
            var psi = new ProcessStartInfo(exe)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                WorkingDirectory = Path.GetDirectoryName(exe)!,
            };
            if (!string.Equals(exe, Environment.GetEnvironmentVariable("FM4_WHEEL_PROBE"), StringComparison.OrdinalIgnoreCase)) psi.ArgumentList.Add("--wheel-probe");
            var p = Process.Start(psi) ?? throw new InvalidOperationException("The wheel probe didn't start.");
            p.OutputDataReceived += (_, e) => { if (!string.IsNullOrEmpty(e.Data) && e.Data[0] == '{') onLine(e.Data); };
            p.ErrorDataReceived += (_, e) => { if (!string.IsNullOrEmpty(e.Data)) onLine(System.Text.Json.JsonSerializer.Serialize(new { t = "log", line = e.Data })); };
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            _probe = p;
            SendSettingsLocked(Load());
        }
    }

    /// <summary>The Test force feedback button: the probe turns the wheel right, then left (its log lines say how it went).</summary>
    public static void TestForceFeedback()
    {
        lock (Gate)
        {
            if (_probe is not { HasExited: false } p) throw new InvalidOperationException("The live wheel view isn't running.");
            try { p.StandardInput.Write("fftest\n"); p.StandardInput.Flush(); }
            catch (IOException) { throw new InvalidOperationException("The live wheel view stopped."); }
        }
    }

    /// <summary>Stops the probe (the end of its input makes it exit).</summary>
    public static void Stop()
    {
        lock (Gate)
        {
            var p = _probe;
            _probe = null;
            if (p is null) return;
            try { p.StandardInput.Close(); } catch { }
            _ = Task.Run(() =>
            {
                try { if (!p.WaitForExit(2000)) p.Kill(); } catch { }
                p.Dispose();
            });
        }
    }

    static void SendSettingsLocked(IDictionary<string, string> values)
    {
        if (_probe is not { HasExited: false } p) return;
        try
        {
            var sb = new StringBuilder();
            foreach (var (k, v) in values)
                if (k.StartsWith(Prefix, StringComparison.Ordinal) && !k.Contains('\n') && !v.Contains('\n')) sb.Append(k).Append('=').Append(v).Append('\n');
            sb.Append(".\n");
            p.StandardInput.Write(sb.ToString());
            p.StandardInput.Flush();
        }
        catch (IOException) { }
    }
}
