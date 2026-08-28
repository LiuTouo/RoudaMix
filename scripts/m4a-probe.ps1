# M4a pipe probe:有限、可失敗。連線循環(add/set_param/remove/斷線)驗證
# engine 跨 thread 修復。任何 EOF/超時/幀損壞 = 立即 fail,不無界循環。
# 用法:m4a-probe.ps1 [-Rounds 5]
param([int]$Rounds = 5)

$ErrorActionPreference = 'Stop'
$exe = 'D:\Administrator\Documents\GitHub\RoudaMix\engine\build\Release\roudamix-engine.exe'
$MaxFrameBytes = 1048576

function Read-Msg([System.IO.BinaryReader]$rx) {
    $len = $rx.ReadUInt32()
    if ($len -gt $MaxFrameBytes) { throw "frame len $len exceeds 1 MiB cap" }
    $bytes = $rx.ReadBytes($len)
    if ($bytes.Length -ne $len) { throw "short read: got $($bytes.Length) of $len" }
    [System.Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
}

function Wait-Reply([System.IO.BinaryReader]$rx, [uint64]$id, [int]$TimeoutMs = 10000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $frames = 0
    while ($true) {
        if ([DateTime]::UtcNow -gt $deadline) { throw "reply id=$id timeout after $TimeoutMs" }
        if (++$frames -gt 200) { throw "reply id=${id}: over 200 frames without match" }
        $m = Read-Msg $rx
        if ($null -ne ($m.PSObject.Properties['id']) -and [uint64]$m.id -eq $id) { return $m }
    }
}

function Send-Cmd([System.IO.BinaryWriter]$bw, [uint64]$id, [string]$kind, $payload) {
    $o = @{ id = $id; kind = $kind; payload = $payload; protocolVersion = 1 } |
        ConvertTo-Json -Compress -Depth 5
    $b = [System.Text.Encoding]::UTF8.GetBytes($o)
    $bw.Write([uint32]$b.Length); $bw.Write($b); $bw.Flush()
}

# --- 啟動 + readiness ping(取代固定 sleep) ---
Get-Process roudamix-engine -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -RedirectStandardError "$env:TEMP\rmx-err.txt" `
    -RedirectStandardOutput "$env:TEMP\rmx-out.txt" -PassThru -WindowStyle Hidden
try {
    $ready = $false
    for ($i = 0; $i -lt 100 -and -not $ready; $i++) {
        Start-Sleep -Milliseconds 100
        try {
            $c = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine',
                [System.IO.Pipes.PipeDirection]::InOut)
            $c.Connect(500)
            $rx = New-Object System.IO.BinaryReader($c)
            $bw = New-Object System.IO.BinaryWriter($c)
            $null = Read-Msg $rx                    # snapshot push = ready
            Send-Cmd $bw 1 'ping' @{}
            $r = Wait-Reply $rx 1 3000
            if (-not $r.ok) { throw "ping not ok" }
            $ready = $true
            "ready after $(($i + 1) * 100)ms: engineVersion=$($r.result.engineVersion)"
            $c.Close()
        } catch {
            if ($_.Exception -is [System.TimeoutException]) { throw }
            if ($c) { $c.Close() }
            continue                                # pipe 未開 = 還沒起來,重試
        }
    }
    if (-not $ready) { throw "engine not ready after 10s" }

    # --- stress:連線循環,每輪 scan → add → set_param x20 → remove → 斷線 ---
    for ($round = 1; $round -le $Rounds; $round++) {
        $client = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine',
            [System.IO.Pipes.PipeDirection]::InOut)
        $client.Connect(3000)
        $rx = New-Object System.IO.BinaryReader($client)
        $bw = New-Object System.IO.BinaryWriter($client)
        $null = Read-Msg $rx                        # snapshot
        Send-Cmd $bw 10 'scan_plugins' @{}
        $scan = Wait-Reply $rx 10 30000
        $mod = $scan.result.plugins | Where-Object { $_.classes.Count -gt 0 } |
            Select-Object -First 1
        Send-Cmd $bw 11 'add_plugin' @{ path = $mod.path; classId = $mod.classes[0].uid }
        $add = Wait-Reply $rx 11 15000
        if (-not $add.ok) { throw "add_plugin failed: $($add.error.message)" }
        $iid = $add.result.instanceId
        $paramId = $add.result.rack[-1].params[0].paramId   # param id 非連續,取第一個
        for ($i = 0; $i -lt 20; $i++) {
            Send-Cmd $bw ([uint64](100 + $i)) 'set_param' `
                @{ instanceId = $iid; paramId = $paramId; value = 0.5 }
            $r = Wait-Reply $rx ([uint64](100 + $i)) 5000
            if (-not $r.ok) { throw "set_param failed: $($r.error.message)" }
        }
        Send-Cmd $bw 12 'remove_plugin' @{ instanceId = $iid }
        $r = Wait-Reply $rx 12 10000
        if (-not $r.ok) { throw "remove_plugin failed: $($r.error.message)" }
        $client.Close()
        Start-Sleep -Milliseconds 200
        "round ${round}: ok  iid=$iid  set_param x20  ($(Split-Path $mod.path -Leaf))"
    }

    if ($p.HasExited) { throw "engine exited unexpectedly (code $($p.ExitCode))" }
    Write-Output "PASS: $Rounds rounds, engine alive"
    exit 0
} catch {
    Write-Output "FAIL: $($_.Exception.Message)"
    Write-Output "engine alive: $(-not $p.HasExited)"
    Write-Output '--- engine stderr ---'
    Get-Content "$env:TEMP\rmx-err.txt" -ErrorAction SilentlyContinue |
        Select-Object -Last 10
    exit 1
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
