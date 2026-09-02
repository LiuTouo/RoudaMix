param([switch]$CallbackOnly)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$engineExe = Join-Path $root 'engine\build\Release\roudamix-engine.exe'
$fixture = Join-Path $root 'engine\build\Release\rmx_latency_fixture.vst3'

function Read-Frame([IO.Stream]$Stream) {
    $len = New-Object byte[] 4; $read = 0
    while ($read -lt 4) { $n = $Stream.Read($len, $read, 4 - $read); if ($n -le 0) { throw 'EOF' }; $read += $n }
    $size = [BitConverter]::ToUInt32($len, 0)
    $body = New-Object byte[] $size; $read = 0
    while ($read -lt $size) { $n = $Stream.Read($body, $read, $size - $read); if ($n -le 0) { throw 'EOF' }; $read += $n }
    [Text.Encoding]::UTF8.GetString($body) | ConvertFrom-Json
}
function Send-Frame([IO.Stream]$Stream, $Frame) {
    $json = $Frame | ConvertTo-Json -Depth 12 -Compress
    $body = [Text.Encoding]::UTF8.GetBytes($json)
    $len = [BitConverter]::GetBytes([uint32]$body.Length)
    $Stream.Write($len, 0, 4); $Stream.Write($body, 0, $body.Length); $Stream.Flush()
}
$script:id = 0
function Invoke-Engine([IO.Stream]$Stream, [string]$Kind, $Payload) {
    $script:id++
    Send-Frame $Stream @{ protocolVersion = 2; id = $script:id; kind = $Kind; payload = $Payload }
    while ($true) {
        $f = Read-Frame $Stream
        if ($f.id -eq 0 -and -not $f.ok) { throw "engine rejected frame: $($f.error.message)" }
        if ($null -eq $f.id -or $f.id -ne $script:id) { continue }
        if (-not $f.ok) { throw "$Kind failed: $($f.error.code) $($f.error.message)" }
        return $f.result
    }
}
function Snapshot([IO.Stream]$Stream) { (Invoke-Engine $Stream 'get_snapshot' @{}).snapshot }
function Find-Slot($Snapshot, [uint32]$InstanceId) {
    @($Snapshot.status.tracks | ForEach-Object { @($_.plugins) }) |
        Where-Object { $_.instanceId -eq $InstanceId } | Select-Object -First 1
}
function Wait-Slot([IO.Stream]$Stream, [uint32]$InstanceId, [scriptblock]$Predicate, [string]$Message) {
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        $snap = Snapshot $Stream
        $slot = Find-Slot $snap $InstanceId
        if (& $Predicate $slot) { return @{ snapshot = $snap; slot = $slot } }
    }
    throw $Message
}

