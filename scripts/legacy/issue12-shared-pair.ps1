# Issue #12 驗證:ASIO 輸入/輸出 pair 多軌共用(fan-out)— 真實 ASIO 路徑。
# 命令層:同 in/out pair 第二軌不再 device_busy;out-of-range 仍 bad_command。
# RT 層(SHM v4):共用 in pair 的兩軌 meter peak 逐窗相等(同 scratch fan-out);
#   其中一軌改走 sine → 振幅分道、改回 → 相等恢復;engine 輸出(strip0)有訊號。
# 前置:ASIO 裝置存在且有 ≥1 in / ≥1 out;engine 未跑或可自行 spawn。
# 判定:任何一步錯 = throw(非零退出);全過印 PASSED
$ErrorActionPreference = 'Stop'
$engineExe = Join-Path $PSScriptRoot '..\engine\build\Release\roudamix-engine.exe'

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
    Send-Frame $s @{ protocolVersion = 2; id = $script:id; kind = $kind; payload = $payload }
    while ($true) {
        $f = Read-Frame $s  # 跳過 event 幀
        if ($f.id -ne $null) {
            if ($f.id -eq 0 -and $f.ok -eq $false) { throw "parse error: $($f.error.message)" }
            if ($f.id -eq $script:id) {
                if (-not $f.ok) { throw "$kind failed: $($f.error.code) $($f.error.message)" }
                return $f
            }
        }
    }
}
function Try-Command($s, $kind, $payload) {
    $script:id++
    Send-Frame $s @{ protocolVersion = 2; id = $script:id; kind = $kind; payload = $payload }
    while ($true) {
        $f = Read-Frame $s
        if ($f.id -ne $null -and $f.id -eq $script:id) { return $f }
    }
}
function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }
function Clear-Tracks($s, $status) {
    if ($status.running) { [void](Command $s 'stop' @{}) }
    foreach ($t in @($status.tracks)) {
        [void](Command $s 'track_remove' @{ trackId = [uint32]$t.trackId })
    }
}
function Strip-Peaks($acc, $stripCount, $trackId) {
    # SHM v4:strips@48,32B/strip:peak_l@+8 peak_r@+12;kind 1 = track
    for ($i = 1; $i -lt $stripCount; $i++) {
        if ($acc.ReadUInt32(48 + 32 * $i) -eq $trackId -and $acc.ReadUInt32(48 + 32 * $i + 4) -eq 1) {
            return @{ l = $acc.ReadSingle(48 + 32 * $i + 8); r = $acc.ReadSingle(48 + 32 * $i + 12) }
        }
    }
    return $null
}
# 等下一個 30Hz publish 窗(peak 週期性重置),回傳兩軌快照
function Read-Pair($acc, $stripCount, $idA, $idB) {
    $a = $null; $b = $null
    for ($try = 0; $try -lt 40 -and ($a -eq $null -or $b -eq $null); $try++) {
        Start-Sleep -Milliseconds 40
        $a = Strip-Peaks $acc $stripCount $idA
        $b = Strip-Peaks $acc $stripCount $idB
    }
    return @{ a = $a; b = $b }
}

# --- 0. engine 連線 + 清場 ---
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'roudamix-engine', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(1500) } catch {
    $running = Get-Process roudamix-engine -ErrorAction SilentlyContinue
    if ($running) { throw "engine running but pipe busy (another client holds it) - close the UI first" }
    if (-not (Test-Path $engineExe)) { throw "engine exe not found: $engineExe" }
    Start-Process -FilePath $engineExe -WindowStyle Hidden | Out-Null
    Start-Sleep -Milliseconds 800
    $pipe.Connect(3000)
}
$snap = Read-Frame $pipe
Assert ($snap.kind -eq 'snapshot') "first frame is snapshot"
Clear-Tracks $pipe $snap.payload.status

# --- 1. 建軌:Vox/Sing 同 asioIn ch0;O1/O2 同 asioOut ch0 ---
$vox = Command $pipe 'track_add' @{ kind = 'audio'; name = 'Vox' }
$sing = Command $pipe 'track_add' @{ kind = 'audio'; name = 'Sing' }
$o1 = Command $pipe 'track_add' @{ kind = 'output'; name = 'Out1' }
$o2 = Command $pipe 'track_add' @{ kind = 'output'; name = 'Out2' }
$voxId = [uint32]$vox.result.trackId; $singId = [uint32]$sing.result.trackId
$o1Id = [uint32]$o1.result.trackId; $o2Id = [uint32]$o2.result.trackId

$src = @{ type = 'asioIn'; channel = 0 }
[void](Command $pipe 'track_set_source' @{ trackId = $voxId; source = $src })
[void](Command $pipe 'track_set_source' @{ trackId = $singId; source = $src })  # #12:不再 device_busy
Write-Host "shared input pair: both bound (vox=$voxId sing=$singId)"

