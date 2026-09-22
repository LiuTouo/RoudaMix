# 診斷用:載入 session + start,抽樣 telemetry SHM 的 callbackLoad 與 per-plugin load
param(
    [Parameter(Mandatory = $true)]
    [string]$SessionPath,
    [int]$Seconds = 6
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
    [void](Read-Frame $pipe)  # hello
    $loaded = Invoke-Engine $pipe 'load_session' @{ path = [IO.Path]::GetFullPath($SessionPath) }
    $startPayload = @{ deviceKey = $loaded.deviceKey }
    if ($loaded.sampleRate) { $startPayload.sampleRate = [uint32]$loaded.sampleRate }
    if ($loaded.bufferSize) { $startPayload.bufferSize = [uint32]$loaded.bufferSize }
    [void](Invoke-Engine $pipe 'start' $startPayload)
    Start-Sleep -Milliseconds 500

    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
    $accessor = $mapping.CreateViewAccessor(0, 7232)
    try {
        $n = [Math]::Ceiling($Seconds / 2)
        for ($k = 0; $k -lt $n; $k++) {
            Start-Sleep -Milliseconds 2000
            # seqlock 讀:sequence 前後一致且 even 才收
            $load = 0.0; $bufSize = 0; $sr = 0.0
            for ($try = 0; $try -lt 20; $try++) {
                $s1 = $accessor.ReadUInt32(8)
                $load = $accessor.ReadSingle(24)
                $sr = $accessor.ReadSingle(28)
                $bufSize = $accessor.ReadUInt32(32)
                $s2 = $accessor.ReadUInt32(8)
                if ($s1 -eq $s2 -and (($s1 -band 1) -eq 0)) { break }
                Start-Sleep -Milliseconds 5
            }
            $parts = @()
            $loadCount = [Math]::Min([int]$accessor.ReadUInt32(3124), 256)
            $names = @{}
            $snap = Invoke-Engine $pipe 'get_snapshot' @{}
            foreach ($t in $snap.snapshot.status.tracks) {
                foreach ($p in @($t.plugins)) { $names[[uint32]$p.instanceId] = $p.name }
            }
            for ($i = 0; $i -lt $loadCount; $i++) {
                $off = 3132 + 16 * $i
                $iid = $accessor.ReadUInt32($off)
                $variant = $accessor.ReadUInt32($off + 4)
                $pl = $accessor.ReadSingle($off + 8)
                $label = if ($names.ContainsKey($iid)) { $names[$iid] } else { "inst$iid" }
                if ($variant -eq 1) { $label = "$label(shadow)" }
                $parts += ('{0}={1:P1}' -f $label, $pl)
            }
            Write-Host ('t+{0,2}s callbackLoad={1,6:P1} sr={2} buf={3} plugins: {4}' -f (($k + 1) * 2), $load, [int]$sr, $bufSize, ($parts -join '  '))
        }
    } finally {
        $accessor.Dispose()
        $mapping.Dispose()
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
