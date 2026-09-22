# M6 定義性測試:gain/mute RT 穩態真的有乘(track 錶 = post-fader 振幅)+ track_move master 絕對索引
# 前置:engine 已建(Release)、有 ASIO 裝置(取第一台)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Core

$engineExe = "$PSScriptRoot\..\engine\build\Release\roudamix-engine.exe"

Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

function Read-Frame([System.IO.Stream]$s) {
    $lenBuf = New-Object byte[] 4
    $read = 0
    while ($read -lt 4) { $n = $s.Read($lenBuf, $read, 4 - $read); if ($n -le 0) { throw "EOF" }; $read += $n }
    $len = [BitConverter]::ToUInt32($lenBuf, 0)
    $buf = New-Object byte[] $len
    $read = 0
    while ($read -lt $len) { $n = $s.Read($buf, $read, $len - $read); if ($n -le 0) { throw "EOF" }; $read += $n }
    [Text.Encoding]::UTF8.GetString($buf) | ConvertFrom-Json
}
function Send-Frame([System.IO.Stream]$s, [string]$json) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $s.Write([BitConverter]::GetBytes([UInt32]$bytes.Length), 0, 4)
    $s.Write($bytes, 0, $bytes.Length)
    $s.Flush()
}
function Wait-Reply([System.IO.Stream]$s, [uint64]$id) {
    while ($true) {
        $f = Read-Frame $s
        if ($f.PSobject.Properties.Name -contains "ok" -and $f.id -eq $id) { return $f }
    }
}
$script:nextId = 100
function Cmd([System.IO.Stream]$s, [string]$kind, [hashtable]$payload) {
    $script:nextId++
    $env = @{ protocolVersion = 2; id = $script:nextId; kind = $kind; payload = $payload }
    Send-Frame $s ($env | ConvertTo-Json -Compress -Depth 6)
    return (Wait-Reply $s $script:nextId)
}
function Assert([bool]$cond, [string]$msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }

# SHM:strips 自 offset 48、每條 32B(id +0、kind +4、peakL +8);kind 1 = trackId
function Read-TrackPeak($acc, [uint32]$trackId) {
    for ($i = 0; $i -lt 64; $i++) {
        $base = 48 + $i * 32
        if ($acc.ReadUInt32($base + 4) -eq 1 -and $acc.ReadUInt32($base) -eq $trackId) {
            return $acc.ReadSingle($base + 8)
        }
    }
    return 0.0
}
function Peak-Max($acc, [uint32]$trackId, [int]$ms) {
    $m = 0.0
    $deadline = [DateTime]::UtcNow.AddMilliseconds($ms)
    while ([DateTime]::UtcNow -lt $deadline) {
        $m = [Math]::Max($m, (Read-TrackPeak $acc $trackId))
        Start-Sleep -Milliseconds 30
    }
    return $m
}

