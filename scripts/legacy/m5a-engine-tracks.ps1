# M5a 驗證:track graph(protocol v2)— 軌道 CRUD、路由鏈、環偵測、插件無上限、
# telemetry v3 strips、session v2 roundtrip、v1 檔拒載
# 前置:engine 已跑(或此腳本自行 spawn);ASIO 裝置存在;Common Files\VST3 有 .vst3
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
    Send-Frame $s @{ protocolVersion = 2; id = $script:id; kind = $kind; payload = $payload }
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
    Send-Frame $s @{ protocolVersion = 2; id = $script:id; kind = $kind; payload = $payload }
    while ($true) {
        $f = Read-Frame $s
        if ($f.id -ne $null -and $f.id -eq $script:id) { return $f }
    }
}
function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }
function Clear-Tracks($s, $status) {
    if ($status.running) { [void](Command $s 'stop' @{}) }
    foreach ($t in @($status.tracks)) {
        [void](Command $s 'track_remove' @{ trackId = [uint32]$t.trackId })
    }
}

# --- 0. engine 連線 + 清場 ---
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(1500) } catch {
    $running = Get-Process roudamix-engine -ErrorAction SilentlyContinue
    if ($running) { throw "engine running but pipe busy (another client holds it) - close the UI first" }
    if (-not (Test-Path $engineExe)) { throw "engine exe not found: $engineExe" }
    Start-Process -FilePath $engineExe -WindowStyle Hidden | Out-Null
    Start-Sleep -Milliseconds 800
    $pipe.Connect(3000)
}
$snap = Read-Frame $pipe
Assert ($snap.kind -eq 'snapshot') "first frame is snapshot"
Assert ($snap.payload.status.PSObject.Properties['tracks'] -ne $null) "status carries tracks (v2)"
Clear-Tracks $pipe $snap.payload.status

# --- 1. 建軌:audio(sine 880)→ fx → output(ASIO 0/1)---
$a = Command $pipe 'track_add' @{ kind = 'audio'; name = 'Mic' }
$fx = Command $pipe 'track_add' @{ kind = 'fx'; name = 'FX 鏈' }
$mon = Command $pipe 'track_add' @{ kind = 'output'; name = '監聽' }
$aId = [uint32]$a.result.trackId; $fxId = [uint32]$fx.result.trackId; $monId = [uint32]$mon.result.trackId
Assert ($aId -ne $fxId -and $fxId -ne $monId) 'distinct trackIds'
Write-Host "tracks: audio=$aId fx=$fxId out=$monId"

[void](Command $pipe 'track_set_source' @{ trackId = $aId; source = @{ type = 'sine'; freq = 880 } })
[void](Command $pipe 'track_set_dests' @{ trackId = $aId; dests = @($fxId) })
[void](Command $pipe 'track_set_dests' @{ trackId = $fxId; dests = @($monId) })
[void](Command $pipe 'track_set_output' @{ trackId = $monId; output = @{ type = 'asioOut'; channel = 0 } })
[void](Command $pipe 'track_set' @{ trackId = $aId; gain = 0.75; mute = $false })

# --- 2. 環偵測:fx → audio 會成環( audio→fx→audio )= cycle_detected 且不套用 ---
$cyc = Try-Command $pipe 'track_set_dests' @{ trackId = $fxId; dests = @($aId) }
Assert ($cyc.ok -eq $false) 'cycle rejected'
Assert ($cyc.error.code -eq 'cycle_detected') "cycle_detected (got $($cyc.error.code))"
$self = Try-Command $pipe 'track_set_dests' @{ trackId = $aId; dests = @($aId) }
Assert ($self.error.code -eq 'bad_command') 'self-dest rejected'
$gs = Try-Command $pipe 'get_snapshot' @{}
$fxNode = $gs.result.snapshot.status.tracks | Where-Object { $_.trackId -eq $fxId }
Assert ($fxNode.dests.Count -eq 1 -and $fxNode.dests[0] -eq $monId) 'dests unchanged after cycle reject'
Write-Host "routing: chain ok, cycle Detected rejected"

# --- 3. 掃描 + 17 個插件進 fx 軌(無上限驗證:舊上限 15)---
$scan = Command $pipe 'scan_plugins' @{}
$mods = $scan.result.plugins
Assert ($mods.Count -ge 1) 'scan found >=1 module'
$first = $mods[0]
$ids = @()
for ($i = 0; $i -lt 17; $i++) {
    $r = Command $pipe 'add_plugin' @{ trackId = $fxId; path = $first.path; classId = $first.classes[0].uid }
    $ids += [uint32]$r.result.instanceId
}
Assert (($ids | Select-Object -Unique).Count -eq 17) '17 distinct instances (no cap)'
$gs = Try-Command $pipe 'get_snapshot' @{}
$fxNode = $gs.result.snapshot.status.tracks | Where-Object { $_.trackId -eq $fxId }
Assert ($fxNode.plugins.Count -eq 17) "fx chain has 17 plugins (got $($fxNode.plugins.Count))"
Write-Host "plugins: 17 in fx chain (cap removed)"

