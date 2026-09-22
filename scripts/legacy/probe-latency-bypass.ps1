# 診斷:每 plugin latencySamples + bypass THE VOID 前後 callbackLoad 對比
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
        if ($n -le 0) { throw 'EOF' }
        $read += $n
    }
    $size = [BitConverter]::ToUInt32($len, 0)
    $body = New-Object byte[] $size
    $read = 0
    while ($read -lt $size) {
        $n = $Stream.Read($body, $read, $size - $read)
        if ($n -le 0) { throw 'EOF' }
        $read += $n
    }
    [Text.Encoding]::UTF8.GetString($body) | ConvertFrom-Json
}
function Send-Frame([System.IO.Stream]$Stream, $Frame) {
    $json = $Frame | ConvertTo-Json -Depth 12 -Compress
    $body = [Text.Encoding]::UTF8.GetBytes($json)
    $Stream.Write([BitConverter]::GetBytes([uint32]$body.Length), 0, 4)
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

if (Get-Process roudamix-engine -ErrorAction SilentlyContinue) {
    throw 'roudamix-engine already running'
}

$engine = Start-Process -FilePath $engineExe -PassThru -WindowStyle Hidden
$pipe = New-Object IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect(5000)
    [void](Read-Frame $pipe)
    $loaded = Invoke-Engine $pipe 'load_session' @{ path = [IO.Path]::GetFullPath($SessionPath) }
    $startPayload = @{ deviceKey = $loaded.deviceKey }
    if ($loaded.sampleRate) { $startPayload.sampleRate = [uint32]$loaded.sampleRate }
    if ($loaded.bufferSize) { $startPayload.bufferSize = [uint32]$loaded.bufferSize }
    [void](Invoke-Engine $pipe 'start' $startPayload)
    Start-Sleep -Milliseconds 800

    function Read-Telemetry {
        $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
        $accessor = $mapping.CreateViewAccessor(0, 7232)
        try {
            for ($try = 0; $try -lt 20; $try++) {
                $s1 = $accessor.ReadUInt32(8)
                $load = $accessor.ReadSingle(24)
                $s2 = $accessor.ReadUInt32(8)
                if ($s1 -eq $s2 -and (($s1 -band 1) -eq 0)) { return $load }
                Start-Sleep -Milliseconds 5
            }
            return -1.0
        } finally {
            $accessor.Dispose()
            $mapping.Dispose()
        }
    }
    function Show-Plugins($snap, $label) {
        Write-Host "[$label]"
        foreach ($t in $snap.snapshot.status.tracks) {
            foreach ($p in @($t.plugins)) {
                Write-Host ('  {0,-18} avail={1,-7} bypassed={2,-5} latencySamples={3} monitorState={4}' -f $p.name, $p.availability, $p.bypassed, $p.latencySamples, $p.monitorState)
            }
        }
        $sum = $snap.snapshot.status.pluginDelay
        Write-Host ('  pluginDelay: primary={0} monitor={1} stream={2}' -f $sum.primarySamples, $sum.monitorSamples, $sum.streamSamples)
    }

    $snap1 = Invoke-Engine $pipe 'get_snapshot' @{}
    Show-Plugins $snap1 'before bypass'
    $l1 = Read-Telemetry
    Write-Host ('callbackLoad(bypass前) = {0:P1}' -f $l1)

    $target = @($snap1.snapshot.status.tracks | ForEach-Object { @($_.plugins) } |
        Where-Object { $_.name -match 'VOID' }) | Select-Object -First 1
    if ($target) {
        [void](Invoke-Engine $pipe 'set_bypass' @{ instanceId = [uint32]$target.instanceId; bypassed = $true })
        Start-Sleep -Seconds 2
        $l2 = Read-Telemetry
        Write-Host ('callbackLoad(VOID bypass) = {0:P1}' -f $l2)
        [void](Invoke-Engine $pipe 'set_bypass' @{ instanceId = [uint32]$target.instanceId; bypassed = $false })
        Start-Sleep -Seconds 2
        $l3 = Read-Telemetry
        Write-Host ('callbackLoad(VOID 恢復) = {0:P1}' -f $l3)
    }

    [void](Invoke-Engine $pipe 'stop' @{})
    [void](Invoke-Engine $pipe 'shutdown_engine' @{})
} finally {
    $pipe.Dispose()
    if (-not $engine.HasExited) {
        $engine.WaitForExit(3000) | Out-Null
        if (-not $engine.HasExited) { $engine.Kill() }
    }
}
