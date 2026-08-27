# M3 驗證:session save/load roundtrip(真 plugin)+ deviceKey override + 壞檔拒絕
# 前置:engine 已跑(或此腳本自行 spawn);ASIO 裝置存在;Common Files\VST3 有至少一個 .vst3
# 判定:任何一步錯 = throw(非零退出);全過印 PASSED
$ErrorActionPreference = 'Stop'
$engineExe = Join-Path $PSScriptRoot '..\engine\build\Release\roudamix-engine.exe'

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
function Clear-Rack($s, $status) {
    if ($status.running) { [void](Command $s 'stop' @{}) }
    foreach ($slot in @($status.rack)) {
        [void](Command $s 'remove_plugin' @{ instanceId = [uint32]$slot.instanceId })
    }
}

# --- 0. engine 連線 + 清場 ---
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(1500) } catch {
    # Connect timeout 不一定代表沒 engine:單 client 協議下 pipe 被佔住也會 timeout。
    # 只在真的沒有 engine process 時才 spawn。
    $running = Get-Process roudamix-engine -ErrorAction SilentlyContinue
    if ($running) { throw "engine running but pipe busy (another client holds it) - close the UI first" }
    if (-not (Test-Path $engineExe)) { throw "engine exe not found: $engineExe" }
    Start-Process -FilePath $engineExe -WindowStyle Hidden | Out-Null
    Start-Sleep -Milliseconds 800
    $pipe.Connect(3000)
}
$snap = Read-Frame $pipe
Assert ($snap.kind -eq 'snapshot') "first frame is snapshot"
Clear-Rack $pipe $snap.payload.status

# --- 1. 掃描 + 加入兩個 slot(不同 class 或同 class 兩 instance)---
$scan = Command $pipe 'scan_plugins' @{}
$mods = $scan.result.plugins
Assert ($mods.Count -ge 1) 'scan found >=1 module'
$first = $mods[0]
$dev = Command $pipe 'list_devices' @{}
Assert ($dev.result.devices.Count -ge 1) 'ASIO device present'
$key = $dev.result.devices[0].deviceKey
Write-Host "scan: $($mods.Count) modules, device=$key"

$add1 = Command $pipe 'add_plugin' @{ path = $first.path; classId = $first.classes[0].uid }
$add2 = Command $pipe 'add_plugin' @{ path = $first.path; classId = $first.classes[0].uid }
$inst1 = $add1.result.instanceId; $inst2 = $add2.result.instanceId
Assert ($inst1 -ne $inst2) 'two distinct instances'
Write-Host "add: $inst1, $inst2 ($($add1.result.rack[0].name))"

# --- 2. 參數 + bypass 狀態(slot1: param 0.5;bypass slot2)---
$gp = Command $pipe 'get_params' @{ instanceId = $inst1 }
$p0 = $gp.result.params | Where-Object { -not $_.bypass } | Select-Object -First 1
Assert ($null -ne $p0) 'has non-bypass param'
[void](Command $pipe 'set_param' @{ instanceId = $inst1; paramId = $p0.paramId; value = 0.5 })
[void](Command $pipe 'set_bypass' @{ instanceId = $inst2; bypassed = $true })
# --- 3. source: sine 880 ---
[void](Command $pipe 'set_source' @{ source = 'sine'; sineFreq = 880 })
Write-Host "state: param $($p0.paramId)=0.5, slot2 bypassed, sine 880"

# --- 4. start 過一次再 stop(engine 記住 last device)---
$st = Command $pipe 'start' @{ deviceKey = $key; sampleRate = 48000 }
Assert ($st.result.running) 'running'
[void](Command $pipe 'stop' @{})

# --- 5. save(不帶 override)→ 檔內 deviceKey = 該裝置 ---
$sessionDir = Join-Path $env:TEMP 'rmx-m3-session'
New-Item -ItemType Directory -Force $sessionDir | Out-Null
$f1 = Join-Path $sessionDir 'plain.rmsession'
$sv = Command $pipe 'save_session' @{ path = $f1 }
Assert ($sv.result.savedPath -eq $f1) 'savedPath echoed'
$doc = Get-Content $f1 -Raw | ConvertFrom-Json
Assert ($doc.roudamixSession -eq 1) 'file version 1'
Assert ($doc.deviceKey -eq $key) "plain save deviceKey = started device (got '$($doc.deviceKey)')"
Assert ($doc.sampleRate -eq 48000) 'plain save sampleRate = 48000'
Assert ($doc.sineFreq -eq 880) 'file sineFreq 880'
Assert ($doc.rack.Count -eq 2) 'file rack 2 slots'
Write-Host "save(plain): deviceKey=$($doc.deviceKey) rate=$($doc.sampleRate) rack=$($doc.rack.Count)"

