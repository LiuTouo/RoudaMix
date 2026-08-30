# kHs Gain preset roundtrip(使用者回報:gain 存檔重載數值沒回來)—— 暫時腳本
$ErrorActionPreference = 'Stop'
$engineExe = Join-Path $PSScriptRoot '..\engine\build\Release\roudamix-engine.exe'
function Read-Frame($s) {
    $len = New-Object byte[] 4; $r = 0
    while ($r -lt 4) { $n = $s.Read($len, $r, 4 - $r); if ($n -le 0) { throw 'EOF' }; $r += $n }
    $sz = [BitConverter]::ToUInt32($len, 0)
    $body = New-Object byte[] $sz; $r = 0
    while ($r -lt $sz) { $n = $s.Read($body, $r, $sz - $r); if ($n -le 0) { throw 'EOF' }; $r += $n }
    [Text.Encoding]::UTF8.GetString($body) | ConvertFrom-Json
}
function Send-Frame($s, $obj) {
    $json = $obj | ConvertTo-Json -Depth 10 -Compress
    $b = [Text.Encoding]::UTF8.GetBytes($json)
    $s.Write([BitConverter]::GetBytes([UInt32]$b.Length), 0, 4); $s.Write($b, 0, $b.Length); $s.Flush()
}
$script:id = 200
function Cmd($s, $kind, $payload) {
    $script:id++
    Send-Frame $s @{ protocolVersion = 1; id = $script:id; kind = $kind; payload = $payload }
    while ($true) {
        $f = Read-Frame $s
        if ($f.id -eq $script:id) {
            if (-not $f.ok) { throw "$kind failed: $($f.error.code) $($f.error.message)" }
            return $f
        }
    }
}
function Param-Map($reply) {
    $h = @{}
    foreach ($p in $reply.result.params) { $h[[uint32]$p.paramId] = [double]$p.normalized }
    return $h
}
Start-Process -FilePath $engineExe -WindowStyle Hidden | Out-Null
Start-Sleep -Milliseconds 900
$engine = Get-Process roudamix-engine -ErrorAction Stop
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(3000)
$scan = Cmd $pipe 'scan_plugins' @{}
$gain = $scan.result.plugins | Where-Object { $_.classes[0].name -like '*Gain*' } | Select-Object -First 1
if (-not $gain) { throw 'kHs Gain not found' }
$add = Cmd $pipe 'add_plugin' @{ path = $gain.path; classId = $gain.classes[0].uid }
$inst = $add.result.instanceId
$orig = Param-Map (Cmd $pipe 'get_params' @{ instanceId = $inst })
Write-Host "params: $($orig.Count)"
# 全部改到 0.3 → 存檔(期望值 = 0.3 快照)→ 再改到 0.8 → 載檔 → 每顆回 0.3
foreach ($k in $orig.Keys) { [void](Cmd $pipe 'set_param' @{ instanceId = $inst; paramId = $k; value = 0.3 }) }
$saved = Param-Map (Cmd $pipe 'get_params' @{ instanceId = $inst })
$file = Join-Path $env:TEMP 'rmx-gain.vstpreset'
[void](Cmd $pipe 'save_preset' @{ instanceId = $inst; path = $file })
foreach ($k in $orig.Keys) { [void](Cmd $pipe 'set_param' @{ instanceId = $inst; paramId = $k; value = 0.8 }) }
[void](Cmd $pipe 'load_preset' @{ instanceId = $inst; path = $file })
$now = Param-Map (Cmd $pipe 'get_params' @{ instanceId = $inst })
$bad = 0
foreach ($k in $saved.Keys) {
    if ([Math]::Abs($now[$k] - $saved[$k]) -gt 1e-4) { $bad++; Write-Host "param $k not restored: saved=$($saved[$k]) now=$($now[$k])" }
}
if ($bad -gt 0) { throw "ASSERT FAIL: $bad params not restored" }
[void](Cmd $pipe 'remove_plugin' @{ instanceId = $inst })
$pipe.Dispose()
Stop-Process -Id $engine.Id -Force
Write-Host 'GAIN PRESET ROUNDTRIP PASSED'
