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
if (Get-Process roudamix-engine -ErrorAction SilentlyContinue) {
    throw 'roudamix-engine is already running; close RoudaMix before this smoke test'
}

$engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden
$pipe = New-Object IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect(5000)
    $first = Read-Frame $pipe
    if (@($first.payload.capabilities) -notcontains 'pluginLatencyPdcV1') {
        throw 'pluginLatencyPdcV1 capability missing'
    }
    $loaded = Invoke-Engine $pipe 'load_session' @{ path = [IO.Path]::GetFullPath($SessionPath) }
    $before = Invoke-Engine $pipe 'get_snapshot' @{}
    $slot = @($before.snapshot.status.tracks | ForEach-Object { @($_.plugins) }) |
        Where-Object { -not $_.bypassed -and $_.availability -eq 'ok' } | Select-Object -First 1
    if ($null -eq $slot) { throw 'session has no active plugin for shadow test' }

    $startPayload = @{ deviceKey = $loaded.deviceKey }
    if ($loaded.sampleRate) { $startPayload.sampleRate = [uint32]$loaded.sampleRate }
    if ($loaded.bufferSize) { $startPayload.bufferSize = [uint32]$loaded.bufferSize }
    $started = Invoke-Engine $pipe 'start' $startPayload
    if (-not $started.running) { throw 'engine did not enter running state' }

    $watch = [Diagnostics.Stopwatch]::StartNew()
    [void](Invoke-Engine $pipe 'set_monitor_bypass' @{
        instanceId = [uint32]$slot.instanceId
        bypassed = $true
    })
    $watch.Stop()
    Start-Sleep -Milliseconds 250
    $after = Invoke-Engine $pipe 'get_snapshot' @{}
    $shadow = @($after.snapshot.status.tracks | ForEach-Object { @($_.plugins) }) |
        Where-Object { $_.instanceId -eq $slot.instanceId } | Select-Object -First 1
    if (-not $shadow.monitorBypassed -or $null -eq $shadow.monitorLatencySamples) {
        throw 'running Monitor Shadow did not enter graph'
    }
    if ($shadow.monitorState -ne 'active') {
        throw "running Monitor Shadow state is $($shadow.monitorState)"
    }
    if ($after.snapshot.status.pluginFails -ne 0) {
        throw "plugin process failures after shadow commit: $($after.snapshot.status.pluginFails)"
    }
    $delaySummary = $after.snapshot.status.pluginDelay
    if ($null -eq $delaySummary.monitorSamples -or $null -eq $delaySummary.streamSamples) {
        throw 'running status lost Monitor/Stream Total Plugin Delay'
    }
    $report = Invoke-Engine $pipe 'get_latency_report' @{}
    if (-not $report.report.ok -or @($report.report.outputs).Count -lt 2) {
        throw 'running latency report is incomplete'
    }
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
    $accessor = $mapping.CreateViewAccessor(0, 7232)
    try {
        if ($accessor.ReadUInt32(4) -ne 4) { throw 'telemetry ABI is not v4' }
        $loadCount = [Math]::Min([int]$accessor.ReadUInt32(3124), 256)
        if ($loadCount -lt 1) { throw 'telemetry contains no Plugin Process Load entries' }
        $seenShadow = $false
        for ($i = 0; $i -lt $loadCount; $i++) {
            $offset = 3132 + 16 * $i
            $variant = $accessor.ReadUInt32($offset + 4)
            $load = $accessor.ReadSingle($offset + 8)
            if ([Single]::IsNaN($load) -or [Single]::IsInfinity($load)) {
                throw "non-finite Plugin Process Load at index $i"
            }
            if ($variant -eq 1) { $seenShadow = $true }
            if ($seenShadow -and $variant -eq 0) {
                throw 'Plugin Process Load allocation is not primary-first'
            }
        }
        if (-not $seenShadow) { throw 'telemetry contains no Monitor Shadow entry' }
    } finally {
        $accessor.Dispose()
        $mapping.Dispose()
    }
    [void](Invoke-Engine $pipe 'set_monitor_bypass' @{
        instanceId = [uint32]$slot.instanceId
        bypassed = $false
    })
    [void](Invoke-Engine $pipe 'stop' @{})
    Write-Host "LATENCY RUNTIME SMOKE PASSED: plugin='$($slot.name)' shadowLatency=$($shadow.monitorLatencySamples) monitorTotal=$($delaySummary.monitorSamples) streamTotal=$($delaySummary.streamSamples) pluginLoads=$loadCount preRollCommitMs=$($watch.ElapsedMilliseconds)"
    [void](Invoke-Engine $pipe 'shutdown_engine' @{})
} finally {
    $pipe.Dispose()
    if (-not $engine.HasExited) {
        $engine.WaitForExit(3000) | Out-Null
        if (-not $engine.HasExited) { $engine.Kill() }
    }
}