# --- 4. start + telemetry v3 SHM 驗證(strip kind + trackId)---
$dev = Command $pipe 'list_devices' @{}
Assert ($dev.result.devices.Count -ge 1) 'ASIO device present'
$dev0 = $dev.result.devices[0]
$key = $dev0.deviceKey
Assert ($dev0.PSObject.Properties['inputNames'] -ne $null) 'list_devices carries inputNames (v2)'
$st = Command $pipe 'start' @{ deviceKey = $key; sampleRate = 48000 }
Assert ($st.result.running) 'running'
Start-Sleep -Milliseconds 700  # 讓 publish thread 寫幾幀

$mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
$acc = $mm.CreateViewAccessor(0, 3128)
$magic = $acc.ReadUInt32(0); $abi = $acc.ReadUInt32(4)
Assert ($magic -eq 0x524D5854) 'SHM magic RMXT'
Assert ($abi -eq 3) "telemetry abi v3 (got $abi)"
$stripCount = $acc.ReadUInt32(12)
# strips@48,32B:id@0 kind@4。strip0 = engine 輸出(id 0xFFFFFFFF kind 2)
# 注意:PS 的 0xFFFFFFFF 字面值 = Int32 -1,和 ReadUInt32 的 4294967295 比對會 fail
Assert ($acc.ReadUInt32(48) -eq 4294967295 -and $acc.ReadUInt32(52) -eq 2) 'strip0 = engine out (kind 2)'
$foundTracks = 0; $foundPlugins = 0
for ($i = 1; $i -lt $stripCount; $i++) {
    $sid = $acc.ReadUInt32(48 + 32 * $i); $skind = $acc.ReadUInt32(48 + 32 * $i + 4)
    if ($skind -eq 1 -and ($sid -eq $aId -or $sid -eq $fxId -or $sid -eq $monId)) { $foundTracks++ }
    if ($skind -eq 0 -and $ids -contains $sid) { $foundPlugins++ }
}
Assert ($foundTracks -eq 3) "3 track strips by id (got $foundTracks)"
Assert ($foundPlugins -ge 17) "17 plugin strips (got $foundPlugins)"
Write-Host "telemetry v3: stripCount=$stripCount tracks=$foundTracks plugins=$foundPlugins"
$acc.Dispose(); $mm.Dispose()

# --- 5. session v2 roundtrip(結構 + dests 重接 + gain)---
$sessionDir = Join-Path $env:TEMP 'rmx-m5a'
New-Item -ItemType Directory -Force $sessionDir | Out-Null
$f1 = Join-Path $sessionDir 'plain.rmsession'
[void](Command $pipe 'stop' @{})
$sv = Command $pipe 'save_session' @{ path = $f1 }
Assert ($sv.result.savedPath -eq $f1) 'savedPath echoed'
$doc = Get-Content $f1 -Raw -Encoding UTF8 | ConvertFrom-Json
Assert ($doc.roudamixSession -eq 2) 'file version 2'
Assert ($doc.tracks.Count -eq 3) 'file has 3 tracks'
Assert ($doc.deviceKey -eq $key) "deviceKey = started device (got '$($doc.deviceKey)')"
$docAudio = $doc.tracks | Where-Object { $_.kind -eq 'audio' }
Assert ($docAudio.source.type -eq 'sine' -and $docAudio.source.freq -eq 880) 'audio sine 880 saved'

# 清場 → load → 結構復原(trackId 全新、dests 重接)
Clear-Tracks $pipe (Try-Command $pipe 'get_snapshot' @{}).result.snapshot.status
$ld = Command $pipe 'load_session' @{ path = $f1 }
Assert ($ld.result.deviceKey -eq $key) 'applied.deviceKey'
$gs = Try-Command $pipe 'get_snapshot' @{}
$ts = $gs.result.snapshot.status.tracks
Assert ($ts.Count -eq 3) "tracks restored 3 (got $($ts.Count))"
$newAudio = $ts | Where-Object { $_.kind -eq 'audio' }
$newFx = $ts | Where-Object { $_.kind -eq 'fx' }
$newMon = $ts | Where-Object { $_.kind -eq 'output' }
Assert ($newAudio.source.type -eq 'sine' -and $newAudio.source.freq -eq 880) 'sine restored'
Assert ($newAudio.dests.Count -eq 1 -and $newAudio.dests[0] -eq $newFx.trackId) 'audio→fx remapped'
Assert ($newFx.dests.Count -eq 1 -and $newFx.dests[0] -eq $newMon.trackId) 'fx→monitor remapped'
Assert ($newMon.output.type -eq 'asioOut') 'monitor asioOut restored'
Assert ($newFx.plugins.Count -eq 17) 'fx chain 17 plugins restored'
Assert ($newAudio.trackId -ne $aId) 'fresh trackIds'
Write-Host "session v2 roundtrip: tracks=3 dests remapped, plugins=17 restored"

# --- 6. v1 檔拒載 ---
$f1v1 = Join-Path $sessionDir 'v1.rmsession'
Set-Content $f1v1 '{"roudamixSession":1,"rack":[],"source":"sine","sineFreq":440}'
$bad = Try-Command $pipe 'load_session' @{ path = $f1v1 }
Assert ($bad.ok -eq $false) 'v1 file rejected'
Assert ($bad.error.code -eq 'session_io') "v1 rejected session_io (got $($bad.error.code))"
Write-Host 'v1 session: rejected (session_io)'

# --- 7. 清場 ---
Clear-Tracks $pipe (Try-Command $pipe 'get_snapshot' @{}).result.snapshot.status
Remove-Item -Recurse -Force $sessionDir
$pipe.Dispose()
Write-Host 'PASSED'
