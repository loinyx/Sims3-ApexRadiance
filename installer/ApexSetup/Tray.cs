using System.Runtime.InteropServices;

namespace ApexSetup;

// The launcher's icon in the notification area while the game runs (and when started with Windows): a click opens the
// window, the menu has Open, Play and Quit. A message-only window of its own receives the icon's clicks.
public sealed partial class Tray : IDisposable
{
    const int WmApp = 0x8000, WmTray = WmApp + 1, WmLButtonUp = 0x0202, WmRButtonUp = 0x0205, WmCommand = 0x0111;
    const uint NimAdd = 0, NimModify = 1, NimDelete = 2, NifMessage = 1, NifIcon = 2, NifTip = 4;
    readonly WndProc _proc; // (kept alive: Windows calls it)
    readonly nint _hwnd, _icon;
    readonly Action _open;
    readonly Func<IReadOnlyList<(string Text, Action Run)>> _menu;
    readonly uint _taskbarCreated;
    bool _shown;

    public Tray(string iconFile, Action open, Func<IReadOnlyList<(string, Action)>> menu)
    {
        _open = open;
        _menu = menu;
        _proc = Proc;
        var wc = new WndClass { lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_proc), lpszClassName = "ApexLauncherTray", hInstance = GetModuleHandleW(null) };
        RegisterClassW(ref wc);
        _hwnd = CreateWindowExW(0, "ApexLauncherTray", "", 0, 0, 0, 0, 0, new nint(-3) /* HWND_MESSAGE */, 0, wc.hInstance, 0);
        _icon = File.Exists(iconFile) ? LoadImageW(0, iconFile, 1 /* IMAGE_ICON */, 16, 16, 0x10 /* LR_LOADFROMFILE */) : 0;
        _taskbarCreated = RegisterWindowMessageW("TaskbarCreated");
    }

    public void Show(string tip)
    {
        var d = Data(tip);
        if (_shown) Shell_NotifyIconW(NimModify, ref d);
        else _shown = Shell_NotifyIconW(NimAdd, ref d);
    }

    public void Hide()
    {
        if (!_shown) return;
        var d = Data("");
        Shell_NotifyIconW(NimDelete, ref d);
        _shown = false;
    }

    NotifyIconData Data(string tip) => new()
    {
        cbSize = (uint)Marshal.SizeOf<NotifyIconData>(), hWnd = _hwnd, uID = 1, uFlags = NifMessage | NifIcon | NifTip,
        uCallbackMessage = WmTray, hIcon = _icon, szTip = tip.Length > 127 ? tip[..127] : tip,
    };

    nint Proc(nint hwnd, uint msg, nint w, nint l)
    {
        if (msg == _taskbarCreated && _shown) { _shown = false; Show(""); } // (Explorer restarted: the icon is added again)
        if (msg == WmTray)
        {
            var ev = (int)(l & 0xFFFF);
            if (ev == WmLButtonUp) _open();
            else if (ev == WmRButtonUp) ShowMenu();
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }

    void ShowMenu()
    {
        var items = _menu();
        var m = CreatePopupMenu();
        for (int i = 0; i < items.Count; i++) AppendMenuW(m, 0, (nuint)(i + 1), items[i].Text);
        GetCursorPos(out var pt);
        SetForegroundWindow(_hwnd); // (so the menu closes when the player clicks elsewhere)
        var cmd = TrackPopupMenuEx(m, 0x0100 /* TPM_RETURNCMD */, pt.X, pt.Y, _hwnd, 0);
        DestroyMenu(m);
        if (cmd > 0 && cmd <= items.Count) items[cmd - 1].Run();
    }

    public void Dispose()
    {
        Hide();
        if (_icon != 0) DestroyIcon(_icon);
        DestroyWindow(_hwnd);
    }

    delegate nint WndProc(nint hwnd, uint msg, nint w, nint l);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WndClass
    {
        public uint style; public nint lpfnWndProc; public int cbClsExtra, cbWndExtra; public nint hInstance, hIcon, hCursor, hbrBackground;
        public string? lpszMenuName; public string lpszClassName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct NotifyIconData
    {
        public uint cbSize; public nint hWnd; public uint uID, uFlags, uCallbackMessage; public nint hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
        public uint dwState, dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
        public uint uVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
        public uint dwInfoFlags; public Guid guidItem; public nint hBalloonIcon;
    }

    struct Pt { public int X, Y; }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern bool Shell_NotifyIconW(uint msg, ref NotifyIconData data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WndClass wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern nint CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h, nint parent, nint menu, nint inst, nint param);
    [DllImport("user32.dll")] static extern nint DefWindowProcW(nint hwnd, uint msg, nint w, nint l);
    [DllImport("user32.dll")] static extern bool DestroyWindow(nint hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint RegisterWindowMessageW(string name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern nint LoadImageW(nint inst, string name, uint type, int cx, int cy, uint load);
    [DllImport("user32.dll")] static extern bool DestroyIcon(nint icon);
    [DllImport("user32.dll")] static extern nint CreatePopupMenu();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool AppendMenuW(nint menu, uint flags, nuint id, string text);
    [DllImport("user32.dll")] static extern int TrackPopupMenuEx(nint menu, uint flags, int x, int y, nint hwnd, nint tpm);
    [DllImport("user32.dll")] static extern bool DestroyMenu(nint menu);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out Pt p);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(nint hwnd);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern nint GetModuleHandleW(string? name);
}