$engine = $null
try {
    $engine = Start-Process -FilePath $engineExe -WorkingDirectory (Split-Path $engineExe) -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $c = New-Object System.IO.Pipes.NamedPipeClientStream(".", "roudamix-engine", [System.IO.Pipes.PipeDirection]::InOut)
    $c.Connect(3000)
    $null = Read-Frame $c  # snapshot push

    # --- 場景:audio(sine 440)→ dests → output(asioOut 0)→ start ---
    $r = Cmd $c "track_add" @{ kind = "audio" }
    $tid = [uint32]$r.result.trackId
    $null = Cmd $c "track_set_source" @{ trackId = $tid; source = @{ type = "sine"; freq = 440 } }
    $r = Cmd $c "track_add" @{ kind = "output"; name = "Mon" }
    $oid = [uint32]$r.result.trackId
    $null = Cmd $c "track_set_output" @{ trackId = $oid; output = @{ type = "asioOut"; channel = 0 } }
    $null = Cmd $c "track_set_dests" @{ trackId = $tid; dests = @($oid) }
    $devs = (Cmd $c "list_devices" @{}).result.devices
    if (-not $devs -or $devs.Count -eq 0) { throw "no ASIO device" }
    $rep = Cmd $c "start" @{ deviceKey = $devs[0].deviceKey; sampleRate = $null }
    Assert ($rep.ok -eq $true -and $rep.result.running -eq $true) "start failed"
    Write-Host "start OK  device=$($devs[0].name)"

    Start-Sleep -Milliseconds 300
    $mh = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Local\roudamix-telemetry")
    $acc = $mh.CreateViewAccessor(0, 3128)

    # --- gain 穩態:unity P1 ≈ 0.25(sine 本身 -12dBFS)→ gain 0.25 後 P2/P1 ≈ 0.25 ---
    $p1 = Peak-Max $acc $tid 900
    Assert ($p1 -gt 0.15 -and $p1 -lt 0.35) "sine unity peak out of range: $p1"
    $loadBefore = $acc.ReadSingle(24)
    $null = Cmd $c "track_set" @{ trackId = $tid; gain = 0.25 }
    Start-Sleep -Milliseconds 400
    $p2 = Peak-Max $acc $tid 900
    $ratio = $p2 / $p1
    Assert ([Math]::Abs($ratio - 0.25) -lt 0.06) "gain 0.25 not applied steady-state: P1=$p1 P2=$p2 ratio=$ratio"
    Write-Host ("gain RT OK  P1={0:N3}  P2={1:N3}  ratio={2:N3}" -f $p1, $p2, $ratio)

    # --- mute 穩態:歸零;解除後回 P2 ---
    $null = Cmd $c "track_set" @{ trackId = $tid; mute = $true }
    Start-Sleep -Milliseconds 300
    $pm = Peak-Max $acc $tid 600
    Assert ($pm -lt 0.01) "mute not applied steady-state: $pm"
    $null = Cmd $c "track_set" @{ trackId = $tid; mute = $false }
    Start-Sleep -Milliseconds 300
    $p3 = Peak-Max $acc $tid 600
    Assert ([Math]::Abs($p3 / $p1 - 0.25) -lt 0.06) "unmute did not restore gain: $p3"
    $loadAfter = $acc.ReadSingle(24)
    Write-Host ("mute RT OK  peak={0:N4} -> restored {1:N3}  callbackLoad {2:N3} -> {3:N3}" -f $pm, $p3, $loadBefore, $loadAfter)

    # --- track_move:master 絕對索引(erase+insert) ---
    $kinds = { param($snap) $snap.result.snapshot.status.tracks | ForEach-Object { $_.kind } }
    $r = Cmd $c "track_add" @{ kind = "fx" }
    $fid = [uint32]$r.result.trackId
    # master 現在 = [audio, output, fx];fx 移到 0
    $r = Cmd $c "track_move" @{ trackId = $fid; newIndex = 0 }
    Assert ($r.ok -eq $true) "track_move fx->0 failed: $($r.error.message)"
    $order = $r.result.tracks | ForEach-Object { $_.kind }
    Assert ("$order" -eq "fx audio output") "fx->0 order wrong: $order"
    # app 追加到尾,再 newIndex 超尾(999)= 移到尾端(夾限,不噴錯)
    $r = Cmd $c "track_add" @{ kind = "app" }
    $pid2 = [uint32]$r.result.trackId
    $r = Cmd $c "track_move" @{ trackId = $pid2; newIndex = 999 }
    Assert ($r.ok -eq $true) "track_move overrun failed"
    $order = $r.result.tracks | ForEach-Object { $_.kind }
    Assert ("$order" -eq "fx audio output app") "overrun clamp order wrong: $order"
    # audio(app 之前那軌)移到 1 = [fx audio ...] 不變(cur==new early-return)
    # 未知 id = bad_command
    $r = Cmd $c "track_move" @{ trackId = 424242; newIndex = 0 }
    Assert ($r.ok -ne $true -and $r.error.code -eq "bad_command") "unknown trackId should bad_command"
    Write-Host "track_move OK  order=$order"

    # --- session roundtrip:順序即 tracks 陣列序 ---
    $sessPath = "$env:TEMP\roudamix-m6.rmsession"
    $null = Cmd $c "save_session" @{ path = $sessPath }
    $before = (Cmd $c "get_snapshot" @{}).result.snapshot.status.tracks | ForEach-Object { $_.kind }
    $null = Cmd $c "track_move" @{ trackId = $fid; newIndex = 999 }  # 打亂
    $r = Cmd $c "load_session" @{ path = $sessPath }
    Assert ($r.ok -eq $true) "load_session failed"
    $after = (Cmd $c "get_snapshot" @{}).result.snapshot.status.tracks | ForEach-Object { $_.kind }
    Assert ("$before" -eq "$after") "session order not restored: before=$before after=$after"
    Write-Host "session order OK  $after"

    # --- 收尾 ---
    $script:nextId++
    Send-Frame $c '{"protocolVersion":2,"id":9999,"kind":"shutdown_engine","payload":{}}'
    $null = Wait-Reply $c 9999
    $c.Close()
    Start-Sleep -Seconds 2
    Assert ($engine.HasExited) "engine did not exit after shutdown"
    Write-Host "engine clean shutdown OK"
}
finally {
    if ($engine -and -not $engine.HasExited) { Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host "`nM6 gain-RT + track-order test PASSED" -ForegroundColor Green