# --- 6. save 帶 override(UI 語意:帶目前選的)---
$f2 = Join-Path $sessionDir 'override.rmsession'
[void](Command $pipe 'save_session' @{ path = $f2; deviceKey = 'asio:fake-dev'; sampleRate = 44100 })
$doc2 = Get-Content $f2 -Raw | ConvertFrom-Json
Assert ($doc2.deviceKey -eq 'asio:fake-dev') "override deviceKey written (got '$($doc2.deviceKey)')"
Assert ($doc2.sampleRate -eq 44100) 'override sampleRate written'
Write-Host "save(override): deviceKey=$($doc2.deviceKey) rate=$($doc2.sampleRate)"

# --- 7. 清場(模擬換 session)+ load plain ---
$snapNow = Try-Command $pipe 'get_snapshot' @{}
Clear-Rack $pipe $snapNow.result.snapshot.status
[void](Command $pipe 'set_source' @{ source = 'sine'; sineFreq = 440 })
$ld = Command $pipe 'load_session' @{ path = $f1 }
$rack = $ld.result.rack  # load 成功 = mutation 廣播 status;reply result = applied(deviceKey/sampleRate)
$applied = $ld.result
Assert ($applied.deviceKey -eq $key) "applied.deviceKey (got '$($applied.deviceKey)')"
Assert ($applied.sampleRate -eq 48000) 'applied.sampleRate'
# rack 復原驗證(get_snapshot 拿權威狀態)
$gs = Command $pipe 'get_snapshot' @{}
$rack2 = $gs.result.snapshot.status.rack
Assert ($rack2.Count -eq 2) "rack restored 2 slots (got $($rack2.Count))"
Assert ($rack2[0].pluginPath -eq $first.path -and $rack2[1].pluginPath -eq $first.path) 'paths restored'
Assert ($rack2[0].name -eq $rack2[1].name) 'names restored'
Assert ($rack2[0].instanceId -ne $inst1 -and $rack2[1].instanceId -ne $inst2) 'fresh instanceIds'
Assert ($rack2[1].bypassed -eq $true -and $rack2[0].bypassed -eq $false) 'bypass restored (slot2 only)'
$paramRestored = $rack2[0].params | Where-Object { $_.paramId -eq $p0.paramId }
Assert ($paramRestored.normalized -eq 0.5) "param $($p0.paramId) restored 0.5 (got $($paramRestored.normalized))"
Assert ($gs.result.snapshot.status.sineFreq -eq 880) 'sineFreq restored 880'
Write-Host "load(plain): rack=$($rack2.Count) bypass@1=$($rack2[1].bypassed) param=$($paramRestored.normalized) sine=$($gs.result.snapshot.status.sineFreq)"

# --- 8. load override 檔 → applied.deviceKey = 蓋寫值 ---
$ld2 = Command $pipe 'load_session' @{ path = $f2 }
Assert ($ld2.result.deviceKey -eq 'asio:fake-dev') 'override applied.deviceKey'
Write-Host "load(override): applied.deviceKey=$($ld2.result.deviceKey)"

# --- 9. 壞檔:不存在的路徑 → session_io ---
$bad = Try-Command $pipe 'load_session' @{ path = (Join-Path $sessionDir 'nope.rmsession') }
Assert ($bad.ok -eq $false) 'missing file fails'
Assert ($bad.error.code -eq 'session_io') "error code session_io (got $($bad.error.code))"
# 壞內容檔
$badFile = Join-Path $sessionDir 'corrupt.rmsession'
Set-Content $badFile 'not json{'
$bad2 = Try-Command $pipe 'load_session' @{ path = $badFile }
Assert ($bad2.ok -eq $false -and $bad2.error.code -eq 'session_io') 'corrupt file rejected session_io'
Write-Host "bad files: rejected session_io"

# --- 10. 清場 ---
$snapEnd = Try-Command $pipe 'get_snapshot' @{}
Clear-Rack $pipe $snapEnd.result.snapshot.status
[void](Command $pipe 'set_source' @{ source = 'sine'; sineFreq = 440 })
Remove-Item -Recurse -Force $sessionDir
$pipe.Dispose()
Write-Host 'PASSED'
