$ErrorActionPreference = 'Stop'
$engineExe = 'D:\Administrator\Documents\GitHub\RoudaMix\engine\build\Release\roudamix-engine.exe'
$outDir = "$env:TEMP\rmx-glass-probe"
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
Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Start-Process -FilePath $engineExe -WindowStyle Hidden -RedirectStandardError "$outDir\eng-step.err" | Out-Null
Start-Sleep -Milliseconds 900
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(3000)
$pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Byte
Write-Host 'connected'
try {
    $snap = Read-Frame $pipe
    Write-Host ("snapshot: kind=" + $snap.kind + " running=" + $snap.payload.status.running)
} catch { Write-Host "snapshot read FAIL: $_" }
$script:id = 1
Send-Frame $pipe @{ protocolVersion = 2; id = 1; kind = 'ping'; payload = @{} }
try {
    $f = Read-Frame $pipe
    Write-Host ("ping reply: id=" + $f.id + " ok=" + $f.ok)
} catch { Write-Host "ping read FAIL: $_" }
Send-Frame $pipe @{ protocolVersion = 2; id = 2; kind = 'track_add'; payload = @{ kind = 'fx' } }
try {
    $f = Read-Frame $pipe
    Write-Host ("track_add reply: id=" + $f.id + " ok=" + $f.ok + " result=" + ($f.result | ConvertTo-Json -Compress -Depth 5))
} catch { Write-Host "track_add read FAIL: $_" }
$pipe.Dispose()
Get-Process roudamix-engine -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'STEP DONE'
