param(
    [Parameter(Mandatory = $true)]
    [string]$SessionPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$engineExe = Join-Path $root 'engine\build\Release\roudamix-engine.exe'

function Read-Frame([System.IO.Stream]$Stream) {
    $len = New-Object byte[] 4
    $read = 0
    while ($read -lt 4) {
        $n = $Stream.Read($len, $read, 4 - $read)
        if ($n -le 0) { throw 'EOF while reading frame length' }
        $read += $n
    }
    $size = [BitConverter]::ToUInt32($len, 0)
    $body = New-Object byte[] $size
    $read = 0
    while ($read -lt $size) {
        $n = $Stream.Read($body, $read, $size - $read)
        if ($n -le 0) { throw 'EOF while reading frame body' }
        $read += $n
    }
    [Text.Encoding]::UTF8.GetString($body) | ConvertFrom-Json
}

function Send-Frame([System.IO.Stream]$Stream, $Frame) {
    $json = $Frame | ConvertTo-Json -Depth 12 -Compress
    $body = [Text.Encoding]::UTF8.GetBytes($json)
    $len = [BitConverter]::GetBytes([uint32]$body.Length)
    $Stream.Write($len, 0, $len.Length)
    $Stream.Write($body, 0, $body.Length)
    $Stream.Flush()
}

$script:nextId = 0
function Invoke-Engine([System.IO.Stream]$Stream, [string]$Kind, $Payload) {
    $script:nextId++
    Send-Frame $Stream @{ protocolVersion = 2; id = $script:nextId; kind = $Kind; payload = $Payload }
    while ($true) {
        $frame = Read-Frame $Stream
        if ($null -eq $frame.id -or $frame.id -ne $script:nextId) { continue }
        if (-not $frame.ok) { throw "$Kind failed: $($frame.error.code) $($frame.error.message)" }
        return $frame.result
    }
}

if (-not (Test-Path -LiteralPath $SessionPath -PathType Leaf)) {
    throw "session not found: $SessionPath"
}
if (-not (Test-Path -LiteralPath $engineExe -PathType Leaf)) {
    throw "engine not found: $engineExe"
}

$existing = Get-Process roudamix-engine -ErrorAction SilentlyContinue
if ($existing) { throw 'roudamix-engine is already running; close RoudaMix before this smoke test' }

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$output = [IO.Path]::GetFullPath((Join-Path $tempRoot "roudamix-save-smoke-$PID.rmsession"))
if (-not $output.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing temp path outside temp root: $output"
}

$engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden
$pipe = New-Object IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect(5000)
    $snapshot = Read-Frame $pipe
    if ($snapshot.kind -ne 'snapshot') { throw 'first engine frame was not snapshot' }
    if (@($snapshot.payload.capabilities) -notcontains 'pluginLatencyPdcV1') {
        throw 'engine did not advertise pluginLatencyPdcV1'
    }

    $loaded = Invoke-Engine $pipe 'load_session' @{ path = [IO.Path]::GetFullPath($SessionPath) }
    $live = Invoke-Engine $pipe 'get_snapshot' @{}
    $slots = @($live.snapshot.status.tracks | ForEach-Object { @($_.plugins) })
    if ($slots.Count -gt 0) {
        foreach ($slot in $slots) {
            if ($null -eq $slot.latencySamples) { throw "plugin latency missing: $($slot.name)" }
            if ($slot.runtimeState -notin @('active', 'preparing', 'degraded', 'suspended')) {
                throw "plugin runtime state missing: $($slot.name)"
            }
        }
    }
    $latency = Invoke-Engine $pipe 'get_latency_report' @{}
    if (-not $latency.report.ok) { throw "latency report failed: $($latency.report.error)" }
    if (@($latency.report.outputs).Count -lt 2) { throw 'latency report lost system outputs' }
    if ($slots.Count -gt 0) {
        $probeId = [uint32]$slots[0].instanceId
        [void](Invoke-Engine $pipe 'set_monitor_bypass' @{ instanceId = $probeId; bypassed = $true })
        $shadowSnapshot = Invoke-Engine $pipe 'get_snapshot' @{}
        $shadowSlot = @($shadowSnapshot.snapshot.status.tracks | ForEach-Object { @($_.plugins) }) |
            Where-Object { $_.instanceId -eq $probeId } | Select-Object -First 1
        if (-not $shadowSlot.monitorBypassed -or $null -eq $shadowSlot.monitorLatencySamples) {
            throw "Monitor Bypass shadow was not prepared for instance $probeId"
        }
        [void](Invoke-Engine $pipe 'set_monitor_bypass' @{ instanceId = $probeId; bypassed = $false })
    }
    $saved = Invoke-Engine $pipe 'save_session' @{ path = $output }
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) { throw 'save_session did not create output' }
    $json = Get-Content -Raw -LiteralPath $output | ConvertFrom-Json
    if ($json.roudamixSession -ne 3) { throw "expected Session v3, got $($json.roudamixSession)" }
    if ($json.tracks.Count -lt 1) { throw 'saved Session lost all tracks' }
    foreach ($track in $json.tracks) {
        foreach ($slot in @($track.plugins)) {
            if ($slot.PSObject.Properties.Name -contains 'runtimeState' -or
                $slot.PSObject.Properties.Name -contains 'monitorState') {
                throw "runtime-derived state leaked into Session: $($slot.name)"
            }
        }
    }
    Write-Host "SESSION/LATENCY SAVE SMOKE PASSED: tracks=$($json.tracks.Count) plugins=$($slots.Count) outputs=$(@($latency.report.outputs).Count) bytes=$((Get-Item -LiteralPath $output).Length) revision=$($saved.revision)"
    [void](Invoke-Engine $pipe 'shutdown_engine' @{})
} finally {
    $pipe.Dispose()
    if (-not $engine.HasExited) {
        $engine.WaitForExit(3000) | Out-Null
        if (-not $engine.HasExited) { $engine.Kill() }
    }
    if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Force }
    $backup = "$output.bak"
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }
}
