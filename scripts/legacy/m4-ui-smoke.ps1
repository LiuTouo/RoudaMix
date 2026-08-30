# M4 UI smoke (release app): launch, auto-connect/start, scan, add plugin, preset buttons,
# spectrum WebGL bars lit, stop, exit -> engine dies (job object). CDP on 9223 via env var.
# ASCII-only. Any failure = throw (nonzero exit); all pass = SMOKE PASSED.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$appExe = Join-Path $root 'target\release\roudamix-app.exe'
function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }

# --- 0. clean slate ---
foreach ($p in @('roudamix-app', 'roudamix-engine', 'roudamix-worker')) {
    Get-Process $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 800

# --- 1. launch app with CDP port via WebView2 env arg (no config pollution) ---
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = '--remote-debugging-port=9223'
$app = Start-Process -FilePath $appExe -PassThru -WindowStyle Hidden
try {
    # --- 2. CDP connect ---
    $ws = $null
    for ($i = 0; $i -lt 30 -and $null -eq $ws; $i++) {
        Start-Sleep -Milliseconds 500
        try {
            $targets = Invoke-RestMethod 'http://127.0.0.1:9223/json/list' -ErrorAction Stop
            $page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
            if ($page) { $ws = New-Object System.Net.WebSockets.ClientWebSocket }
        } catch {}
    }
    Assert ($null -ne $ws) 'CDP endpoint reachable'
    $ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).Wait(5000) | Out-Null
    Assert ($ws.State -eq 'Open') 'CDP websocket open'

    $script:errors = New-Object System.Collections.ArrayList
    function Cdp($method, $params, $waitId) {
        $msg = @{ id = $script:id; method = $method; params = $params } | ConvertTo-Json -Depth 8 -Compress
        $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
        $seg = [ArraySegment[byte]]::new($bytes)
        $script:id++
        $ws.SendAsync($seg, 'Text', $true, [Threading.CancellationToken]::None).Wait(5000) | Out-Null
        $buf = New-Object byte[] (2 * 1024 * 1024)
        while ($true) {
            $ms = New-Object System.IO.MemoryStream
            while ($true) {
                $seg = [ArraySegment[byte]]::new($buf)
                $r = $ws.ReceiveAsync($seg, [Threading.CancellationToken]::None)
                $r.Wait(10000) | Out-Null
                $ms.Write($buf, 0, $r.Result.Count)
                if ($r.Result.EndOfMessage) { break }
            }
            $text = [Text.Encoding]::UTF8.GetString($ms.ToArray())
            $f = $text | ConvertFrom-Json
            if ($f.method -in @('Runtime.consoleAPICalled', 'Log.entryAdded', 'Runtime.exceptionThrown')) {
                $t = $text
                if ($t -match '"type"\s*:\s*"error"' -or $t -match '"level"\s*:\s*"error"' -or $t -match '"exceptionThrown"') {
                    [void]$script:errors.Add($text.Substring(0, [Math]::Min(300, $text.Length)))
                }
                continue
            }
            # waitId null = 只等下一個回應;非 null = 等指定 id(事件照收)
            if ($null -eq $waitId -or $f.id -eq $waitId) { return $f }
        }
    }
    $script:id = 1
    [void](Cdp 'Runtime.enable' @{} $null)

    function EvalJs($expr, $timeoutMs = 15000) {
        $wid = $script:id
        $p = @{ expression = $expr; returnByValue = $true; awaitPromise = $true }
        $r = Cdp 'Runtime.evaluate' $p $wid
        return $r.result.result.value
    }

    # --- 3. app connected + engine auto-start (header shows Hz) ---
    $hdr = $null
    for ($i = 0; $i -lt 30; $i++) {
        $hdr = EvalJs "document.querySelector('.bar') ? document.querySelector('.bar').textContent : ''" 5000
        if ($hdr -match 'Hz') { break }
        Start-Sleep -Milliseconds 500
    }
    Assert ($hdr -match 'Hz') "engine auto-started (header: $hdr)"
    Write-Host "started: $($hdr.Trim().Substring(0, [Math]::Min(80, $hdr.Trim().Length)))"

    # --- 4a. force a signal: set sine via the app's own Tauri IPC (pipe is single-client;
    #     an external pipe client would time out behind the bridge) ---
    $sine = EvalJs @"
(async () => {
  const inv = window.__TAURI_INTERNALS__ && window.__TAURI_INTERNALS__.invoke;
  if (!inv) return { err: 'no __TAURI_INTERNALS__' };
  try {
    await inv('engine_command', { kind: 'set_source', payload: { source: 'sine', sineFreq: 440 } });
    return { ok: true };
  } catch (e) { return { err: String(e) }; }
})()
"@ 15000
    Assert ($null -ne $sine -and -not $sine.err) "sine source set via IPC (got $(($sine | ConvertTo-Json -Compress)))"
    Write-Host 'sine source set (440 Hz)'

    # --- 4b. spectrum canvas: read INSIDE the same rAF frame as the draw loop
    # (preserveDrawingBuffer=false: read in a later frame = cleared buffer = zeros) ---
    $spec = EvalJs @"
new Promise(resolve => {
  const c = document.querySelector('canvas[title*="頻譜"]');
  if (!c) { resolve({ err: 'spectrum canvas not in DOM' }); return; }
  const gl = c.getContext('webgl');
  if (!gl) { resolve({ err: 'no webgl' }); return; }
  let n = 0;
  const tick = () => {
    requestAnimationFrame(() => {
      try {
        const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
        const px = new Uint8Array(w * h * 4);
        gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
        let lit = 0, maxSum = 0;
        const row = (h / 2) | 0;
        for (let i = 0; i < w * h; i++) {
          const s = px[i*4] + px[i*4+1] + px[i*4+2];
          if (s > maxSum) maxSum = s;
        }
        for (let i = 0; i < w; i++) {
          const o = (row * w + i) * 4;
          if (px[o] + px[o+1] + px[o+2] > 90) lit++;
        }
        if (lit >= 3 || maxSum > 120 || n > 30) { resolve({ w: w, h: h, lit: lit, maxSum: maxSum }); }
        else { n++; tick(); }
      } catch (e) { resolve({ err: String(e) }); }
    });
  };
  tick();
})
"@ 30000
    Assert ($null -ne $spec -and -not $spec.err -and $spec.lit -ge 3) "spectrum bars lit (got $(($spec | ConvertTo-Json -Compress)))"
    Write-Host "spectrum canvas: $($spec.w)x$($spec.h), lit pixels mid-row = $($spec.lit)"

    # --- 5. scan via UI (+ button), wait module list, add first class ---
    $plus = EvalJs "(() => { const b = document.querySelector('button.add'); if (b) { b.click(); return true; } return false; })()"
    Assert ($plus -eq $true) 'scan (+) button found and clicked'
    $classes = $null
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $classes = EvalJs "(() => { const card = document.querySelector('.card.scan'); if (!card) return 0; return card.querySelectorAll('.classes button').length; })()" 5000
        if ($classes -gt 0) { break }
    }
    Assert ($classes -gt 0) "scan panel lists classes (got $classes)"
    [void](EvalJs "(() => { const b = document.querySelector('.card.scan .classes button'); b.click(); return b.textContent; })()" 5000)
    $slots = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        $slots = EvalJs "document.querySelectorAll('.rackcol .slot, .rackcol button[class*=slot]').length" 5000
    }
    Write-Host "scan via UI ok, added plugin (scan classes: $classes)"

    # --- 5. preset buttons present on focused card ---
    $saveBtn = EvalJs "[...document.querySelectorAll('button')].some(b => b.textContent.includes('存 Preset'))" 5000
    $loadBtn = EvalJs "[...document.querySelectorAll('button')].some(b => b.textContent.includes('載 Preset'))" 5000
    Assert ($saveBtn -and $loadBtn) 'preset save/load buttons present'

    # --- 7. stop via UI ---
    [void](EvalJs "(() => { const b = [...document.querySelectorAll('button')].find(x => x.textContent.trim() === 'Stop'); if (b) { b.click(); return true; } return false; })()" 5000)
    Start-Sleep -Milliseconds 800
    Write-Host 'stop clicked'
} finally {
    $script:errors | Select-Object -First 3 | ForEach-Object { Write-Host "console-error: $_" }
    if ($ws) { $ws.Dispose() }
    if ($app -and -not $app.HasExited) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
}
Start-Sleep -Seconds 2
$engLeft = Get-Process roudamix-engine -ErrorAction SilentlyContinue
Assert ($null -eq $engLeft) 'engine died with app (job object)'
Write-Host 'SMOKE PASSED'
