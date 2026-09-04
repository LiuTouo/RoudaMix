# M6 驗證 step 4:WASAPI master clock — deviceKey "wasapi" start(系統預設輸出
# 當時脈 + 監聽)、sine→monitor(kAsioOut 解析到 clock scratch)SHM engine strip
# 有能量、rate 不符拒絕、stop/restart 乾淨。只需 render endpoint,不需 ASIO。
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
    # --- 1. 建場景:sine 軌 → 監聽軌(kAsioOut ch0;WASAPI master 下 = clock scratch)---
    $sine = Command $pipe 'track_add' @{ kind = 'audio'; name = 'Sine' }
    $sineId = [uint32]$sine.result.trackId
    [void](Command $pipe 'track_set_source' @{ trackId = $sineId; source = @{ type = 'sine'; freq = 440 } })
    $mon = Command $pipe 'track_add' @{ kind = 'output'; name = '監聽' }
    $monId = [uint32]$mon.result.trackId
    [void](Command $pipe 'track_set_output' @{ trackId = $monId; output = @{ type = 'asioOut'; channel = 0 } })
    [void](Command $pipe 'track_set_dests' @{ trackId = $sineId; dests = @($monId) })

    # --- 2. WASAPI master start ---
    $st = Command $pipe 'start' @{ deviceKey = 'wasapi'; sampleRate = $null }
    Assert ($st.result.running) 'running (wasapi master)'
    Assert ($st.result.deviceKey -eq 'wasapi') "deviceKey = wasapi (got $($st.result.deviceKey))"
    Write-Host "wasapi master: rate=$($st.result.sampleRate) block=$($st.result.bufferSize)"

    # --- 3. 跑 5 秒:SHM engine strip(strip 0)有能量、軌無 error ---
    Start-Sleep -Milliseconds 5000
    $gs = Try-Command $pipe 'get_snapshot' @{}
    $status = $gs.result.snapshot.status
    Assert ($status.running) 'still running'
    Write-Host "5s wasapi: xruns=$($status.xruns) pluginFails=$($status.pluginFails) block=$($status.bufferSize)"

    $mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
    $acc = $mm.CreateViewAccessor(0, 3128)
    $stripCount = $acc.ReadUInt32(12)
    $enginePeak = 0.0
    $sinePeak = 0.0
    for ($i = 0; $i -lt $stripCount; $i++) {
        $base = 48 + 32 * $i
        $kind = $acc.ReadUInt32($base + 4)
        $tid = $acc.ReadUInt32($base)
        if ($i -gt 0 -and $kind -eq 1 -and $tid -eq $sineId) {
            $sinePeak = [Math]::Max($sinePeak, $acc.ReadSingle($base + 8))
        }
        if ($kind -eq 2) {  # engine strip(kEngineOutput)
            $enginePeak = [Math]::Max($enginePeak, $acc.ReadSingle($base + 8))
        }
    }
    $acc.Dispose(); $mm.Dispose()
    Assert ($sinePeak -gt 0.1) "sine track energy (peak=$sinePeak)"
    Assert ($enginePeak -gt 0.1) "engine strip energy via clock scratch (peak=$enginePeak)"

    # --- 4. stop 不崩,可重啟 ---
    [void](Command $pipe 'stop' @{})
    $st2 = Command $pipe 'start' @{ deviceKey = 'wasapi'; sampleRate = $null }
    Assert ($st2.result.running) 'restart after stop'
    [void](Command $pipe 'stop' @{})
    Write-Host 'stop/restart clean'

    # --- 5. rate 不符(shared 鎖 mix rate)→ device_open_failed ---
    $mixRate = [int]$st.result.sampleRate
    $badRate = 48000
    if ($mixRate -eq 48000) { $badRate = 44100 }
    $bad = Try-Command $pipe 'start' @{ deviceKey = 'wasapi'; sampleRate = $badRate }
    Assert ($bad.ok -eq $false -and $bad.error.code -eq 'device_open_failed') "rate mismatch rejected device_open_failed (got $($bad.error.code))"
    Write-Host 'rate mismatch rejected'

    Clear-Tracks $pipe (Try-Command $pipe 'get_snapshot' @{}).result.snapshot.status
    $pipe.Dispose()
    Write-Host 'PASSED'
} finally {
    Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
