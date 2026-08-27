# M2 驗證:VST3 rack 全流程(scan → add → params → start → SHM strips → bypass → move → remove → stop)
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
function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }

# --- 0. engine 連線(在跑就接;沒在跑就 spawn)---
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(1500) } catch {
    if (-not (Test-Path $engineExe)) { throw "engine exe not found: $engineExe" }
    Start-Process -FilePath $engineExe -WindowStyle Hidden | Out-Null
    Start-Sleep -Milliseconds 800
    $pipe.Connect(3000)
}
$snap = Read-Frame $pipe
Assert ($snap.kind -eq 'snapshot') "first frame is snapshot"
Write-Host "connect: epoch=$($snap.payload.epoch) rack=$($snap.payload.status.rack.Count)"
# attach 到既有 engine 時清空殘留狀態(驗證環境從零開始)
if ($snap.payload.status.running) { [void](Command $pipe 'stop' @{}) }
foreach ($s in @($snap.payload.status.rack)) {
    [void](Command $pipe 'remove_plugin' @{ instanceId = $s.instanceId })
}

# --- 1. 掃描 ---
$scan = Command $pipe 'scan_plugins' @{}
$mods = $scan.result.plugins
Assert ($mods.Count -ge 1) 'scan found >=1 module'
$first = $mods[0]
Assert ($first.classes.Count -ge 1) 'first module has classes'
Write-Host "scan: $($mods.Count) modules, first=$(Split-Path $first.path -Leaf) class=$($first.classes[0].name)"

# --- 2. 加入 ---
$add = Command $pipe 'add_plugin' @{ path = $first.path; classId = $first.classes[0].uid }
$inst = $add.result.instanceId
Assert ($add.result.rack.Count -ge 1) 'rack has the new slot'
Assert ($add.result.rack[-1].instanceId -eq $inst) 'new slot is last'
Assert ($add.result.rack[-1].params.Count -ge 1) 'slot carries params'
Write-Host "add: instanceId=$inst name=$($add.result.rack[-1].name) params=$($add.result.rack[-1].params.Count)"

# --- 3. get_params + set_param ---
$gp = Command $pipe 'get_params' @{ instanceId = $inst }
$p0 = $gp.result.params | Where-Object { -not $_.bypass } | Select-Object -First 1
Assert ($null -ne $p0) 'has non-bypass param'
[void](Command $pipe 'set_param' @{ instanceId = $inst; paramId = $p0.paramId; value = 0.5 })
Write-Host "set_param: $($p0.name) -> 0.5"

# --- 4. start(帶 sampleRate;契約必填)---
$dev = Command $pipe 'list_devices' @{}
Assert ($dev.result.devices.Count -ge 1) 'ASIO device present'
$key = $dev.result.devices[0].deviceKey
$st = Command $pipe 'start' @{ deviceKey = $key; sampleRate = 48000 }
Assert ($st.result.running) 'running'
Assert ($st.result.pluginFails -eq 0) "pluginFails=0 at start (got $($st.result.pluginFails))"
Write-Host "start: rate=$($st.result.sampleRate) buf=$($st.result.bufferSize) pluginFails=0"

Start-Sleep -Milliseconds 1500
$gs = Command $pipe 'get_snapshot' @{}
Assert ($gs.result.snapshot.status.running) 'still running'
Assert ($gs.result.snapshot.status.pluginFails -eq 0) "pluginFails=0 while running (got $($gs.result.snapshot.status.pluginFails))"

# --- 5. SHM strips:strip0=engine 輸出(0xFFFFFFFF)、strip1=slot ---
$shmFile = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Local\roudamix-telemetry')
$acc = $shmFile.CreateViewAccessor()
$tries = 0
do {
    $s1 = $acc.ReadUInt32(8); Start-Sleep -Milliseconds 20; $s2 = $acc.ReadUInt32(8); $tries++
} while (($s1 -ne $s2 -or ($s2 % 2) -ne 0) -and $tries -lt 50)
Assert (($s2 % 2) -eq 0 -and $s1 -eq $s2) 'SHM seqlock stable'
$count = $acc.ReadUInt32(12)
$peak0 = $acc.ReadSingle(56); $inst0 = $acc.ReadUInt32(48)
$inst1 = $acc.ReadUInt32(80); $peak1 = $acc.ReadSingle(88)
$acc.Dispose(); $shmFile.Dispose()
Assert ($count -ge 2) "stripCount>=2 (got $count)"
Assert ($inst0 -eq 4294967295) "strip0 is engine out (got $inst0)"
Assert ($inst1 -eq $inst) "strip1 instanceId matches slot (got $inst1)"
Assert ($peak0 -gt 0.001) "engine out has signal (peak=$peak0)"
Assert ($peak1 -gt 0.001) "slot has signal (peak=$peak1)"
Write-Host "shm: stripCount=$count strip0=$inst0/$peak0 strip1=$inst1/$peak1"

# --- 6. bypass / move / remove ---
$by = Command $pipe 'set_bypass' @{ instanceId = $inst; bypassed = $true }
Assert ($by.result.rack[-1].bypassed) 'bypassed'
Assert ($by.result.rack[-1].instanceId -eq $inst) 'bypass on right slot'
$mv = Command $pipe 'move_plugin' @{ instanceId = $inst; newIndex = 0 }
Assert ($mv.result.rack[0].instanceId -eq $inst) 'moved to index 0'
$rm = Command $pipe 'remove_plugin' @{ instanceId = $inst }
Assert (($rm.result.rack | Where-Object { $_.instanceId -eq $inst }).Count -eq 0) 'removed'
Write-Host "bypass/move/remove: ok"

# --- 7. stop + start 重入(plugin initialize 冪等路徑)---
[void](Command $pipe 'stop' @{})
$add2 = Command $pipe 'add_plugin' @{ path = $first.path; classId = $first.classes[0].uid }
$st2 = Command $pipe 'start' @{ deviceKey = $key; sampleRate = 48000 }
Assert ($st2.result.pluginFails -eq 0) "pluginFails=0 on second start (got $($st2.result.pluginFails))"
[void](Command $pipe 'stop' @{})
[void](Command $pipe 'remove_plugin' @{ instanceId = $add2.result.instanceId })
Write-Host 'stop/start-reentry: ok'

$pipe.Dispose()
Write-Host 'PASSED'
