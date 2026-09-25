# rmx-glass-probe.ps1 — throwaway: open editor over color tiles, screenshot,
# analyze strip/caption/client bands for blur vs solid + flicker burst on open.
$ErrorActionPreference = 'Stop'
$engineExe = 'D:\Administrator\Documents\GitHub\RoudaMix\engine\build\Release\roudamix-engine.exe'
$vstDir = 'C:\Program Files\Common Files\VST3'
$prefer = @('DuskVerb.vst3', 'rnnoise.vst3', 'IA DeEsser.vst3')
$outDir = "$env:TEMP\rmx-glass-probe"
New-Item -ItemType Directory -Force $outDir | Out-Null

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class RmxProbe {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int cb);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr CreateWindowExW(int ex, string cls, string name, int style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandleW(string name);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern ushort RegisterClassExW(ref WNDCLASSEX w);
    delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct WNDCLASSEX { public int cb; public uint style; public WndProc proc; public int clsExtra, wndExtra; public IntPtr inst, icon, cursor, bg; public string menu, name; public IntPtr smIcon; }
    static IntPtr DefProc(IntPtr h, uint m, IntPtr w, IntPtr l) { return DefWindowProcW(h, m, w, l); }
    public static void RegisterTile(string name, int colorRef) {
        var wc = new WNDCLASSEX();
        wc.cb = Marshal.SizeOf(typeof(WNDCLASSEX));
        wc.proc = DefProc; wc.inst = GetModuleHandleW(null);
        wc.bg = CreateSolidBrush(colorRef); wc.name = name;
        RegisterClassExW(ref wc);
    }
    [DllImport("user32.dll")] public static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateSolidBrush(int c);
    public static List<long> VisibleWindowsOfPid(uint pid, string cls) {
        var result = new List<long>();
        EnumWindows((h, lp) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h)) {
                var sb = new StringBuilder(256); GetClassName(h, sb, 256);
                if (sb.ToString() == cls) result.Add(h.ToInt64());
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@

function Read-Frame($s) {
    $len = New-Object byte[] 4; $read = 0
    while ($read -lt 4) { $n = $s.Read($len, $read, 4 - $read); if ($n -le 0) { throw 'EOF at len' }; $read += $n }
    $size = [BitConverter]::ToUInt32($len, 0)
    $body = New-Object byte[] $size; $read = 0
    while ($read -lt $size) { $n = $s.Read($body, $read, $size - $read); if ($n -le 0) { throw 'EOF at body' }; $read += $n }
    return [System.Text.Encoding]::UTF8.GetString($body) | ConvertFrom-Json
}
function Send-Frame($s, $obj) {
    $json = $obj | ConvertTo-Json -Depth 10 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $len = [BitConverter]::GetBytes([UInt32]$bytes.Length)
    $s.Write($len, 0, 4); $s.Write($bytes, 0, $bytes.Length); $s.Flush()
}
$script:id = 0
function Command($s, $kind, $payload) {
    $script:id++
    Send-Frame $s @{ protocolVersion = 2; id = $script:id; kind = $kind; payload = $payload }
    while ($true) {
        $f = Read-Frame $s
        if ($f.id -ne $null -and $f.id -eq $script:id) {
            if (-not $f.ok) { throw "$kind failed: $($f.error.code) $($f.error.message)" }
            return $f
        }
    }
}
function Try-Command($s, $kind, $payload) {
    $script:id++
    Send-Frame $s @{ protocolVersion = 2; id = $script:id; kind = $kind; payload = $payload }
    while ($true) { $f = Read-Frame $s; if ($f.id -ne $null -and $f.id -eq $script:id) { return $f } }
}
function Wait-Editor($enginePid, $ms) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($ms)
    while ([DateTime]::UtcNow -lt $deadline) {
        $wins = [RmxProbe]::VisibleWindowsOfPid($enginePid, 'RmxVST3Editor')
        if ($wins.Count -ge 1) { return $wins[0] }
        Start-Sleep -Milliseconds 50
    }
    return 0
}

Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$LOG = "$env:TEMP\rmx-glass-probe\probe.log"
Set-Content $LOG "probe start $(Get-Date -Format T)"
function Write-Host {  # 逐行落盤:重導向緩衝會藏掛點
    param([string]$m)
    Add-Content -LiteralPath $LOG "[$(Get-Date -Format T)] $m"
    Microsoft.PowerShell.Utility\Write-Host $m
}
Start-Process -FilePath $engineExe -WindowStyle Hidden -RedirectStandardError "$outDir\eng.err" | Out-Null
Start-Sleep -Milliseconds 900
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(3000)
$enginePid = (Get-Process roudamix-engine -ErrorAction Stop).Id
$snap = Read-Frame $pipe
Add-Content $LOG "snapshot read ok kind=$($snap.kind)"
Write-Host "engine pid=$enginePid, snapshot kind=$($snap.kind)"

# fx track + first preferred plugin with an editor
$ta = Command $pipe 'track_add' @{ kind = 'fx' }
Add-Content $LOG "track_add ok id=$($ta.result.trackId)"
$trackId = $ta.result.trackId
$inst = 0; $picked = $null
foreach ($f in ($prefer + @())) {
    $p = Join-Path $vstDir $f
    if (-not (Test-Path $p)) { continue }
    Add-Content $LOG "trying $f"
    $ap = Command $pipe 'add_plugin' @{ trackId = $trackId; path = $p }
    Add-Content $LOG "add_plugin $f -> inst=$($ap.result.instanceId)"
    $inst = $ap.result.instanceId
    $oe = Try-Command $pipe 'open_editor' @{ instanceId = $inst }
    Add-Content $LOG "open_editor ok=$($oe.ok)"
    if ($oe.ok) { $picked = $f; break }
    [void](Command $pipe 'remove_plugin' @{ instanceId = $inst }); $inst = 0
}
if (-not $picked) { throw 'no preferred plugin editor; check eng.err' }
Write-Host "editor plugin: $picked (inst=$inst)"
$hwndVal = Wait-Editor $enginePid 4000
Add-Content $LOG "wait-editor -> $hwndVal"
if ($hwndVal -eq 0) { throw 'editor window not visible' }
$hwnd = [IntPtr]$hwndVal
Start-Sleep -Milliseconds 500

# deterministic position (200,200)
[void][RmxProbe]::SetWindowPos($hwnd, [IntPtr](-1), 200, 200, 0, 0, 0x0002 -bor 0x0004 -bor 0x0010)  # NOSIZE|NOZORDER(topmost)|NOACTIVATE
Start-Sleep -Milliseconds 200

# color tiles behind editor
$ef = New-Object RmxProbe+RECT
[void][RmxProbe]::DwmGetWindowAttribute($hwnd, 9, [ref]$ef, [Runtime.InteropServices.Marshal]::SizeOf($ef))
$w = $ef.R - $ef.L; $h = $ef.B - $ef.T
Add-Content $LOG "bounds ${w}x${h}"
$colors = @(0x0000D0, 0x00D000, 0xD00000, 0xD0D000, 0xD000D0, 0x00D0D0)
$tiles = @()
$i = 0
for ($ty = 0; $ty -lt 2; $ty++) {
    for ($tx = 0; $tx -lt 3; $tx++) {
        $name = "RmxTile$i"
        [RmxProbe]::RegisterTile($name, $colors[$i])
        $tw = [Math]::Max(280, [Math]::Ceiling($w / 3)); $th = [Math]::Max(230, [Math]::Ceiling($h / 2))
        $t = [RmxProbe]::CreateWindowExW(0x8, $name, '', 0x80000000 -bor 0x10000000, $ef.L + $tx * 280, $ef.T + $ty * 230, $tw, $th, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
        $tiles += $t; $i++
    }
}
Start-Sleep -Milliseconds 300
[void][RmxProbe]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0010)  # NOSIZE|NOMOVE|topmost
Start-Sleep -Milliseconds 600

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($ef.L, $ef.T, 0, 0, $bmp.Size); $g.Dispose()
$png = "$outDir\editor.png"
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
Add-Content $LOG "shot saved $png"

function Analyze-Band($bmp, $y0, $y1, $label) {
    $spread = 0.0; $n = 0; $maxS = 0
    for ($y = $y0; $y -lt $y1; $y += 2) {
        for ($x = 4; $x -lt ($bmp.Width - 4); $x += 3) {
            $c = $bmp.GetPixel($x, $y)
            $mx = [Math]::Max($c.R, [Math]::Max($c.G, $c.B)); $mn = [Math]::Min($c.R, [Math]::Min($c.G, $c.B))
            $s = $mx - $mn; $spread += $s; $n++
            if ($s -gt $maxS) { $maxS = $s }
        }
    }
    $avg = if ($n) { $spread / $n } else { -1 }
    $verdict = if ($avg -gt 12) { 'GLASS(color-mix)' } elseif ($avg -lt 3) { 'flat/solid' } else { 'weak' }
    Add-Content $LOG ("BAND {0} y={1}-{2} avgSpread={3:N1} max={4} {5}" -f $label, $y0, $y1, $avg, $maxS, $verdict)
}
# caption ~ y 0..captionH(≈32), strip = caption..caption+64, client below
[void](Analyze-Band $bmp 4 26 'caption')
[void](Analyze-Band $bmp 36 92 'strip')
[void](Analyze-Band $bmp ($h - 40) ($h - 6) 'client')

# flicker burst around reopen
[void](Command $pipe 'close_editor' @{ instanceId = $inst })
Start-Sleep -Milliseconds 400
$frames = @()
for ($k = 0; $k -lt 14; $k++) {
    if ($k -eq 3) { [void](Command $pipe 'open_editor' @{ instanceId = $inst }) }
    $fb = New-Object System.Drawing.Bitmap $w, $h
    $fg = [System.Drawing.Graphics]::FromImage($fb)
    $fg.CopyFromScreen($ef.L, $ef.T, 0, 0, $fb.Size); $fg.Dispose()
    $frames += ,$fb
    Start-Sleep -Milliseconds 30
}
$changes = 0; $maxDiff = 0.0
for ($k = 1; $k -lt $frames.Count; $k++) {
    $diff = 0.0; $n = 0
    for ($y = 2; $y -lt ($h - 2); $y += 6) {
        for ($x = 2; $x -lt ($w - 2); $x += 6) {
            $a = $frames[$k-1].GetPixel($x, $y); $b = $frames[$k].GetPixel($x, $y)
            $diff += [Math]::Abs($a.R - $b.R) + [Math]::Abs($a.G - $b.G) + [Math]::Abs($a.B - $b.B); $n++
        }
    }
    $d = $diff / $n
    if ($d -gt 4) { $changes++ }
    if ($d -gt $maxDiff) { $maxDiff = $d }
    Add-Content $LOG ("FRAME {0} meanDiff={1:N1}" -f $k, $d)
}
Add-Content $LOG "FLICKER $changes/13 max=$([Math]::Round($maxDiff,1))"
foreach ($t in $tiles) { [void][RmxProbe]::DestroyWindow($t) }
[void](Command $pipe 'close_editor' @{ instanceId = $inst })
Start-Sleep -Milliseconds 200
[void](Command $pipe 'remove_plugin' @{ instanceId = $inst })
$pipe.Dispose()
Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'PROBE DONE'
