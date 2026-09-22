# M1 定義性測試:audio 開跑後 UI 生死不影響 engine — start 出聲、SHM 推進、殺 UI 音不斷
# 前置:engine 已建(Release)、app 已建(debug)、有 ASIO 裝置(取第一台)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Core

$engineExe = "$PSScriptRoot\..\engine\build\Release\roudamix-engine.exe"
$appExe = "$PSScriptRoot\..\target\release\roudamix-app.exe"

# 清場
Get-Process roudamix-app, roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force
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

$engine = $null
$app = $null
try {
    # --- engine 直接開,pipe 下 start(sine 440 出聲) ---
    $engine = Start-Process -FilePath $engineExe -WorkingDirectory (Split-Path $engineExe) -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $c = New-Object System.IO.Pipes.NamedPipeClientStream(".", "roudamix-engine", [System.IO.Pipes.PipeDirection]::InOut)
    $c.Connect(3000)
    $null = Read-Frame $c  # snapshot push
    # M5:v2 預設零軌 = 靜音。sine 軌路由到輸出軌(ASIO 0/1),strip0 才有訊號
    Send-Frame $c '{"protocolVersion":2,"id":1,"kind":"track_add","payload":{"kind":"audio"}}'
    $tr = Wait-Reply $c 1
    $tid = $tr.result.trackId
    $srcJson = @{ protocolVersion = 2; id = 2; kind = "track_set_source"; payload = @{ trackId = $tid; source = @{ type = "sine"; freq = 440 } } } | ConvertTo-Json -Compress
    Send-Frame $c $srcJson
    $null = Wait-Reply $c 2
    Send-Frame $c '{"protocolVersion":2,"id":3,"kind":"track_add","payload":{"kind":"output","name":"Mon"}}'
    $oid = (Wait-Reply $c 3).result.trackId
    $outJson = @{ protocolVersion = 2; id = 4; kind = "track_set_output"; payload = @{ trackId = $oid; output = @{ type = "asioOut"; channel = 0 } } } | ConvertTo-Json -Compress
    Send-Frame $c $outJson
    $null = Wait-Reply $c 4
    $dJson = @{ protocolVersion = 2; id = 5; kind = "track_set_dests"; payload = @{ trackId = $tid; dests = @($oid) } } | ConvertTo-Json -Compress
    Send-Frame $c $dJson
    $null = Wait-Reply $c 5
    Send-Frame $c '{"protocolVersion":2,"id":6,"kind":"list_devices","payload":{}}'
    $devs = (Wait-Reply $c 6).result.devices
    if (-not $devs -or $devs.Count -eq 0) { throw "no ASIO device" }
    $key = $devs[0].deviceKey
    $devJson = @{ protocolVersion = 2; id = 7; kind = "start"; payload = @{ deviceKey = $key; sampleRate = $null } } | ConvertTo-Json -Compress
    Send-Frame $c $devJson
    $rep = Wait-Reply $c 7
    if ($rep.ok -ne $true -or $rep.result.running -ne $true) { throw "start failed: $($rep | ConvertTo-Json -Compress)" }
    Write-Host "start OK  device=$($devs[0].name)  $($rep.result.sampleRate) Hz buf=$($rep.result.bufferSize)"
    # 斷開 pipe — engine 照跑(pipe 單 instance,讓位給 app)
    $c.Close()

    # --- SHM 遙測推進(等首版 publish,30Hz) ---
    Start-Sleep -Milliseconds 300
    $mh = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Local\roudamix-telemetry")
    $acc = $mh.CreateViewAccessor(0, 560)
    $s1 = $acc.ReadUInt32(8); $p1 = $acc.ReadSingle(56)
    Start-Sleep -Milliseconds 400
    $s2 = $acc.ReadUInt32(8); $p2 = $acc.ReadSingle(56)
    if ($s1 -eq $s2) { throw "telemetry seq not advancing: $s1" }
    if ($p1 -le 0.0) { throw "sine peak not present: $p1" }
    Write-Host "telemetry OK  seq $s1->$s2  peakL $p1/$p2"

    # --- 開 UI:app attach,錶應顯示(人工看) ---
    $app = Start-Process -FilePath $appExe -WorkingDirectory (Split-Path $appExe) -PassThru
    Start-Sleep -Seconds 4
    if ($app.HasExited) { throw "app exited during attach" }
    if ($engine.HasExited) { throw "engine died after app attach!" }
    $s3 = $acc.ReadUInt32(8)
    if ($s3 -le $s2) { throw "telemetry stopped after app attach" }
    Write-Host "app attached OK  seq=$s3 (meter should be moving in UI)"

    # --- 殺 UI:聲音/遙測必須不斷 ---
    $app.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 2
    if (-not $app.HasExited) { Stop-Process -Id $app.Id -Force; Start-Sleep -Milliseconds 500 }
    if ($engine.HasExited) { throw "engine died when UI killed!" }
    $s4 = $acc.ReadUInt32(8)
    if ($s4 -le $s3) { throw "telemetry stopped when UI killed" }
    Write-Host "UI killed, engine alive + telemetry continuing OK  seq=$s4"

    # --- 重開 UI:attach 同一 engine pid,錶續動 ---
    $app2 = Start-Process -FilePath $appExe -WorkingDirectory (Split-Path $appExe) -PassThru
    Start-Sleep -Seconds 4
    if ($app2.HasExited) { throw "app2 exited" }
    $engineNow = Get-Process -Id $engine.Id -ErrorAction SilentlyContinue
    if (-not $engineNow) { throw "engine died after UI restart!" }
    $s5 = $acc.ReadUInt32(8)
    if ($s5 -le $s4) { throw "telemetry stopped after UI restart" }
    Write-Host "UI restarted, same engine pid=$($engine.Id), telemetry continuing OK  seq=$s5"
    $app2.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 1

    # --- 收尾:乾淨 shutdown(經 pipe) ---
    $c2 = New-Object System.IO.Pipes.NamedPipeClientStream(".", "roudamix-engine", [System.IO.Pipes.PipeDirection]::InOut)
    $c2.Connect(3000)
    $null = Read-Frame $c2
    Send-Frame $c2 '{"protocolVersion":2,"id":9,"kind":"shutdown_engine","payload":{}}'
    $null = Wait-Reply $c2 9
    $c2.Close()
    Start-Sleep -Seconds 2
    if (-not $engine.HasExited) { throw "engine did not exit after shutdown" }
    Write-Host "engine clean shutdown OK"
}
finally {
    Get-Process roudamix-app -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if ($engine -and -not $engine.HasExited) { Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host "`nM1 audio lifecycle test PASSED" -ForegroundColor Green
