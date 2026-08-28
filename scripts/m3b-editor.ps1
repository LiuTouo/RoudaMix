# M4a 驗證:VST3 editor 視窗(open/close、冪等、engine process 出現/消失 top-level 視窗、
# 開著 editor 時 remove_plugin 不死鎖)
# 前置:engine 已跑(或此腳本自行 spawn);Common Files\VST3 有至少一個 .vst3(無 editor 也可,走 no-editor 路徑)
# 判定:任何一步錯 = throw(非零退出);全過印 PASSED
$ErrorActionPreference = 'Stop'
$engineExe = Join-Path $PSScriptRoot '..\engine\build\Release\roudamix-engine.exe'

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class RmxWinEnum {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    delegate bool EnumProc(IntPtr h, IntPtr lp);
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
    Send-Frame $s @{ protocolVersion = 1; id = $script:id; kind = $kind; payload = $payload }
    while ($true) {
        $f = Read-Frame $s  # 跳過 event 幀
        if ($f.id -ne $null) {
            if ($f.id -eq 0 -and $f.ok -eq $false) { throw "parse error: $($f.error.message)" }
            if ($f.id -eq $script:id) {
                if (-not $f.ok) { throw "$kind failed: $($f.error.code) $($f.error.message)" }
                return $f
            }
        }
    }
}
function Try-Command($s, $kind, $payload) {
    $script:id++
    Send-Frame $s @{ protocolVersion = 1; id = $script:id; kind = $kind; payload = $payload }
    while ($true) {
        $f = Read-Frame $s
        if ($f.id -ne $null -and $f.id -eq $script:id) { return $f }
    }
}
function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }
function Wait-EditorWindows($enginePid, $wantCount, $ms) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($ms)
    while ([DateTime]::UtcNow -lt $deadline) {
        $wins = [RmxWinEnum]::VisibleWindowsOfPid($enginePid, 'RmxVST3Editor')
        if ($wins.Count -eq $wantCount) { return $wins }
        Start-Sleep -Milliseconds 100
    }
    return ,([RmxWinEnum]::VisibleWindowsOfPid($enginePid, 'RmxVST3Editor'))
}

# --- 0. engine 連線 + 清場 ---
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(1500) } catch {
    # Connect timeout 不一定代表沒 engine:單 client 協議下 pipe 被佔住也會 timeout。
    $running = Get-Process roudamix-engine -ErrorAction SilentlyContinue
    if ($running) { throw "engine running but pipe busy (another client holds it) - close the UI first" }
    if (-not (Test-Path $engineExe)) { throw "engine exe not found: $engineExe" }
    Start-Process -FilePath $engineExe -WindowStyle Hidden | Out-Null
    Start-Sleep -Milliseconds 800
    $pipe.Connect(3000)
}
$engineProc = Get-Process roudamix-engine -ErrorAction Stop
$enginePid = $engineProc.Id
$snap = Read-Frame $pipe
Assert ($snap.kind -eq 'snapshot') "first frame is snapshot"
if ($snap.payload.status.running) { [void](Command $pipe 'stop' @{}) }
foreach ($slot in @($snap.payload.status.rack)) {
    [void](Command $pipe 'remove_plugin' @{ instanceId = [uint32]$slot.instanceId })
}
Assert ((Wait-EditorWindows $enginePid 0 500).Count -eq 0) 'no editor window at start'

