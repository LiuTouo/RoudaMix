# M0 定義性測試:engine 不依賴 UI — attach、斷線、存活、reattach 同 epoch
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Core

$exe = "$PSScriptRoot\..\engine\build\Release\roudamix-engine.exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

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

function Connect-Engine {
    $c = New-Object System.IO.Pipes.NamedPipeClientStream(".", "roudamix-engine", [System.IO.Pipes.PipeDirection]::InOut)
    $c.Connect(3000)
    ,$c
}

$epoch1 = $null
try {
    # --- 第一次 attach:接上即 snapshot,然後 ping ---
    $c1 = Connect-Engine
    $snap1 = Read-Frame $c1
    if ($snap1.kind -ne "snapshot") { throw "expected snapshot event, got $($snap1.kind)" }
    $epoch1 = $snap1.payload.epoch
    Send-Frame $c1 '{"protocolVersion":1,"id":1,"kind":"ping","payload":{}}'
    $rep = Read-Frame $c1
    if ($rep.ok -ne $true -or $rep.result.engineVersion -ne "0.1.0") { throw "bad ping reply: $($rep | ConvertTo-Json -Compress)" }
    Write-Host "attach1 OK  epoch=$epoch1  engine=$($rep.result.engineVersion)"
    $c1.Close()

    # --- 斷線後 engine 必須續活 ---
    Start-Sleep -Seconds 2
    if ($proc.HasExited) { throw "engine died after client disconnect!" }
    Write-Host "engine alive after disconnect OK"

    # --- reattach:同 epoch 證狀態未重置 ---
    $c2 = Connect-Engine
    $snap2 = Read-Frame $c2
    $epoch2 = $snap2.payload.epoch
    if ($epoch2 -ne $epoch1) { throw "epoch changed: $epoch1 -> $epoch2" }
    Send-Frame $c2 '{"protocolVersion":1,"id":2,"kind":"shutdown_engine","payload":{}}'
    $rep2 = Read-Frame $c2
    if ($rep2.ok -ne $true) { throw "shutdown reply not ok" }
    $c2.Close()
    Write-Host "reattach OK  epoch=$epoch2 (same)  shutdown accepted"
}
finally {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}

Write-Host "`nM0 engine lifecycle test PASSED" -ForegroundColor Green
