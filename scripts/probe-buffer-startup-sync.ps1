# 迴歸探針:剛開程式 Buffer 顯示 ≠ 實際(啟動競態把 bufSize 留成 stale 值)。
# 根因:session 恢復覆寫 bufSize 後其 start 撞 busy 被吞,engine 實跑 lastWorking 值;
# status 事件權威對齊補回(App.svelte status handler 同步 bufferSize)。
# 釘死時序:autoStart 的 start 慢 300ms(模型真 ASIO 開啟時間),load_session 150ms 回
# (session buffer=512 < lastWorking=1024)。
# 紅 = dropdown(512) ≠ badge(1024)。綠 = 兩者 = 1024,且手動改 buffer 仍可校正。
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$protocolContractJson = Get-Content -Raw (Join-Path $root 'contracts\command_contract.json')
$protocolStubFramework = Get-Content -Raw (Join-Path $root 'scripts\protocol-stub-framework.js')
$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }

Push-Location (Join-Path $root 'ui')
npm run build 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'npm run build failed' }
Pop-Location
$preview = Start-Process -FilePath 'cmd' -ArgumentList '/c', "npx vite preview --port 4173 --strictPort > `"$env:TEMP\rmx-bufsync-preview.log`" 2>&1" -WorkingDirectory (Join-Path $root 'ui') -PassThru -WindowStyle Hidden
$up = $false
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 500
    try { $null = Invoke-RestMethod 'http://localhost:4173/' -ErrorAction Stop; $up = $true; break } catch {}
}
if (-not $up) { throw 'vite preview not up on 4173' }

$dataDir = Join-Path $env:TEMP 'rmx-bufsync-edge'
if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue }
$edgeProc = Start-Process -FilePath $edge -ArgumentList @(
    '--headless=new', '--remote-debugging-port=9223', "--user-data-dir=$dataDir",
    '--no-first-run', '--disable-gpu', 'http://localhost:4173/'
) -PassThru -WindowStyle Hidden
$ws = $null
try {
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
        $msg = @{ id = $script:mid; method = $method; params = $params } | ConvertTo-Json -Depth 8 -Compress
        $script:mid++
        $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
        $ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true, [Threading.CancellationToken]::None).Wait(5000) | Out-Null
        $buf = New-Object byte[] (4 * 1024 * 1024)
        while ($true) {
            $ms = New-Object System.IO.MemoryStream
            while ($true) {
                $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf), [Threading.CancellationToken]::None)
                $r.Wait(10000) | Out-Null
                $ms.Write($buf, 0, $r.Result.Count)
                if ($r.Result.EndOfMessage) { break }
            }
            $text = [Text.Encoding]::UTF8.GetString($ms.ToArray())
            $f = $text | ConvertFrom-Json
            if ($f.method -in @('Runtime.consoleAPICalled', 'Log.entryAdded', 'Runtime.exceptionThrown')) {
                if ($text -match '"type"\s*:\s*"error"' -or $text -match '"level"\s*:\s*"error"' -or $text -match '"exceptionThrown"') {
                    [void]$script:errors.Add($text.Substring(0, [Math]::Min(300, $text.Length)))
                }
                continue
            }
            if ($null -eq $waitId -or $f.id -eq $waitId) { return $f }
        }
    }
    $script:mid = 0
    [void](Cdp 'Page.enable' @{} $null)

    # --- stub:settings 開 session 恢復 + lastWorking=1024;engine start 慢 300ms;load_session 150ms 回 buffer=512 ---
    $stub = @'
const __protocolContract = __ROUDAMIX_COMMAND_CONTRACT__;
const __validateEngineCommand = createProtocolStubValidator(__protocolContract);
window.__engine = {
  devices: [{ deviceKey: 'dev1', name: 'Test ASIO', maxIn: 4, maxOut: 4, sampleRates: [48000],
    currentSampleRate: 48000, minBufferSize: 64, maxBufferSize: 2048, preferredBufferSize: 256,
    bufferSizes: [256, 512, 1024], inputNames: ['Analog 1', 'Analog 2', 'Analog 3', 'Analog 4'],
    outputNames: ['Out 1', 'Out 2', 'Out 3', 'Out 4'] }],
  listeners: {}, running: false, runningBuf: 0, startLog: [],
  status: function () {
    return { running: this.running, deviceKey: this.running ? 'dev1' : null, sampleRate: 48000,
      bufferSize: this.runningBuf, inputLatency: 10, outputLatency: 12, xruns: 0, trackCount: 0,
      pluginFails: 0, latencyGeneration: 1, pluginDelay: { monitorSamples: 0, streamSamples: 0 },
      tracks: [], revision: 0, error: null };
  },
  emit: function () {
    const wrapped = { event: 'engine-event', payload: { kind: 'status', payload: this.status() } };
    for (const fn of this.listeners['engine-event'] || []) { try { fn(wrapped); } catch (e) {} }
  },
  cmd: function (kind, p) {
    __validateEngineCommand(kind, p);
    switch (kind) {
      case 'get_snapshot': return { snapshot: { epoch: 1, engineVersion: 'stub',
        capabilities: ['pluginLatencyPdcV1'], status: this.status(), tracks: [], lastScan: null } };
      case 'list_devices': return { devices: this.devices };
      case 'load_session': {
        // session:dev1 + buffer 512(≠ lastWorking 1024);延遲回 = 重現真實 IPC 時序
        return new Promise(res => setTimeout(() => res({ revision: 1, deviceKey: 'dev1',
          sampleRate: 48000, bufferSize: 512, missing: [] }), 150));
      }
      case 'start': {
        return new Promise(res => setTimeout(() => {
          this.running = true; this.runningBuf = p.bufferSize != null ? p.bufferSize : 256;
          this.startLog.push(this.runningBuf);
          this.emit();
          res(this.status());
        }, 300));
      }
      case 'stop': {
        this.running = false; this.runningBuf = 0; this.emit();
        return this.status();
      }
      default: return {};
    }
  }
};
window.__TAURI_INTERNALS__ = {
  invoke: (cmd, args) => {
    if (cmd === 'engine_command') return Promise.resolve(window.__engine.cmd(args.kind, args.payload || {}));
    if (cmd === 'connect_status') return Promise.resolve({ connected: true, epoch: 1, engineVersion: 'stub' });
    if (cmd === 'get_settings') return Promise.resolve({ settings: { startupMode: 'last',
      sessionDir: null, startupFile: null, lastSessionPath: 'C:/Temp/bufsync.rmsession',
      lastWorkingDevice: 'dev1', lastWorkingBuffer: 1024, closeBehavior: 'exit',
      startMinimizedOnAutostart: false }, warnings: [] });
    if (cmd === 'set_settings') return Promise.resolve({ settings: args.patch || {}, warnings: [] });
    if (cmd === 'plugin:event|listen') {
      const ev = args.event;
      (window.__engine.listeners[ev] = window.__engine.listeners[ev] || []).push(args.handler);
      if (ev === 'engine-connection') {
        setTimeout(() => { try { args.handler({ event: ev, payload: { connected: true, phase: 'ready', epoch: 1, engineVersion: 'stub' } }); } catch (e) {} }, 0);
      }
      if (ev === 'engine-snapshot') {
        setTimeout(() => { try { args.handler({ event: ev, payload: window.__engine.cmd('get_snapshot', {}).snapshot }); } catch (e) {} }, 0);
      }
      return Promise.resolve(Math.random());
    }
    return Promise.resolve({});
  },
  transformCallback: (cb) => cb
};
console.log('__engine_status__ stub installed');
'@
    $stub = $protocolStubFramework + "`n" + $stub.Replace('__ROUDAMIX_COMMAND_CONTRACT__', $protocolContractJson)
    [void](Cdp 'Page.addScriptToEvaluateOnNewDocument' @{ source = $stub } $null)
    [void](Cdp 'Runtime.enable' @{} $null)
    [void](Cdp 'Page.reload' @{ ignoreCache = $false } $null)

    function EvalJs($expr, $timeoutMs = 15000) {
        $wid = $script:mid
        $p = @{ expression = $expr; returnByValue = $true; awaitPromise = $true }
        $r = Cdp 'Runtime.evaluate' $p $wid
        return $r.result.result.value
    }

    # --- 等啟動完成:engine 跑起來(autoStart 贏家,startLog=[1024])---
    $st = $null
    for ($i = 0; $i -lt 25; $i++) {
        Start-Sleep -Milliseconds 400
        $st = EvalJs "(() => { const sel = document.querySelector('#setbuf'); return { running: !!document.querySelector('.dot.ok'), bufSel: sel ? sel.value : null, badge: (document.querySelector('header .mono') ? [...document.querySelectorAll('header .mono')].map(e => e.textContent).join(' | ') : '') } })()" 5000
        if ($st.running) { break }
    }
    Assert ($st.running) "engine running after startup (last: $($st | ConvertTo-Json -Compress))"
    Write-Host "startup: running, startLog=$(EvalJs 'JSON.stringify(window.__engine.startLog)' 5000)"

    $badgeBuf = if ($st.badge -match 'buf (\d+)') { $Matches[1] } else { $null }
    Write-Host "state: dropdown=$($st.bufSel) badge=$badgeBuf  (expect 兩者一致 = engine 實跑值 1024)"
    Assert ($st.bufSel -eq '1024') "dropdown 跟上 engine 實際 buffer 1024(got $($st.bufSel))"
    Assert ($badgeBuf -eq '1024') "badge = 1024(got $badgeBuf)"

    # --- 手動更改仍可校正(使用者既有路徑不能壞):改 512 → restart → 兩者 = 512 ---
    [void](EvalJs @"
(() => { const s = document.querySelector('#setbuf'); s.value = '512';
  s.dispatchEvent(new Event('change', { bubbles: true })); return true; })()
"@ 5000)
    $ok512 = $false
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 400
        $v = EvalJs "(() => { const sel = document.querySelector('#setbuf'); const badge = [...document.querySelectorAll('header .mono')].map(e => e.textContent).join(' '); const m = badge.match(/buf (\d+)/); return { sel: sel ? sel.value : null, badge: m ? m[1] : null } })()" 5000
        if ($v.sel -eq '512' -and $v.badge -eq '512') { $ok512 = $true; break }
    }
    Assert ($ok512) "手動改 512 後 dropdown+badge 對齊(got $($v | ConvertTo-Json -Compress))"
    Write-Host "manual: 改 512 → dropdown=badge=512(校正路徑完好)"

    $script:errors | Select-Object -First 3 | ForEach-Object { Write-Host "console-error: $_" }
    Assert ($script:errors.Count -eq 0) "no console errors (got $($script:errors.Count))"
} finally {
    if ($ws) { $ws.Dispose() }
    if ($edgeProc -and -not $edgeProc.HasExited) { Stop-Process -Id $edgeProc.Id -Force -ErrorAction SilentlyContinue }
    Get-Process msedge -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 'rmx-bufsync-edge' } | Stop-Process -Force -ErrorAction SilentlyContinue
    if ($preview -and -not $preview.HasExited) { Stop-Process -Id $preview.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
}
Write-Host 'PROBE PASSED'
