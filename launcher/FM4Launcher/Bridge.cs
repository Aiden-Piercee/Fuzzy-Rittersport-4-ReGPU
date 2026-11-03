using System.Text.Json;
using System.Text.Json.Nodes;
using Photino.NET;

namespace FM4Launcher;

/// <summary>
/// The page's calls: { id, cmd, args } over the window's message channel (wwwroot/host.js), answered with
/// { id, ok, result | error }. Progress and other pushes go out as { event, ... }.
/// </summary>
public sealed class Bridge
{
    static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    readonly PhotinoWindow _window;
    Updater.Release? _latest;
    bool _dragging;
    long _lastOver;

    public Bridge(PhotinoWindow window)
    {
        _window = window;
        window.RegisterWebMessageReceivedHandler((_, text) => OnMessage(text));
        // Windows: drops from Explorer give the real paths (DropTarget.cs). The window's child windows exist a moment
        // after the page loads, so the drop target is taken over a few times. Elsewhere the page's own drop is used.
        if (OperatingSystem.IsWindows())
            _ = Task.Run(async () =>
            {
                foreach (var wait in new[] { 1500, 2000, 4000, 8000 })
                {
                    await Task.Delay(wait);
                    // RegisterDragDrop must run on the window's own (STA) thread, not this pool thread.
                    try { window.Invoke(() => DropTarget.Install(window.WindowHandle, OnDropped, OnHover)); }
                    catch { }
                }
            });
    }

    /// <summary>A file drag over the window (x, y: client pixels): lights the page's hint and, at most every 50 ms, tells it where the pointer is.</summary>
    void OnHover(bool on, int x, int y)
    {
        if (!on)
        {
            _dragging = false;
            Push("dragging", false);
            return;
        }
        if (!_dragging) { _dragging = true; Push("dragging", true); }
        var now = Environment.TickCount64;
        if (now - _lastOver < 50) return;
        _lastOver = now;
        Push("over", new { x, y });
    }

    /// <summary>Files dropped: the page works out which tile is under the point and asks for them to be used ("drop.paths").</summary>
    void OnDropped(string[] paths, int x, int y) => Push("dropped-at", new { paths, x, y });

    async void OnMessage(string text)
    {
        JsonNode? msg;
        try { msg = JsonNode.Parse(text); } catch { return; }
        var id = msg?["id"]?.GetValue<int>() ?? 0;
        var cmd = msg?["cmd"]?.GetValue<string>() ?? "";
        var args = msg?["args"];
        try
        {
            var result = await Run(cmd, args);
            Post(new { id, ok = true, result });
        }
        catch (Exception ex)
        {
            Post(new { id, ok = false, error = ex.Message });
        }
    }

    // Photino takes messages from any thread (an update's progress comes from a worker).
    void Post(object o) => _window.SendWebMessage(JsonSerializer.Serialize(o, Json));
    void Push(string name, object data) => Post(new { @event = name, data });

    static readonly (string, string[])[] AnyFile = [("Xbox 360 content packages", ["*.*"])];
    static readonly (string, string[])[] ZipFilter = [("Profile zip", ["*.zip"])];

