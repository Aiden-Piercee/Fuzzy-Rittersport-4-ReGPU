using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace FM4Launcher;

/// <summary>
/// Windows: files dragged from Explorer onto the launcher. WebView2 takes file drops itself and hands the page only
/// the contents, never where the files are. So the OLE drop target of the window and of each of its child windows
/// (WebView2's render surface is one) is replaced with one that reads the dropped paths (CF_HDROP) and tells the
/// page the paths and the pointer position; the page works out which tile is under it. A dropped folder or disc image
/// is then used where it is, with no copy. The child windows appear a moment after the page loads, so
/// <see cref="Install"/> runs a few times; it must run on the window's own (STA) thread.
/// Linux builds keep the page's own drop (the files are streamed to a staging folder, see Import.cs).
/// </summary>
[ComVisible(true)]
public sealed class DropTarget : IDropTarget
{
    readonly IntPtr _hwnd;
    readonly Action<string[], int, int> _dropped;
    readonly Action<bool, int, int> _hover;

    // One target per window: the point of a drag is converted against the window that received it (the render
    // surface's origin is the page's, and it moves with the window).
    DropTarget(IntPtr hwnd, Action<string[], int, int> dropped, Action<bool, int, int> hover) { _hwnd = hwnd; _dropped = dropped; _hover = hover; }

    /// <summary>Takes over the drop target of the window and its child windows; returns how many it took.</summary>
    public static int Install(IntPtr window, Action<string[], int, int> dropped, Action<bool, int, int> hover)
    {
        if (window == IntPtr.Zero) return 0;
        OleInitialize(IntPtr.Zero);
        int taken = 0;
        void Take(IntPtr h)
        {
            RevokeDragDrop(h);
            if (RegisterDragDrop(h, new DropTarget(h, dropped, hover)) == 0) taken++;
        }
        Take(window);
        EnumChildWindows(window, (h, _) => { Take(h); return true; }, IntPtr.Zero);
        return taken;
    }

    // Client pixels (of the window that received the drag) from the OLE screen point.
    (int X, int Y) ToClient(int x, int y)
    {
        var p = new POINT { X = x, Y = y };
        ScreenToClient(_hwnd, ref p);
        return (p.X, p.Y);
    }

    const int DropEffectCopy = 1;

    public int DragEnter(IDataObject data, int keys, POINT pt, ref int effect)
    {
        effect = HasFiles(data) ? DropEffectCopy : 0;
        if (effect != 0) { var c = ToClient(pt.X, pt.Y); _hover(true, c.X, c.Y); }
        return 0;
    }

    public int DragOver(int keys, POINT pt, ref int effect)
    {
        effect = DropEffectCopy;
        var c = ToClient(pt.X, pt.Y);
        _hover(true, c.X, c.Y);
        return 0;
    }

    public int DragLeave() { _hover(false, 0, 0); return 0; }

    public int Drop(IDataObject data, int keys, POINT pt, ref int effect)
    {
        _hover(false, 0, 0);
        var paths = Paths(data);
        effect = paths.Length > 0 ? DropEffectCopy : 0;
        if (paths.Length > 0) { var c = ToClient(pt.X, pt.Y); _dropped(paths, c.X, c.Y); }
        return 0;
    }

    static FORMATETC HDrop() => new() { cfFormat = 15, ptd = IntPtr.Zero, dwAspect = DVASPECT.DVASPECT_CONTENT, lindex = -1, tymed = TYMED.TYMED_HGLOBAL };

    static bool HasFiles(IDataObject data)
    {
        var f = HDrop();
        return data.QueryGetData(ref f) == 0;
    }

    static string[] Paths(IDataObject data)
    {
        var f = HDrop();
        if (data.QueryGetData(ref f) != 0) return [];
        data.GetData(ref f, out var medium);
        try
        {
            var count = DragQueryFileW(medium.unionmember, 0xFFFFFFFF, null, 0);
            var list = new List<string>();
            for (uint i = 0; i < count; i++)
            {
                var sb = new StringBuilder(1024);
                DragQueryFileW(medium.unionmember, i, sb, (uint)sb.Capacity);
                list.Add(sb.ToString());
            }
            return [.. list];
        }
        finally { ReleaseStgMedium(ref medium); }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }

    delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr hwnd, ref POINT pt);
    [DllImport("ole32.dll")] static extern int OleInitialize(IntPtr reserved);
    [DllImport("ole32.dll")] static extern int RegisterDragDrop(IntPtr hwnd, IDropTarget target);
    [DllImport("ole32.dll")] static extern int RevokeDragDrop(IntPtr hwnd);
    [DllImport("ole32.dll")] static extern void ReleaseStgMedium(ref STGMEDIUM medium);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern uint DragQueryFileW(IntPtr hDrop, uint index, StringBuilder? file, uint size);
}

[ComImport, Guid("00000122-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IDropTarget
{
    [PreserveSig] int DragEnter(IDataObject data, int keys, DropTarget.POINT pt, ref int effect);
    [PreserveSig] int DragOver(int keys, DropTarget.POINT pt, ref int effect);
    [PreserveSig] int DragLeave();
    [PreserveSig] int Drop(IDataObject data, int keys, DropTarget.POINT pt, ref int effect);
}
