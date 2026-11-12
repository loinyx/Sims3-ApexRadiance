# Records what the game window shows, independent of the mod: PrintWindow (the window's own content, other windows over it
# do not matter) at -Fps frames a second, each frame scaled to 480x270 and saved as a JPEG, and the mean brightness of a
# 16 x 9 grid of cells written to luma.csv (one row per frame: qpc ms, wall clock as in Apex's log, frame, then the cells). flash_scan.py reads luma.csv.
# Runs until the game exits or -StopFile exists. Started by run.ps1 -Grab; usable alone:
#   powershell -File grab.ps1 -GamePid <pid> -OutDir <dir> [-Fps 8]
param([Parameter(Mandatory)][int]$GamePid, [Parameter(Mandatory)][string]$OutDir, [double]$Fps = 8, [string]$StopFile)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class ApexGrab {
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long c);
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long f);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    public const int GX = 16, GY = 9, W = 480, H = 270;
    public static double QpcMs() { long c, f; QueryPerformanceCounter(out c); QueryPerformanceFrequency(out f); return c * 1000.0 / f; }
    // One frame: the scaled image (null when the window has no client area) and the cells' mean luma (0..255)
    public static Bitmap Grab(IntPtr h, double[] cells) {
        RECT r; if (!GetClientRect(h, out r) || r.R < 16 || r.B < 16) return null;
        using (var full = new Bitmap(r.R, r.B, PixelFormat.Format32bppRgb)) {
            using (var g = Graphics.FromImage(full)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); } // 3: client area, full content
            var s = new Bitmap(W, H, PixelFormat.Format32bppRgb);
            using (var g = Graphics.FromImage(s)) { g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.Bilinear; g.DrawImage(full, 0, 0, W, H); }
            var d = s.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
            var px = new byte[d.Stride * H]; Marshal.Copy(d.Scan0, px, 0, px.Length); s.UnlockBits(d);
            var n = new int[GX * GY]; Array.Clear(cells, 0, cells.Length);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                int i = y * d.Stride + x * 4, c = (y * GY / H) * GX + x * GX / W;
                cells[c] += 0.0722 * px[i] + 0.7152 * px[i + 1] + 0.2126 * px[i + 2]; n[c]++;
            }
            for (int c = 0; c < cells.Length; c++) cells[c] /= Math.Max(1, n[c]);
            return s;
        }
    }
}
'@
$proc = Get-Process -Id $GamePid
# A 1-pixel window kept on top at the screen's corner: a borderless game window over the whole screen with nothing over
# it is shown by independent flip, which bypasses the desktop composition PrintWindow reads (black frames); any window
# over it keeps it composed. Nothing in the game changes.
Add-Type -AssemblyName System.Windows.Forms
$pin = New-Object Windows.Forms.Form -Property @{ FormBorderStyle = 'None'; ShowInTaskbar = $false; TopMost = $true; StartPosition = 'Manual';
    Location = (New-Object Drawing.Point 0, 0); Size = (New-Object Drawing.Size 1, 1); BackColor = [Drawing.Color]::Black }
$pin.Show()
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$jpeg = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
$ep = New-Object Drawing.Imaging.EncoderParameters 1
$ep.Param[0] = New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Quality), 80L
$cells = New-Object double[] ([ApexGrab]::GX * [ApexGrab]::GY)
$csv = New-Object IO.StreamWriter (Join-Path $OutDir 'luma.csv')
$csv.WriteLine('qpcMs,clock,frame,' + ((0..($cells.Length - 1) | ForEach-Object { "c$_" }) -join ','))
$period = 1000.0 / $Fps
$next = [ApexGrab]::QpcMs()
$n = 0
try {
    while (-not $proc.HasExited -and -not ($StopFile -and (Test-Path -LiteralPath $StopFile))) {
        $t = [ApexGrab]::QpcMs()
        [Windows.Forms.Application]::DoEvents()
        if ($t -lt $next) { Start-Sleep -Milliseconds ([int][math]::Max(1, $next - $t)); continue }
        $next = [math]::Max($next + $period, $t)
        $proc.Refresh()
        $b = [ApexGrab]::Grab($proc.MainWindowHandle, $cells)
        if (-not $b) { continue }
        $b.Save((Join-Path $OutDir ('{0:D6}.jpg' -f $n)), $jpeg, $ep); $b.Dispose()
        $csv.WriteLine($t.ToString('F1', [Globalization.CultureInfo]::InvariantCulture) + ',' + [DateTime]::Now.ToString('HH:mm:ss.fff') + ",$n," + (($cells | ForEach-Object { $_.ToString('F1', [Globalization.CultureInfo]::InvariantCulture) }) -join ','))
        $n++
    }
} finally { $csv.Dispose(); $pin.Close() }
