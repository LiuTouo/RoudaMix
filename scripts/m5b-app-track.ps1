# M5b 驗證:app 軌 process loopback — list_audio_apps、capture 出聲(SHM 軌 strip
# 有能量)、殺目標程序 → 軌 error、引擎續跑、app_not_found 拒絕
# 前置:engine 已建(Release);Windows 預設輸出裝置存在(C# SoundPlayer 播放用)
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

# --- 1. 背景播放程序(C# SoundPlayer 循環播 tada.wav → session 在該 PID)---
$wav = 'C:\Windows\Media\tada.wav'
if (-not (Test-Path $wav)) { throw "test wav missing: $wav" }
$playerScript = "`$sp = New-Object System.Media.SoundPlayer '$wav'; while (`$true) { `$sp.PlaySync() }"
$player = Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile', '-Command', $playerScript -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1500  # session 建立
if ($player.HasExited) { throw 'player process exited early' }

try {
    # --- 2. list_audio_apps 含播放程序 ---
    $apps = Command $pipe 'list_audio_apps' @{}
    $found = $apps.result.apps | Where-Object { $_.pid -eq $player.Id }
    Assert ($null -ne $found) "list_audio_apps has player pid $($player.Id) (got $($apps.result.apps.Count) apps: $(($apps.result.apps | ForEach-Object { $_.name }) -join ','))"
    Write-Host "list_audio_apps: $($apps.result.apps.Count) apps, player found ($($found.name))"

    # --- 3. 建軌:app(抓 player)→ output(ASIO 0/1)---
    $a = Command $pipe 'track_add' @{ kind = 'app'; name = 'Player' }
    $mon = Command $pipe 'track_add' @{ kind = 'output'; name = '監聽' }
    $aId = [uint32]$a.result.trackId; $monId = [uint32]$mon.result.trackId
    [void](Command $pipe 'track_set_source' @{ trackId = $aId; source = @{ type = 'app'; pid = $player.Id; name = $found.name } })
    [void](Command $pipe 'track_set_output' @{ trackId = $monId; output = @{ type = 'asioOut'; channel = 0 } })
    [void](Command $pipe 'track_set_dests' @{ trackId = $aId; dests = @($monId) })

    # --- 4. app_not_found:不存在的 pid ---
    $bad = Try-Command $pipe 'track_set_source' @{ trackId = $aId; source = @{ type = 'app'; pid = 999999 } }
    Assert ($bad.ok -eq $false -and $bad.error.code -eq 'app_not_found') "bad pid rejected app_not_found (got $($bad.error.code))"

    # --- 5. start → SHM app 軌 strip 有能量(tada.wav 循環有靜音 gap,publish
    #     每 33ms 清 peak —— 5 秒窗口輪詢取 max,單幀讀會漏)---
    $dev = (Command $pipe 'list_devices' @{}).result.devices[0]
    $st = Command $pipe 'start' @{ deviceKey = $dev.deviceKey; sampleRate = 48000 }
    Assert ($st.result.running) 'running'

    $mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
    $acc = $mm.CreateViewAccessor(0, 3128)
    $appPeak = 0.0
    $deadline = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $deadline -and $appPeak -le 0.0) {
        Start-Sleep -Milliseconds 250
        $stripCount = $acc.ReadUInt32(12)
        for ($i = 1; $i -lt $stripCount; $i++) {
            $base = 48 + 32 * $i
            if ($acc.ReadUInt32($base + 4) -eq 1 -and $acc.ReadUInt32($base) -eq $aId) {
                $appPeak = [Math]::Max($appPeak, $acc.ReadSingle($base + 8))
                $appPeak = [Math]::Max($appPeak, $acc.ReadSingle($base + 12))
            }
        }
    }
    Assert ($appPeak -gt 0.0) "app track strip has signal (peak=$appPeak in 6s window)"
    Write-Host "loopback: app track peak = $([Math]::Round($appPeak, 3))"

    # --- 6. 殺播放程序 → 軌 error、引擎續跑 ---
    Stop-Process -Id $player.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 2500  # pump 偵錯 + PostMessage + status 廣播
    $gs = Try-Command $pipe 'get_snapshot' @{}
    $appNode = $gs.result.snapshot.status.tracks | Where-Object { $_.trackId -eq $aId }
    Assert ($appNode.error -ne $null -and $appNode.error -ne '') "app track error set after kill (got '$($appNode.error)')"
    Assert ($gs.result.snapshot.status.running) 'engine still running'
    Write-Host "player killed: track error = '$($appNode.error)', engine alive"

    # --- 7. 換下一個還活著的 app(source 重設後 start 期間重建 capture)---
    [void](Command $pipe 'stop' @{})
    Clear-Tracks $pipe (Try-Command $pipe 'get_snapshot' @{}).result.snapshot.status
    $acc.Dispose(); $mm.Dispose()
    $pipe.Dispose()
    Write-Host 'PASSED'
} finally {
    if ($player -and -not $player.HasExited) { Stop-Process -Id $player.Id -Force -ErrorAction SilentlyContinue }
    Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
