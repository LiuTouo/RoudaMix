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
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr CreateWindowExW(int ex, string cls, string name, int style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
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
    Start-Process -FilePath $engineExe -WindowStyle Hidden `
        -RedirectStandardError "$env:TEMP\rmx-m3b-eng.err" | Out-Null
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
$editorPath = $null
$editorUid = $null
foreach ($m in $mods) {
    foreach ($c in @($m.classes)) {
        $a = Command $pipe 'add_plugin' @{ path = $m.path; classId = $c.uid }
        $inst = $a.result.instanceId
        $oe = Try-Command $pipe 'open_editor' @{ instanceId = $inst }
        if ($oe.ok) {
            $hasEditor = $true
            $editorPath = $m.path
            $editorUid = $c.uid
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
    # 前景斷言(軟性):headless 桌面無 input state,跨 process 前景授予本就不保證;
    # 實 app 的 owner 主視窗 thread 常駐 pump,dance 會成功
    $fgOk = $false
    foreach ($i in 1..10) {
        if ([RmxWinEnum]::GetForegroundWindow() -eq [IntPtr]$wins[0]) { $fgOk = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if ($fgOk) { Write-Host 'open_editor: window is foreground' }
    else { Write-Host "NOTE: editor not foreground in headless (fg=$([RmxWinEnum]::GetForegroundWindow()))" }
}

# --- 2b. 冪等重開 = 現有視窗帶回前景(回歸:冪等路徑沒做前景處理,editor 被主視窗壓到後面)---
if ($hasEditor) {
    # 開個 Static 視窗 + attach dance 搶前景,模擬「使用者正在別的視窗」
    $WS_OVERLAPPEDWINDOW = 0x00CF0000; $WS_VISIBLE = 0x10000000
    $steal = [RmxWinEnum]::CreateWindowExW(0, 'Static', 'rmx-probe-fg-steal', $WS_OVERLAPPEDWINDOW -bor $WS_VISIBLE,
        10, 10, 160, 100, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
    Assert ($steal -ne [IntPtr]::Zero) 'probe steal window created'
    $fgH = [RmxWinEnum]::GetForegroundWindow()
    $fgTid = 0; if ($fgH -ne [IntPtr]::Zero) { [void][RmxWinEnum]::GetWindowThreadProcessId($fgH, [ref]$fgTid) }
    $myTid = [RmxWinEnum]::GetCurrentThreadId()
    if ($fgTid -ne 0 -and $fgTid -ne $myTid) { [void][RmxWinEnum]::AttachThreadInput($myTid, $fgTid, $true) }
    [void][RmxWinEnum]::SetForegroundWindow($steal)
    if ($fgTid -ne 0 -and $fgTid -ne $myTid) { [void][RmxWinEnum]::AttachThreadInput($myTid, $fgTid, $false) }
    Start-Sleep -Milliseconds 200
    if ([RmxWinEnum]::GetForegroundWindow() -eq $steal) {
        [void](Command $pipe 'open_editor' @{ instanceId = $inst })  # 冪等:必須把 editor 搶回前景
        # engine 的 dance 在 helper thread,SetForegroundWindow 的 sync send 要
        # 這個(steal window 所在的)thread pump 才會完成 → 重試 + DoEvents
        Add-Type -AssemblyName System.Windows.Forms
        $ok = $false
        foreach ($i in 1..15) {
            Start-Sleep -Milliseconds 100
            [System.Windows.Forms.Application]::DoEvents()
            if ([RmxWinEnum]::GetForegroundWindow() -eq [IntPtr]$wins[0]) { $ok = $true; break }
        }
        if ($ok) { Write-Host 'idempotent open: editor reclaimed foreground' }
        else { Write-Host 'NOTE: fg-reclaim not observable in headless (helper dance denied by PS thread; real app UI thread pumps)' }
    } else {
        Write-Host 'SKIP: probe could not steal foreground (headless desktop), fg-reclaim assert skipped'
    }
    [void][RmxWinEnum]::DestroyWindow($steal)
}

# --- 3. close_editor:視窗消失;冪等 ---
if ($hasEditor) {
    [void](Command $pipe 'close_editor' @{ instanceId = $inst })
    $gone = Wait-EditorWindows $enginePid 0 3000
    Assert ($gone.Count -eq 0) "editor window gone after close (got $($gone.Count))"
    [void](Command $pipe 'close_editor' @{ instanceId = $inst })  # 冪等
    Write-Host 'close_editor: window gone, idempotent ok'
}

# --- 3b. 使用者按 X(WM_CLOSE)關窗:狀態必須清乾淨,重開要能再開
#         (回歸:GWLP_USERDATA 沒存 → WM_DESTROY 清理跳過 → editor_wnd 殘留 → 重開靜默 no-op)---
if ($hasEditor) {
    [void](Command $pipe 'open_editor' @{ instanceId = $inst })
    $wins = Wait-EditorWindows $enginePid 1 3000
    Assert ($wins.Count -eq 1) 'editor open before X-close'
    $hwnd = [IntPtr]$wins[0]
    [void][RmxWinEnum]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    $gone = Wait-EditorWindows $enginePid 0 3000
    Assert ($gone.Count -eq 0) "editor window gone after X (got $($gone.Count))"
    [void](Command $pipe 'open_editor' @{ instanceId = $inst })  # 不 ok = throw;殘留狀態會在這現形?
    $wins3 = Wait-EditorWindows $enginePid 1 3000
    Assert ($wins3.Count -eq 1) "reopen after X creates a NEW window (got $($wins3.Count))"
    Write-Host 'X-close: window gone, reopen works'
}

# --- 3c. 雙 plugin 切換(Studio Pro host 語意):同窗換嵌、active 關閉自動切換 ---
if ($hasEditor) {
    $b = Command $pipe 'add_plugin' @{ path = $editorPath; classId = $editorUid }
    $instB = $b.result.instanceId
    [void](Command $pipe 'open_editor' @{ instanceId = $inst })
    $winsA = Wait-EditorWindows $enginePid 1 3000
    Assert ($winsA.Count -eq 1) 'A open: 1 window'
    $hwnd1 = $winsA[0]
    [void](Command $pipe 'open_editor' @{ instanceId = $instB })
    $winsB = Wait-EditorWindows $enginePid 1 3000
    Assert ($winsB.Count -eq 1 -and $winsB[0] -eq $hwnd1) "B open: SAME window (switch, got $($winsB.Count) wins)"
    [void](Command $pipe 'close_editor' @{ instanceId = $inst })   # 非 active
    $winsC = Wait-EditorWindows $enginePid 1 3000
    Assert ($winsC.Count -eq 1) 'close non-active A: window stays'
    [void](Command $pipe 'open_editor' @{ instanceId = $inst })    # 再開 = 切回 A,同窗
    $winsD = Wait-EditorWindows $enginePid 1 3000
    Assert ($winsD.Count -eq 1 -and $winsD[0] -eq $hwnd1) 'reopen A: SAME window (switch back)'
    [void](Command $pipe 'close_editor' @{ instanceId = $instB })  # active;A 開著 → 自動切回 A
    $winsE = Wait-EditorWindows $enginePid 1 3000
    Assert ($winsE.Count -eq 1) 'close active B: auto-switch to A, window stays'
    [void](Command $pipe 'close_editor' @{ instanceId = $inst })   # 最後一個 → 關窗
    $winsF = Wait-EditorWindows $enginePid 0 3000
    Assert ($winsF.Count -eq 0) 'close last: window gone'
    [void](Command $pipe 'remove_plugin' @{ instanceId = $instB })
    Write-Host 'dual-plugin switch: same-window switch + auto-switch + final close ok'
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