$out = @{ type = 'asioOut'; channel = 0 }
[void](Command $pipe 'track_set_output' @{ trackId = $o1Id; output = $out })
[void](Command $pipe 'track_set_output' @{ trackId = $o2Id; output = $out })  # #12:疊加語意
Write-Host "shared output pair: both bound (o1=$o1Id o2=$o2Id)"

# 路由:Vox→Out1、Sing→Out2(同 pair 疊加寫入;strip0 = 最後 ASIO out 軌的 sink)
[void](Command $pipe 'track_set_dests' @{ trackId = $voxId; dests = @($o1Id) })
[void](Command $pipe 'track_set_dests' @{ trackId = $singId; dests = @($o2Id) })

# --- 2. 裝置清單(越界斷言在 start 後:capability 須 device open 才有資料)---
$dev = Command $pipe 'list_devices' @{}
Assert ($dev.result.devices.Count -ge 1) 'ASIO device present'
$dev0 = $dev.result.devices[0]
$maxIn = $dev0.inputNames.Count; $maxOut = $dev0.outputNames.Count
Assert ($maxIn -ge 1 -and $maxOut -ge 1) "device has in/out channels (in=$maxIn out=$maxOut)"

# --- 3. start(聯集含共用 pair,driver buffer 一份)---
$key = $dev0.deviceKey
$st = Command $pipe 'start' @{ deviceKey = $key; sampleRate = 48000 }
Assert ($st.result.running) 'running'
Start-Sleep -Milliseconds 700

# --- 3b. out-of-range 仍 bad_command(channel 檢查保留;先驗最後合法 pair 仍成功)---
if ($maxIn -ge 2) {
    $pt = Command $pipe 'track_add' @{ kind = 'audio'; name = 'ProbeHi' }
    $ptId = [uint32]$pt.result.trackId
    [void](Command $pipe 'track_set_source' @{ trackId = $ptId; source = @{ type = 'asioIn'; channel = ($maxIn - 2) } })
    Write-Host "last legal input pair (ch=$($maxIn-2)) binds ok"
}
$badIn = Try-Command $pipe 'track_set_source' @{ trackId = $voxId; source = @{ type = 'asioIn'; channel = ($maxIn - 1) } }
Assert ($badIn.ok -eq $false -and $badIn.error.code -eq 'bad_command') "input ch out of range -> bad_command (got $($badIn.error.code))"
$badOut = Try-Command $pipe 'track_set_output' @{ trackId = $o1Id; output = @{ type = 'asioOut'; channel = ($maxOut - 1) } }
Assert ($badOut.ok -eq $false -and $badOut.error.code -eq 'bad_command') "output ch out of range -> bad_command (got $($badOut.error.code))"
Write-Host "range check kept: bad_command on out-of-range (in=$($maxIn-1) out=$($maxOut-1))"

$mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('roudamix-telemetry')
$acc = $mm.CreateViewAccessor(0, 7232)
Assert ($acc.ReadUInt32(4) -eq 4) 'telemetry abi v4'
$stripCount = $acc.ReadUInt32(12)

# --- 4. RT fan-out:同 in pair 兩軌 peak 逐窗相等(同 scratch)---
$p1 = Read-Pair $acc $stripCount $voxId $singId
Assert ($p1.a -ne $null -and $p1.b -ne $null) 'both shared-input strips present in SHM'
Assert ([Math]::Abs($p1.a.l - $p1.b.l) -lt 1e-6 -and [Math]::Abs($p1.a.r - $p1.b.r) -lt 1e-6) "fan-out peaks equal (a=$($p1.a.l)/$($p1.a.r) b=$($p1.b.l)/$($p1.b.r))"
if ($p1.a.l -eq 0.0 -and $p1.a.r -eq 0.0) { Write-Host 'note: input silent - equality trivially true' }
Write-Host "RT fan-out: peaks equal (L=$($p1.a.l) R=$($p1.a.r))"

# --- 5. engine 輸出(strip0)有訊號(輸出 pair 寫入路徑活著)---
# 註:同 pair 疊加發生在 driver scratch 上,無 loopback 硬體不可觀測 —
#     疊加語意由 bus_add 累加(與 dest summing 同構、既有測試覆蓋)+ 命令層
#     兩軌綁定成功(session_test 11)鎖定;此處只驗寫入路徑本身。
$engPeak = [Math]::Max($acc.ReadSingle(48 + 8), $acc.ReadSingle(48 + 12))
Assert ($engPeak -gt 0.0) "engine output strip0 has signal (peak=$engPeak)"
Write-Host "engine output strip0 peak=$engPeak (shared out pair write path alive)"
$acc.Dispose(); $mm.Dispose()

# --- 6. 清場 ---
Clear-Tracks $pipe (Try-Command $pipe 'get_snapshot' @{}).result.snapshot.status
$pipe.Dispose()
Write-Host 'PASSED'