    async Task<object?> Run(string cmd, JsonNode? a)
    {
        switch (cmd)
        {
            case "state": return State();
            case "save":
                SaveFromPage(a.Deserialize<GameSettings>(Json)!);
                return State();
            // The folders are changed only by a drop, a pick or Reset, never by the page's copy of the settings.
            case "game.reset":
                {
                    var s = GameSettings.Load();
                    s.GameFolder = "";
                    s.Save();
                    return State();
                }
            // "Check game files": the only way the game folder's default.xex is hashed besides a drop or pick.
            case "xex.check":
                await Task.Run(() => Import.CheckNow(Paths.GameDir));
                return State();
            case "launch":
                if (a?["settings"].Deserialize<GameSettings>(Json) is { } toSave) SaveFromPage(toSave);
                if (Game.Running) throw new InvalidOperationException("The game is already running.");
                Game.Launch();
                return null;
            case "shaders.delete":
                ShaderCache.Delete();
                return State();
            case "shaders.rebuild":
                ShaderCache.Delete();
                Game.Launch();
                return State();
            case "dlc.toggle":
                Dlc.SetEnabled(a!["id"]!.GetValue<string>(), a["enabled"]!.GetValue<bool>());
                return State();
            case "dlc.add":
                {
                    var files = await _window.ShowOpenFileAsync("Choose DLC packages or archives (zip, 7z)", null, true, AnyFile);
                    if (files is not { Length: > 0 }) return new { added = 0, errors = Array.Empty<string>(), state = State() };
                    var (added, errors) = await Task.Run(() => Import.Dlc(files));
                    return new { added, errors, state = State() };
                }
            // A drop from Explorer (Windows): the paths are known, so things are used where they are, nothing is staged.
            case "drop.paths":
                {
                    var kind = Str(a, "kind");
                    var paths = a!["paths"]!.AsArray().Select(p => p!.GetValue<string>()).ToArray();
                    if (kind == "dlc")
                    {
                        var (added, errors) = await Task.Run(() => Import.Dlc(paths));
                        return new { added, errors, state = State() };
                    }
                    if (paths.Length != 1) throw new InvalidOperationException("Drop the disc image (.iso) or one game folder on the Game tile.");
                    var msg = await Task.Run(() => Import.Game(paths[0], s => Push("status", s)));
                    return new { message = msg, state = State() };
                }
            // A drop in the page (Linux): the page sends each file in pieces (the window can't give a dropped file's path), then asks for it
            // to be used. begin -> write (many) -> finish.
            case "drop.begin": return Import.Begin();
            case "drop.write":
                Import.Write(Str(a, "id"), Str(a, "rel"), a!["first"]!.GetValue<bool>(), Convert.FromBase64String(Str(a, "data")));
                return null;
            case "drop.finish":
                {
                    var id = Str(a, "id");
                    var kind = Str(a, "kind");
                    try
                    {
                        if (kind == "none") return null; // the page gave up: just clear the staging folder
                        var items = Import.StagedItems(id);
                        if (items.Count == 0) throw new InvalidOperationException("Nothing was dropped.");
                        if (kind == "dlc")
                        {
                            var (added, errors) = await Task.Run(() => Import.Dlc(items));
                            return new { added, errors, state = State() };
                        }
                        // One thing: a file, or a folder. A folder dropped with its files beside others: the staging folder itself.
                        var one = items.Count == 1 ? items[0] : Path.GetDirectoryName(items[0])!;
                        var msg = await Task.Run(() => Import.Game(one, s => Push("status", s)));
                        return new { message = msg, state = State() };
                    }
                    finally { Import.Discard(id); }
                }
            case "import.pick":
                {
                    // The Disc image… button: the game disc (.iso), read where it is.
                    var files = await _window.ShowOpenFileAsync("Choose the game disc image (.iso)", null, false, [("Disc image", ["*.iso"])]);
                    if (files is not { Length: > 0 }) return null;
                    var msg = await Task.Run(() => Import.Game(files[0], s => Push("status", s)));
                    return new { message = msg, state = State() };
                }
            case "folder.browse":
                {
                    // Game folder or Title update: pick a folder, keep it in settings.ini ("" from Reset = the default).
                    var which = a?.GetValue<string>();
                    if (which is not ("game" or "update")) throw new ArgumentException("Unknown folder.");
                    var settings = GameSettings.Load();
                    var current = which == "game" ? Paths.GameDir : Paths.TitleUpdateDirDefault;
                    var picked = await _window.ShowOpenFolderAsync(which == "game" ? "Choose the game folder (the extracted disc, with default.xex)" : "Choose the title update folder (with default.xexp)",
                        Directory.Exists(current) ? current : null, false);
                    if (picked is { Length: > 0 })
                    {
                        if (which == "game") Import.RequireGoty(picked[0]);
                        if (which == "game") settings.GameFolder = picked[0]; else settings.TitleUpdateFolder = picked[0];
                        settings.Save();
                    }
                    return State();
                }
            case "photos.open":
                PhotosWindow.Show(_window);
                return null;
            case "open":
                {
                    var which = a?.GetValue<string>();
                    var path = which switch
                    {
                        "user" => Paths.User,
                        "dlc" => Directory.CreateDirectory(Paths.Dlc).FullName,
                        "shaders" => Paths.User,
                        "install" => Paths.Root,
                        "log" => Path.Combine(Paths.Root, "out"),
                        _ => throw new ArgumentException("Unknown folder."),
                    };
                    Platform.OpenFolder(path);
                    return null;
                }
            case "profiles.list": return ProfilesState();
            case "profiles.use":
                Profiles.SetActive(Str(a, "name"));
                return ProfilesState();
            case "profiles.create":
                Profiles.Create(Str(a, "name"));
                return ProfilesState();
            case "profiles.duplicate":
                Profiles.Duplicate(Str(a, "name"), Str(a, "newName"));
                return ProfilesState();
            case "profiles.rename":
                Profiles.Rename(Str(a, "name"), Str(a, "newName"));
                return ProfilesState();
            case "profiles.delete":
                {
                    var b = Profiles.Delete(Str(a, "name"));
                    return new { backup = b.File, state = ProfilesState() };
                }
            case "profiles.backup":
                {
                    var b = Profiles.MakeBackup(Str(a, "name"));
                    return new { backup = b.File, state = ProfilesState() };
                }
            case "profiles.restore":
                {
                    var name = Profiles.Restore(Str(a, "file"), a?["asName"]?.GetValue<string>());
                    return new { name, state = ProfilesState() };
                }
            case "profiles.import":
                {
                    var files = await _window.ShowOpenFileAsync("Import a profile", null, false, ZipFilter);
                    if (files is not { Length: > 0 }) return null;
                    var name = Profiles.Restore(files[0], a?["asName"]?.GetValue<string>());
                    return new { name, state = ProfilesState() };
                }
            case "profiles.export":
                {
                    var name = Str(a, "name");
                    var file = await _window.ShowSaveFileAsync("Export profile", Profiles.FolderName(name) + ".zip", ZipFilter);
                    if (string.IsNullOrEmpty(file)) return false;
                    Profiles.Export(name, file);
                    return true;
                }
            case "profiles.mergePhotos":
                return new { copied = Profiles.MergePhotos(Str(a, "from"), Str(a, "into")), state = ProfilesState() };
            case "profiles.openBackups":
                Platform.OpenFolder(Directory.CreateDirectory(Profiles.Backups).FullName);
                return null;
            case "photos.list": return Photos.List();
            case "photos.copy":
                Platform.CopyPng(Convert.FromBase64String(a!.GetValue<string>()));
                return null;
            case "photos.saveAs":
                {
                    var id = a!.GetValue<string>();
                    var file = await _window.ShowSaveFileAsync("Save photo", Path.Combine(Photos.SaveFolder, Path.GetFileName(Photos.PathOf(id)) + ".jpg"),
                        [("JPEG image", ["*.jpg"]), ("PNG image", ["*.png"])]);
                    return string.IsNullOrEmpty(file) ? null : file;
                }
            case "photos.write":
                {
                    var png = a?["png"]?.GetValue<string>();
                    Photos.Write(Str(a, "id"), Str(a, "file"), png is null ? null : Convert.FromBase64String(png));
                    return null;
                }
            case "photos.reveal":
                Photos.Reveal(a?.GetValue<string>());
                return null;
            case "url":
                {
                    var url = a?.GetValue<string>() ?? "";
                    if (!url.StartsWith("https://github.com/", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Only GitHub links open from here.");
                    Platform.OpenUrl(url);
                    return null;
                }
            case "update.check":
                _latest = await Updater.LatestAsync();
                return UpdateInfo();
            case "update.apply":
                {
                    // The patch zip is laid out for the Windows install (build/tu, a Windows launcher).
                    if (Platform.Linux) throw new InvalidOperationException("On Linux, download the release from its page and unpack it over this folder.");
                    _latest ??= await Updater.LatestAsync();
                    var applied = await Updater.DownloadAndApplyAsync(_latest, (p, text) => Push("progress", new { p, text }));
                    return new { applied.Files, applied.LauncherChanged, update = UpdateInfo(), state = State() };
                }
            case "restart":
                System.Diagnostics.Process.Start(Environment.ProcessPath!);
                _window.Invoke(_window.Close);
                return null;
            default:
                throw new ArgumentException($"Unknown command {cmd}.");
        }
    }

    static string Str(JsonNode? a, string key) => a?[key]?.GetValue<string>() ?? throw new ArgumentException($"Missing {key}.");

    static object ProfilesState() => new { profiles = Profiles.List(), backups = Profiles.ListBackups(), running = Game.Running };

    object UpdateInfo()
    {
        var installed = Updater.InstalledVersion();
        return new
        {
            installed,
            latest = _latest?.Version,
            name = _latest?.Name,
            notes = _latest?.Notes,
            page = _latest?.Page,
            published = _latest?.Published,
            patchBytes = _latest?.PatchBytes ?? 0,
            hasPatch = _latest?.PatchUrl is not null,
            available = _latest is not null && Updater.IsNewer(_latest.Version, installed),
        };
    }

    // The page's copy of the settings, with the folders kept as the backend has them (a drop or pick may have changed
    // them after the page last looked).
    static void SaveFromPage(GameSettings fromPage)
    {
        var now = GameSettings.Load();
        fromPage.GameFolder = now.GameFolder;
        fromPage.TitleUpdateFolder = now.TitleUpdateFolder;
        fromPage.Save();
    }

    static object State()
    {
        var settings = GameSettings.Load();
        var stringTables = Path.Combine(Paths.GameDir, "Media", "StringTables");
        return new
        {
            settings,
            version = Updater.InstalledVersion(),
            launcherVersion = Updater.LauncherVersion(),
            paths = new { root = Paths.Root, user = Paths.User, gameDefault = Paths.GameDirDefault, gameDir = Paths.GameDir, updateDefault = Paths.TitleUpdateDirDefault },
            checks = new
            {
                exe = File.Exists(Paths.Exe),
                game = File.Exists(Path.Combine(Paths.GameDir, "default.xex")),
                xex = Import.XexStatus(Paths.GameDir),
                running = Game.Running,
            },
            options = new
            {
                resolutions = Options.Resolutions(),
                aspects = Options.Aspects.Select(x => new { value = x.Value, name = x.Name }),
                fps = Options.Fps,
                msaa = Options.Msaa.Select(x => new { value = x.Value, name = x.Name }),
                smaa = Options.Smaa.Select(x => new { value = x.Value, name = x.Name }),
                upscalers = Options.Upscalers.Select(x => new { value = x.Value, name = x.Name }),
                upscale = Options.Upscale.Select(x => new { value = x.Value, name = x.Name }),
                optimizeLevels = Options.OptimizeLevels.Select(x => new { value = x.Value, name = x.Name }),
                debugViews = Options.DebugViews.Select(x => new { value = x.Value, name = x.Name }),
                sharpening = Options.Sharpening.Select(x => new { value = x.Value, name = x.Name }),
                languages = Options.InstalledLanguages().Select(l => new { id = l.Id, code = l.Code, name = l.Name }),
                missingLanguages = Directory.Exists(stringTables)
                    ? new[] { "CZ", "NL", "HU" }.Where(c => File.Exists(Path.Combine(stringTables, c + ".zip"))).ToArray() : [],
            },
            shaders = ShaderCache.Get(),
            dlc = Dlc.List(),
        };
    }
}
