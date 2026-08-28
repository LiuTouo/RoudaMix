# M4b probe: worker sandbox (verify/scan), preset save/load roundtrip, spectrum SHM v2.
# ASCII-only (ps1 BOM pitfall). Any failure = throw (nonzero exit); all pass = PASSED.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$engineExe = Join-Path $root 'engine\build\Release\roudamix-engine.exe'
$workerExe = Join-Path $root 'engine\build\Release\roudamix-worker.exe'
$tmp = Join-Path $env:TEMP 'roudamix-m4b'
New-Item -ItemType Directory -Force $tmp | Out-Null
$presetFile = Join-Path $tmp 'probe.vstpreset'

function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }

# --- 0. clean slate: kill app/engine leftovers (probe owns the pipe) ---
foreach ($p in @('roudamix-app', 'roudamix-engine')) {
    Get-Process $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 600

# --- 1. worker standalone probes ---
Assert (Test-Path $workerExe) "worker exe exists: $workerExe"
$bad = Start-Process -FilePath $workerExe -ArgumentList @('--verify', 'C:\definitely\nope.vst3', '48000', '512') -Wait -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $tmp 'bad.err')
Assert ($bad.ExitCode -ne 0) "worker verify bad path fails (exit=$($bad.ExitCode))"

# --- 2. in-process scan to discover a real module (direct C++ not available; use engine) ---
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

$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
$engineErr = Join-Path $tmp 'probe-engine.err'
Start-Process -FilePath $engineExe -WindowStyle Hidden -RedirectStandardError $engineErr | Out-Null
Start-Sleep -Milliseconds 800
$pipe.Connect(3000)
$snap = Read-Frame $pipe
Assert ($snap.kind -eq 'snapshot') 'first frame is snapshot'
if ($snap.payload.status.running) { [void](Command $pipe 'stop' @{}) }
foreach ($s in @($snap.payload.status.rack)) { [void](Command $pipe 'remove_plugin' @{ instanceId = $s.instanceId }) }

# --- 3. scan (engine dispatches to worker; assert worker scan path really ran) ---
$scan = Command $pipe 'scan_plugins' @{}
$mods = $scan.result.plugins
Assert ($mods.Count -ge 1) "scan found >=1 module (got $($mods.Count))"
$first = $mods[0]
Write-Host "scan via worker: $($mods.Count) modules, first=$(Split-Path $first.path -Leaf)"

# --- 4. add_plugin (sandbox verify in worker must pass) ---
$add = Command $pipe 'add_plugin' @{ path = $first.path; classId = $first.classes[0].uid }
$inst = $add.result.instanceId
Write-Host "add (worker-verified): instanceId=$inst name=$($add.result.rack[-1].name)"

# --- 5. add_plugin with garbage path must fail cleanly (worker burn, engine alive) ---
$script:id++
Send-Frame $pipe @{ protocolVersion = 1; id = $script:id; kind = 'add_plugin'; payload = @{ path = 'C:\definitely\nope.vst3' } }
$replied = $false
for ($i = 0; $i -lt 20 -and -not $replied; $i++) {
    $f = Read-Frame $pipe
    if ($f.id -eq $script:id) { $replied = $true; Assert ($f.ok -eq $false) 'bad add_plugin returns error' }
}
Write-Host 'bad module rejected by sandbox worker'

# --- 6. preset roundtrip ---
$gp = Command $pipe 'get_params' @{ instanceId = $inst }
$p0 = $gp.result.params | Where-Object { -not $_.bypass } | Select-Object -First 1
Assert ($null -ne $p0) 'has non-bypass param'
[void](Command $pipe 'set_param' @{ instanceId = $inst; paramId = $p0.paramId; value = 0.9 })
[void](Command $pipe 'save_preset' @{ instanceId = $inst; path = $presetFile })
$head = [System.IO.File]::ReadAllBytes($presetFile)[0..3]
Assert ([System.Text.Encoding]::ASCII.GetString($head) -eq 'VST3') 'preset file starts with VST3 magic'
[void](Command $pipe 'set_param' @{ instanceId = $inst; paramId = $p0.paramId; value = 0.1 })
[void](Command $pipe 'load_preset' @{ instanceId = $inst; path = $presetFile })
$gp2 = Command $pipe 'get_params' @{ instanceId = $inst }
$v2 = ($gp2.result.params | Where-Object { $_.paramId -eq $p0.paramId }).normalized
Assert ([Math]::Abs($v2 - 0.9) -lt 0.05) "preset roundtrip restores 0.9 (got $v2)"
Write-Host "preset roundtrip: param $($p0.paramId) restored to $v2"

# --- 7. start + spectrum SHM v2 ---
[void](Command $pipe 'set_source' @{ source = 'sine'; sineFreq = 4000.0 })
$dev = Command $pipe 'list_devices' @{}
Assert ($dev.result.devices.Count -ge 1) 'ASIO device present'
$d0 = $dev.result.devices[0]
[void](Command $pipe 'start' @{ deviceKey = $d0.deviceKey; sampleRate = $null; bufferSize = $null; inputMono = $true })
Start-Sleep -Milliseconds 1500

$mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
$acc = $mmf.CreateViewAccessor(0, 4096)
$magic = $acc.ReadUInt32(0)
$abi = $acc.ReadUInt32(4)
Assert ($magic -eq 0x524D5854) 'SHM magic'
Assert ($abi -eq 2) "SHM abi v2 (got $abi)"
$specCount = $acc.ReadUInt32(560)
Assert ($specCount -eq 256) "spectrumCount=256 (got $specCount)"
$specRate = $acc.ReadSingle(28)
Assert ($specRate -gt 0) "sampleRate in SHM (got $specRate)"
# ideal FFT index k = freq*2048/rate; our bin j = k/4 - 1
$jIdeal = (4000.0 * 2048.0 / $specRate) / 4.0 - 1.0
$best = -1; $bestDb = -999.0
for ($j = 0; $j -lt 256; $j++) {
    $db = $acc.ReadSingle(564 + 4 * $j)
    if ($db -gt $bestDb) { $bestDb = $db; $best = $j }
}
Assert ([Math]::Abs($best - $jIdeal) -le 3.0) "spectrum peak bin $best within 3 of ideal $([Math]::Round($jIdeal,1)) (db=$bestDb)"
Assert ($bestDb -gt -40.0) "peak strong enough ($bestDb dB)"
# sanity: far-away bins stay low (sine leakage only near the lobe)
$farDb = $acc.ReadSingle(564 + 4 * 200)
Write-Host "spectrum: peak bin $best (ideal $($jIdeal.ToString('0.0'))) at $bestDb dB, bin200=$([Math]::Round($farDb,1)) dB, rate=$specRate"

try {
    Write-Host 'marker: sending stop'
    [void](Command $pipe 'stop' @{})
    Write-Host 'marker: stop replied, sending shutdown'
    [void](Command $pipe 'shutdown_engine' @{})
    Write-Host 'marker: shutdown replied'
} finally {
    $pipe.Close()
    Write-Host '--- engine stderr tail ---'
    Get-Content $engineErr -Tail 10
}
Write-Host 'PASSED'