if (-not (Test-Path -LiteralPath $fixture -PathType Leaf)) { throw "fixture missing: $fixture" }
if (Get-Process roudamix-engine -ErrorAction SilentlyContinue) { throw 'roudamix-engine already running' }
$engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden
$pipe = New-Object IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect(5000); $null = Read-Frame $pipe
    $sys = Invoke-Engine $pipe 'ensure_system_outputs' @{}
    $monitor = @($sys.tracks | Where-Object { $_.systemRole -eq 'monitor' })[0]
    $stream = @($sys.tracks | Where-Object { $_.systemRole -eq 'stream' })[0]
    $fast = Invoke-Engine $pipe 'track_add' @{ kind = 'audio'; name = 'Fixture Fast' }
    $slow = Invoke-Engine $pipe 'track_add' @{ kind = 'audio'; name = 'Fixture Slow' }
    $monitorId = [Convert]::ToUInt32($monitor.trackId)
    $streamId = [Convert]::ToUInt32($stream.trackId)
    $fastId = [Convert]::ToUInt32($fast.trackId)
    $slowId = [Convert]::ToUInt32($slow.trackId)
    foreach ($trackId in @($fastId, $slowId)) {
        [void](Invoke-Engine $pipe 'track_set_source' @{ trackId = $trackId; source = @{ type = 'sine'; freq = 440 } })
        if ($CallbackOnly -and $trackId -eq $fastId) { continue }
        [uint32[]]$destinations = if ($CallbackOnly) { @($streamId) } else { @($monitorId, $streamId) }
        [void](Invoke-Engine $pipe 'track_set_dests' @{ trackId = $trackId; dests = $destinations })
    }
    $added = Invoke-Engine $pipe 'add_plugin' @{ trackId = $slowId; path = $fixture }
    $instance = [uint32]$added.instanceId
    Write-Host 'fixture stage: graph prepared'
    $devices = Invoke-Engine $pipe 'list_devices' @{}
    $device = @($devices.devices | Where-Object { $_.sampleRates -contains 48000 })[0]
    if ($null -eq $device) { throw 'no ASIO device supports 48 kHz' }
    $buffer = if ($device.bufferSizes -contains 128) { 128 } else { [uint32]$device.preferredBufferSize }
    [void](Invoke-Engine $pipe 'start' @{ deviceKey = $device.deviceKey; sampleRate = 48000; bufferSize = $buffer })
    Write-Host 'fixture stage: ASIO running'

    [void](Invoke-Engine $pipe 'set_param' @{ instanceId = $instance; paramId = 100; value = 128.0 / 192000.0 })
    $active = Wait-Slot $pipe $instance { param($s) $s.latencySamples -eq 128 -and $s.runtimeState -eq 'active' } 'fixture latency did not become active'
    Write-Host 'fixture stage: dynamic latency + shadow active'
    if ($CallbackOnly) {
        [void](Invoke-Engine $pipe 'stop' @{})
        [void](Invoke-Engine $pipe 'shutdown_engine' @{})
        Write-Host 'LATENCY FIXTURE CALLBACK SMOKE PASSED'
        return
    }
    $report = (Invoke-Engine $pipe 'get_latency_report' @{}).report
    $streamReport = @($report.outputs | Where-Object { $_.trackId -eq $streamId })[0]
    $monitorReport = @($report.outputs | Where-Object { $_.trackId -eq $monitorId })[0]
    if ($streamReport.totalPluginDelaySamples -ne 128 -or $streamReport.compensationDelaySamples -ne 128 -or -not $streamReport.synchronized) {
        throw 'full-PDC fixture report is not sample-exact'
    }
    if ($monitorReport.compensationDelaySamples -ne 0 -or $monitorReport.synchronized) {
        throw 'low-latency fixture path incorrectly received PDC'
    }
    if ($null -eq $active.slot.monitorLatencySamples) { throw 'PDC divergence did not create Monitor Shadow' }
    Write-Host 'fixture stage: PDC report verified'

    [void](Invoke-Engine $pipe 'set_monitor_bypass' @{ instanceId = $instance; bypassed = $true })
    $bypassedReport = (Invoke-Engine $pipe 'get_latency_report' @{}).report
    $monitorBypassed = @($bypassedReport.outputs | Where-Object { $_.trackId -eq $monitorId })[0]
    if ($monitorBypassed.totalPluginDelaySamples -ne 0) { throw 'Monitor Bypass did not remove monitor latency' }
    [void](Invoke-Engine $pipe 'set_monitor_bypass' @{ instanceId = $instance; bypassed = $false })
    Write-Host 'fixture stage: Monitor Bypass verified'

    [void](Invoke-Engine $pipe 'set_param' @{ instanceId = $instance; paramId = 101; value = 0.5 })
    Start-Sleep -Milliseconds 300
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
    $accessor = $mapping.CreateViewAccessor(0, 7232)
    try {
        $count = [Math]::Min([int]$accessor.ReadUInt32(3124), 256)
        $load = 0.0
        for ($i = 0; $i -lt $count; $i++) {
            $offset = 3132 + 16 * $i
            if ($accessor.ReadUInt32($offset) -eq $instance -and $accessor.ReadUInt32($offset + 4) -eq 0) {
                $load = $accessor.ReadSingle($offset + 8); break
            }
        }
        if ($load -le 0) { throw 'controlled CPU work did not produce Process Load' }
    } finally { $accessor.Dispose(); $mapping.Dispose() }
    Write-Host 'fixture stage: Process Load verified'

    [void](Invoke-Engine $pipe 'set_param' @{ instanceId = $instance; paramId = 102; value = 1.0 })
    Start-Sleep -Milliseconds 200
    if ((Snapshot $pipe).status.pluginFails -lt 1) { throw 'controlled process failure was not observed' }
    Write-Host 'fixture stage: process failure verified'
    [void](Invoke-Engine $pipe 'set_param' @{ instanceId = $instance; paramId = 102; value = 0.0 })
    [void](Invoke-Engine $pipe 'set_param' @{ instanceId = $instance; paramId = 100; value = 1.0 })
    $suspended = Wait-Slot $pipe $instance { param($s) $s.runtimeState -eq 'suspended' } 'over-limit runtime latency did not suspend primary'
    Write-Host 'fixture stage: over-limit suspended'
    [void](Invoke-Engine $pipe 'set_param' @{ instanceId = $instance; paramId = 100; value = 128.0 / 192000.0 })
    $recovered = Wait-Slot $pipe $instance { param($s) $s.runtimeState -eq 'active' -and $s.latencySamples -eq 128 } 'valid latency retry did not recover primary'
    Write-Host 'fixture stage: valid latency recovered'
    [void](Invoke-Engine $pipe 'stop' @{})
    Write-Host "LATENCY FIXTURE ENGINE SMOKE PASSED: PDC=128 load=$([Math]::Round($load * 100, 2))% suspend/recover=OK"
    [void](Invoke-Engine $pipe 'shutdown_engine' @{})
} finally {
    $pipe.Dispose()
    if (-not $engine.HasExited) { $engine.WaitForExit(3000) | Out-Null; if (-not $engine.HasExited) { $engine.Kill() } }
}
