# M6 驗證 step 1+2:mic 軌(kWasapiIn)— list_capture_devices、預設麥克風綁定、
# 死 deviceId 拒絕、ASIO master 下 mic 軌啟動不錯誤、stop/restart 乾淨。
# 能量斷言走 sine(可重現);mic 監聽無回音消除,真 mic 能量不可重現故不斷言。
# 前置:engine 已建(Release);ASIO 裝置 + 至少一個 WASAPI capture endpoint
# 判定:任何一步錯 = throw;全過印 PASSED
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
foreach ($p in @('roudamix-app', 'roudamix-engine')) {
    Get-Process $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 800
if (-not (Test-Path $engineExe)) { throw "engine exe not found: $engineExe" }
Start-Process -FilePath $engineExe -WindowStyle Hidden | Out-Null
Start-Sleep -Milliseconds 800
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(3000)
$snap = Read-Frame $pipe
Assert ($snap.kind -eq 'snapshot') "first frame is snapshot"
Clear-Tracks $pipe $snap.payload.status

try {
    # --- 1. list_capture_devices 非空、恰一 default ---
    $rd = Command $pipe 'list_capture_devices' @{}
    $devices = $rd.result.devices
    Assert ($devices.Count -ge 1) "capture endpoints listed (got $($devices.Count))"
    $defaults = @($devices | Where-Object { $_.default })
    Assert ($defaults.Count -eq 1) "exactly one default capture device (got $($defaults.Count))"
    $target = $defaults[0]
    Write-Host "capture devices: $($devices.Count) (default: $($target.name) @ $($target.sampleRate) Hz)"

    # --- 2. 壞 mic deviceId → device_busy ---
    $mic0 = Command $pipe 'track_add' @{ kind = 'audio'; name = 'MicBad' }
    $bad = Try-Command $pipe 'track_set_source' @{ trackId = $mic0.result.trackId; source = @{ type = 'wasapiIn'; deviceId = '{dead-endpoint}' } }
    Assert ($bad.ok -eq $false -and $bad.error.code -eq 'device_busy') "bad mic id rejected device_busy (got $($bad.error.code))"
    [void](Command $pipe 'track_remove' @{ trackId = [uint32]$mic0.result.trackId })

    # --- 3. 建場景:sine 軌 + mic 軌(預設裝置)→ 監聽軌(ASIO 0/1)---
    $sine = Command $pipe 'track_add' @{ kind = 'audio'; name = 'Sine' }
    $sineId = [uint32]$sine.result.trackId
    [void](Command $pipe 'track_set_source' @{ trackId = $sineId; source = @{ type = 'sine'; freq = 440 } })
    $mic = Command $pipe 'track_add' @{ kind = 'audio'; name = 'Mic' }
    $micId = [uint32]$mic.result.trackId
    [void](Command $pipe 'track_set_source' @{ trackId = $micId; source = @{ type = 'wasapiIn'; deviceId = '' } })
    $mon = Command $pipe 'track_add' @{ kind = 'output'; name = '監聽' }
    $monId = [uint32]$mon.result.trackId
    [void](Command $pipe 'track_set_output' @{ trackId = $monId; output = @{ type = 'asioOut'; channel = 0 } })
    [void](Command $pipe 'track_set_dests' @{ trackId = $sineId; dests = @($monId) })
    [void](Command $pipe 'track_set_dests' @{ trackId = $micId; dests = @($monId) })

    # --- 4. start(ASIO master;start 時 ensure_mic)---
    $dev = (Command $pipe 'list_devices' @{}).result.devices[0]
    $st = Command $pipe 'start' @{ deviceKey = $dev.deviceKey; sampleRate = 48000 }
    Assert ($st.result.running) 'running'

    # --- 5. 跑 5 秒:mic 軌無 error、SHM sine 有能量 + mic strip 存在 ---
    Start-Sleep -Milliseconds 5000
    $gs = Try-Command $pipe 'get_snapshot' @{}
    $status = $gs.result.snapshot.status
    $micNode = $status.tracks | Where-Object { $_.trackId -eq $micId }
    Assert ($micNode.error -eq $null -or $micNode.error -eq '') "mic track no error (got '$($micNode.error)')"
    Assert ($status.running) 'still running'
    Write-Host "5s mic: xruns=$($status.xruns) pluginFails=$($status.pluginFails) micErr='$($micNode.error)'"

    $mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
    $acc = $mm.CreateViewAccessor(0, 3128)
    $stripCount = $acc.ReadUInt32(12)
    $sinePeak = 0.0
    $micStrip = $false
    for ($i = 1; $i -lt $stripCount; $i++) {
        $base = 48 + 32 * $i
        if ($acc.ReadUInt32($base + 4) -eq 1 -and $acc.ReadUInt32($base) -eq $sineId) {
            $sinePeak = [Math]::Max($sinePeak, $acc.ReadSingle($base + 8))
        }
        if ($acc.ReadUInt32($base + 4) -eq 1 -and $acc.ReadUInt32($base) -eq $micId) {
            $micStrip = $true
        }
    }
    $acc.Dispose(); $mm.Dispose()
    Assert ($sinePeak -gt 0.1) "sine feeding monitor (peak=$sinePeak)"
    Assert $micStrip "mic track has telemetry strip"

    # --- 6. stop 不崩,可重啟 ---
    [void](Command $pipe 'stop' @{})
    $st2 = Command $pipe 'start' @{ deviceKey = $dev.deviceKey; sampleRate = 48000 }
    Assert ($st2.result.running) 'restart after stop'
    [void](Command $pipe 'stop' @{})
    Write-Host 'stop/restart clean'

    Clear-Tracks $pipe (Try-Command $pipe 'get_snapshot' @{}).result.snapshot.status
    $pipe.Dispose()
    Write-Host 'PASSED'
} finally {
    Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