# --- 1. 掃描 + 加入:挑第一個有 editor 的 plugin(全沒有才走 no-editor 路徑)---
$scan = Command $pipe 'scan_plugins' @{}
$mods = $scan.result.plugins
Assert ($mods.Count -ge 1) 'scan found >=1 module'
Write-Host "scan: $($mods.Count) modules"
$mods | ForEach-Object { Write-Host "  module: $([IO.Path]::GetFileName($_.path)) -> $($_.classes.name -join ', ')" }
$hasEditor = $false
$inst = 0
$noEditorCount = 0
foreach ($m in $mods) {
    foreach ($c in @($m.classes)) {
        $a = Command $pipe 'add_plugin' @{ path = $m.path; classId = $c.uid }
        $inst = $a.result.instanceId
        $oe = Try-Command $pipe 'open_editor' @{ instanceId = $inst }
        if ($oe.ok) {
            $hasEditor = $true
            Write-Host "editor plugin: $($c.name) ($([IO.Path]::GetFileName($m.path))) [tried $noEditorCount without editor]"
            break
        }
        Assert ($oe.error.code -eq 'plugin_no_editor') "open_editor fails with plugin_no_editor (got $($oe.error.code))"
        $noEditorCount++
        [void](Command $pipe 'close_editor' @{ instanceId = $inst })  # 冪等先驗
        [void](Command $pipe 'remove_plugin' @{ instanceId = $inst })
    }
    if ($hasEditor) { break }
    $inst = 0  # 迴圈尾:已 remove,後續步驟不可再用
}

# --- 2. open_editor:視窗出現在 engine process(冪等 = 同視窗)---
if ($hasEditor) {
    $wins = Wait-EditorWindows $enginePid 1 3000
    Assert ($wins.Count -eq 1) "engine has 1 visible editor window after open (got $($wins.Count))"
    Write-Host "open_editor: window hwnd=$($wins[0])"
    [void](Command $pipe 'open_editor' @{ instanceId = $inst })  # 冪等:再開一次 ok
    $wins2 = Wait-EditorWindows $enginePid 1 1000
    Assert ($wins2.Count -eq 1) "idempotent open keeps exactly 1 window (got $($wins2.Count))"
}

# --- 3. close_editor:視窗消失;冪等 ---
if ($hasEditor) {
    [void](Command $pipe 'close_editor' @{ instanceId = $inst })
    $gone = Wait-EditorWindows $enginePid 0 3000
    Assert ($gone.Count -eq 0) "editor window gone after close (got $($gone.Count))"
    [void](Command $pipe 'close_editor' @{ instanceId = $inst })  # 冪等
    Write-Host 'close_editor: window gone, idempotent ok'
}

# --- 4. editor 開著直接 remove_plugin(死鎖回歸測試:reply 必須回得來)---
if ($hasEditor) {
    [void](Command $pipe 'open_editor' @{ instanceId = $inst })
    $wins = Wait-EditorWindows $enginePid 1 3000
    Assert ($wins.Count -eq 1) 'editor reopened'
    $rm = Command $pipe 'remove_plugin' @{ instanceId = $inst }  # 卡死 = 這行永遠不回
    Assert ($rm.result.rack.Count -eq 0) 'rack empty after remove'
    $gone = Wait-EditorWindows $enginePid 0 3000
    Assert ($gone.Count -eq 0) 'editor window closed by remove_plugin'
    Write-Host 'remove_plugin with editor open: no deadlock, window cleaned'
}

# --- 5. editor 指令對不存在的 instance ---
$bad = Try-Command $pipe 'open_editor' @{ instanceId = 9999 }
Assert ($bad.ok -eq $false -and $bad.error.code -eq 'plugin_not_found') "open_editor unknown id -> plugin_not_found (got $($bad.error.code))"
$bad2 = Try-Command $pipe 'close_editor' @{ instanceId = 9999 }
Assert ($bad2.ok -eq $false -and $bad2.error.code -eq 'plugin_not_found') 'close_editor unknown id -> plugin_not_found'

# --- 6. 清場 ---
$snapEnd = Try-Command $pipe 'get_snapshot' @{}
foreach ($slot in @($snapEnd.result.snapshot.status.rack)) {
    [void](Command $pipe 'remove_plugin' @{ instanceId = [uint32]$slot.instanceId })
}
Assert ((Wait-EditorWindows $enginePid 0 1000).Count -eq 0) 'no editor window at end'
$pipe.Dispose()
Write-Host 'PASSED'
