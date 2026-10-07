# 診斷:VST3 editor host 視窗的視窗樹/矩形量測 + 截圖。
# 找出「原生編輯器內容被遮擋」是被誰擋住(client 裁切?tabs 帶壓住?plugin 子視窗比 client 大?)
# 用法:pwsh scripts/diag-editor-occlusion.ps1 [-Keep]
param([switch]$Keep)
$ErrorActionPreference = 'Stop'
function Step($msg) { Write-Host ("[step] " + $msg) }
$engineExe = Join-Path $PSScriptRoot '..\engine\build\Release\roudamix-engine.exe'
$shotDir = Join-Path $env:TEMP 'rmx-editor-diag'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class RmxDiag {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string name);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    delegate bool EnumProc(IntPtr h, IntPtr lp);
    public static List<long> WindowsOfPid(uint pid, string cls) {
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
    public static string Cls(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
    public static string Rect(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return "(" + r.L + "," + r.T + ")-(" + r.R + "," + r.B + ") " + (r.R - r.L) + "x" + (r.B - r.T);
    }
    // 列出 h 的直接子視窗(class + rect + 相對 parent client 的位置)
    public static List<string> Children(IntPtr h) {
        var list = new List<string>();
        IntPtr prev = IntPtr.Zero;
        while (true) {
            IntPtr c = FindWindowEx(h, prev, null, null);
            if (c == IntPtr.Zero) break;
            list.Add(Cls(c) + " hwnd=" + c + " rect=" + Rect(c) + " z_top");
            prev = c;
        }
        return list;
    }
    // h 的子視窗樹(深遞歸,標縮排)
    public static void Tree(IntPtr h, int depth, List<string> outList, int maxDepth) {
        if (depth > maxDepth) return;
        IntPtr prev = IntPtr.Zero;
        while (true) {
            IntPtr c = FindWindowEx(h, prev, null, null);
            if (c == IntPtr.Zero) break;
            string pad = new string(' ', depth * 2);
            outList.Add(pad + Cls(c) + " vis=" + IsWindowVisible(c) + " rect=" + Rect(c));
            Tree(c, depth + 1, outList, maxDepth);
            prev = c;
        }
    }
}
'@
Add-Type -AssemblyName System.Drawing

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
        if ($f.id -ne $null) {
            if ($f.id -eq 0 -and $f.ok -eq $false) { throw "parse error: $($f.error.message)" }
            if ($f.id -eq $script:id) {
                if (-not $f.ok) { throw "$kind failed: $($f.error.code) $($f.error.message)" }
                return $f
            }
        }
    }
}
function Wait-Event($s, $name, $ms) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($ms)
    while ([DateTime]::UtcNow -lt $deadline) {
        $f = Read-Frame $s
        if ($f.kind -eq $name) { return $f }
        if ($f.id -ne $null -and $f.ok -eq $false) { throw "error frame: $($f.error.message)" }
    }
    throw "timeout waiting $name"
}

# --- 0. engine 連線 ---
Step 'connecting pipe'
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(1500) } catch {
    $running = Get-Process roudamix-engine -ErrorAction SilentlyContinue
    if ($running) { throw "engine running but pipe busy - close the UI first" }
    if (-not (Test-Path $engineExe)) { throw "engine exe not found: $engineExe" }
    Start-Process -FilePath $engineExe -WindowStyle Hidden -RedirectStandardError "$env:TEMP\rmx-diag-eng.err" | Out-Null
    Start-Sleep -Milliseconds 800
    $pipe.Connect(3000)
}
$engineProc = Get-Process roudamix-engine -ErrorAction Stop
$enginePid = $engineProc.Id
Step 'reading first snapshot'
$snap = Read-Frame $pipe
Step 'snapshot read'
if ($snap.payload.status.running) { [void](Command $pipe 'stop' @{}) }
foreach ($t in @($snap.payload.tracks)) {
    foreach ($slot in @($t.plugins)) {
        [void](Command $pipe 'remove_plugin' @{ instanceId = [uint32]$slot.instanceId })
    }
}

# --- 1. 掃描 ---
Step 'start_scan'
$scan = Command $pipe 'start_scan' @{}
$jobId = $scan.result.jobId
Step 'waiting scan_done'
$done = Wait-Event $pipe 'scan_done' 120000
$mods = @($done.payload.plugins)
Write-Host "scan: $($mods.Count) modules"

# --- 2. 清場 + 加 2 條軌、每軌 1 個有 editor 的 plugin ---
Step 'track_add x2'
$trackIds = @()
foreach ($nm in @('DiagA', 'DiagB')) {
    $r = Command $pipe 'track_add' @{ kind = 'audio'; name = $nm }
    $trackIds += [uint32]$r.result.trackId
}
$tracks = $null
Step ("trackIds: " + ($trackIds -join ','))
$added = 0
$picked = @()
foreach ($m in $mods) {
    if ($added -ge 2) { break }
    if ($picked -contains $m.path) { continue }
    try {
        Step ("add_plugin $([IO.Path]::GetFileName($m.path))")
        $a = Command $pipe 'add_plugin' @{ trackId = $trackIds[$added]; path = $m.path }
        $oe = $null
        try { $oe = Command $pipe 'open_editor' @{ instanceId = [uint32]$a.result.instanceId } } catch { }
        if ($oe -ne $null) {
            $picked += $m.path
            $added++
            Write-Host "picked: $([IO.Path]::GetFileName($m.path)) instanceId=$($a.result.instanceId)"
        } else {
            [void](Command $pipe 'remove_plugin' @{ instanceId = [uint32]$a.result.instanceId })
        }
    } catch { Write-Host "skip $([IO.Path]::GetFileName($m.path)): $_" }
}
if ($added -lt 2) { Write-Host "only $added editor-capable plugin added (ok, continue)" }

Step 'measuring windows'
Start-Sleep -Milliseconds 1200
$wins = [RmxDiag]::WindowsOfPid($enginePid, 'RmxVST3Editor')
Write-Host "`n=== editor host windows: $($wins.Count) ==="
foreach ($w in $wins) {
    Write-Host "host hwnd=$w rect=$([RmxDiag]::Rect([IntPtr]$w))"
    $tree = New-Object System.Collections.Generic.List[string]
    [RmxDiag]::Tree([IntPtr]$w, 1, $tree, 4)
    foreach ($line in $tree) { Write-Host $line }
    # 截圖(整窗)
    $r = New-Object RmxDiag+RECT
    [void][RmxDiag]::GetWindowRect([IntPtr]$w, [ref]$r)
    $wd = $r.R - $r.L; $ht = $r.B - $r.T
    if ($wd -gt 0 -and $ht -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap($wd, $ht)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $file = Join-Path $shotDir ("editor-{0}.png" -f (Get-Date -Format 'HHmmss-fff'))
        $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Host "screenshot: $file"
    }
}
if (-not $Keep) {
    $snap2 = (Command $pipe 'get_snapshot' @{}).result.snapshot
    foreach ($t in @($snap2.tracks)) {
        foreach ($slot in @($t.plugins)) {
            [void](Command $pipe 'remove_plugin' @{ instanceId = [uint32]$slot.instanceId })
        }
    }
    [void](Command $pipe 'shutdown_engine' @{})
}
Write-Host "`nDONE"
