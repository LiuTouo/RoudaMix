# M5a UI smoke:build dist → vite preview → headless Edge (CDP 9223) →
# __TAURI_INTERNALS__ stub(記憶體 engine 模擬器)驗證儀表板全部 UI 接線:
# 三欄/新增鈕、預設輸出軌、來源選擇、目的地多選、VST 展開/掃描/bypass、
# 色盤、推桿、meter canvas、console 無 error。
# 背景註記:M5a 起新版 WebView2 runtime 下 --remote-debugging-port(env/conf)
# 皆失效或擋掉 tauri.localhost 導覽,故改用 Edge headless + stub(不驗原生橋;
# 原生橋由 m5a-engine-tracks.ps1 對真 engine 全套驗證)。
# Any failure = throw (nonzero exit); all pass = SMOKE PASSED.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
function Assert($cond, $msg) { if (-not $cond) { throw "ASSERT FAIL: $msg" } }

# --- 0. build dist + vite preview ---
Push-Location (Join-Path $root 'ui')
npm run build 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'npm run build failed' }
Pop-Location
$previewLog = Join-Path $env:TEMP 'rmx-m5a-preview.log'
$preview = Start-Process -FilePath 'cmd' -ArgumentList '/c', "npx vite preview --port 4173 --strictPort > `"$previewLog`" 2>&1" -WorkingDirectory (Join-Path $root 'ui') -PassThru -WindowStyle Hidden
$up = $false
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 500
    try {
        $null = Invoke-RestMethod 'http://localhost:4173/' -ErrorAction Stop
        $up = $true
        break
    } catch {}
}
if (-not $up) {
    if (Test-Path $previewLog) { Get-Content $previewLog | Select-Object -First 5 | ForEach-Object { Write-Host "preview: $_" } }
    throw 'vite preview not up on 4173'
}

# --- 1. headless Edge with CDP ---
$dataDir = Join-Path $env:TEMP 'rmx-m5a-edge'
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
    $script:pending = New-Object System.Collections.ArrayList
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
                $t = $text
                if ($t -match '"type"\s*:\s*"error"' -or $t -match '"level"\s*:\s*"error"' -or $t -match '"exceptionThrown"') {
                    [void]$script:errors.Add($text.Substring(0, [Math]::Min(300, $text.Length)))
                }
                if ($f.method -eq 'Runtime.consoleAPICalled' -and $text -match '__engine_status__') {
                    [void]$script:pending.Add($text)
                }
                continue
            }
            if ($null -eq $waitId -or $f.id -eq $waitId) { return $f }
        }
    }
    $script:mid = 0
    [void](Cdp 'Page.enable' @{} $null)

    # --- 2. stub __TAURI_INTERNALS__ before app code runs(記憶體 engine 模擬器)---
    $stub = @'
window.__engine = {
  nextTrackId: 1, nextInst: 1,
  tracks: [], commands: [],
  devices: [{ deviceKey: 'dev1', name: 'Test ASIO', maxIn: 4, maxOut: 4, sampleRates: [48000],
    currentSampleRate: 48000, minBufferSize: 64, maxBufferSize: 2048, preferredBufferSize: 512,
    bufferSizes: [64, 128, 256, 512, 1024], inputNames: ['Analog 1', 'Analog 2', 'Analog 3', 'Analog 4'],
    outputNames: ['Out 1', 'Out 2', 'Out 3', 'Out 4'] }],
  listeners: {},
  status: function () {
    return { running: true, deviceKey: 'dev1', sampleRate: 48000, bufferSize: 512,
      inputLatency: 10, outputLatency: 12, xruns: 0, trackCount: this.tracks.length,
      pluginFails: 0, tracks: JSON.parse(JSON.stringify(this.tracks)), error: null };
  },
  emit: function () {
    // 對齊真 bridge:engine-event 的 payload = { kind, payload }
    const st = this.status();
    const wrapped = { event: 'engine-event', payload: { kind: 'status', payload: st } };
    for (const fn of this.listeners['engine-event'] || []) {
      try { fn(wrapped); } catch (e) {}
    }
  },
  cmd: function (kind, p) {
    this.commands.push(kind);
    const R = (v) => v;
    switch (kind) {
      case 'engine_command': return 'NEVER';
      case 'get_snapshot': return { snapshot: { epoch: 1, engineVersion: 'stub', status: this.status(), tracks: this.tracks } };
      case 'list_devices': return { devices: this.devices };
      case 'track_add': {
        const t = { trackId: this.nextTrackId++, kind: p.kind, name: p.name || p.kind + ' ' + (this.nextTrackId - 1),
          color: p.color || 0x4da3ff, source: null, dests: [], output: null, gain: 1, mute: false, plugins: [] };
        this.tracks.push(t); this.emit();
        return { trackId: t.trackId, tracks: this.tracks };
      }
      case 'track_remove': { this.tracks = this.tracks.filter(t => t.trackId !== p.trackId); this.emit(); return { tracks: this.tracks }; }
      case 'track_set': {
        const t = this.tracks.find(t => t.trackId === p.trackId);
        if (p.name !== undefined) t.name = p.name;
        if (p.color !== undefined) t.color = p.color;
        if (p.gain !== undefined) t.gain = p.gain;
        if (p.mute !== undefined) t.mute = p.mute;
        this.emit(); return { tracks: this.tracks };
      }
      case 'track_set_source': {
        const t = this.tracks.find(t => t.trackId === p.trackId);
        t.source = p.source === null ? null : JSON.parse(JSON.stringify(p.source));
        this.emit(); return { tracks: this.tracks };
      }
      case 'track_set_dests': {
        const t = this.tracks.find(t => t.trackId === p.trackId);
        t.dests = p.dests.slice(); this.emit(); return { tracks: this.tracks };
      }
      case 'track_set_output': {
        const t = this.tracks.find(t => t.trackId === p.trackId);
        t.output = p.output === null ? null : JSON.parse(JSON.stringify(p.output));
        this.emit(); return { tracks: this.tracks };
      }
      case 'scan_plugins': {
        return { plugins: [{ path: 'C:/VST3/LoudMax.vst3', classes: [{ uid: 'uid1', name: 'LoudMax', vendor: 'v', version: '1', subcategories: 'Fx' }] }] };
      }
      case 'add_plugin': {
        const t = this.tracks.find(t => t.trackId === p.trackId);
        t.plugins.push({ instanceId: this.nextInst++, name: 'LoudMax', pluginPath: p.path, classId: p.classId, bypassed: false, params: [] });
        this.emit(); return { instanceId: this.nextInst - 1, trackId: p.trackId, tracks: this.tracks };
      }
      case 'set_bypass': {
        for (const t of this.tracks) for (const pl of t.plugins) if (pl.instanceId === p.instanceId) pl.bypassed = p.bypassed;
        this.emit(); return { tracks: this.tracks };
      }
      case 'start': case 'stop': return this.status();
      default: return {};
    }
  }
};
window.__TAURI_INTERNALS__ = {
  invoke: (cmd, args) => {
    if (cmd === 'engine_command') {
      const r = window.__engine.cmd(args.kind, args.payload || {});
      return Promise.resolve(r);
    }
    if (cmd === 'connect_status') return Promise.resolve({ connected: true, epoch: 1, engineVersion: 'stub' });
    if (cmd === 'plugin:event|listen') {
      const ev = args.event; const id = Math.random();
      (window.__engine.listeners[ev] = window.__engine.listeners[ev] || []).push(args.handler);
      // 對齊真 bridge:連線建立時推一次 snapshot
      if (ev === 'engine-snapshot') {
        setTimeout(() => {
          try { args.handler({ event: ev, payload: window.__engine.cmd('get_snapshot', {}).snapshot }); } catch (e) {}
        }, 0);
      }
      return Promise.resolve(id);
    }
    return Promise.resolve({});
  },
  transformCallback: (cb) => cb
};
console.log('__engine_status__ stub installed');
'@
    [void](Cdp 'Page.addScriptToEvaluateOnNewDocument' @{ source = $stub } $null)
    [void](Cdp 'Runtime.enable' @{} $null)
    [void](Cdp 'Page.reload' @{ ignoreCache = $false } $null)
    Start-Sleep -Seconds 2

    function EvalJs($expr, $timeoutMs = 15000) {
        $wid = $script:mid
        $p = @{ expression = $expr; returnByValue = $true; awaitPromise = $true }
        $r = Cdp 'Runtime.evaluate' $p $wid
        return $r.result.result.value
    }

    # --- 3. 儀表板骨架:左右欄 + 新增鈕(App disabled)---
    $layout = EvalJs "(() => ({ cols: document.querySelectorAll('.colhead').length, leftBtns: document.querySelector('.colhead') ? document.querySelector('.colhead').querySelectorAll('button').length : 0, leftDisabled: document.querySelector('.colhead') ? document.querySelector('.colhead').querySelectorAll('button:disabled').length : 0, outColBtns: document.querySelectorAll('.colhead')[1] ? document.querySelectorAll('.colhead')[1].querySelectorAll('button').length : 0 }))()" 5000
    Assert ($layout.cols -eq 2) "two track columns (got $($layout.cols))"
    Assert ($layout.leftBtns -eq 3) "3 input add buttons (got $($layout.leftBtns))"
    Assert ($layout.leftDisabled -eq 1) 'App add button disabled (M5b)'
    Assert ($layout.outColBtns -eq 1) '1 output add button'
    Write-Host "layout: cols=$($layout.cols) leftBtns=$($layout.leftBtns)(disabled $($layout.leftDisabled)) outBtns=$($layout.outColBtns)"

    # --- 4. 預設輸出軌(監聽/串流)自動建立 ---
    $strips = 0
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 400
        $strips = EvalJs "document.querySelectorAll('.strip').length" 5000
        if ($strips -eq 2) { break }
    }
    Assert ($strips -eq 2) "2 default output strips (got $strips)"
    $names = EvalJs "[...document.querySelectorAll('.strip .name')].map(e => e.textContent)" 5000
    Assert ($names.length -eq 2) 'two output track names'
    Write-Host "defaults: $strips output tracks ($($names -join ', '))"

    # --- 5. + Audio / + FX 新增 ---
    [void](EvalJs "(() => { const b = [...document.querySelector('.colhead').querySelectorAll('button')].find(x => !x.disabled); b.click(); return b.textContent; })()" 5000)
    Start-Sleep -Milliseconds 500
    [void](EvalJs "(() => { const b = [...document.querySelectorAll('.colhead button')].find(x => x.textContent.includes('FX')); b.click(); return true; })()" 5000)
    $counts = 0
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 400
        $counts = EvalJs "(() => ({ all: document.querySelectorAll('.strip').length, out: document.querySelectorAll('.strip.out').length }))()" 5000
        if ($counts.all -eq 4) { break }
    }
    Assert ($counts.all -eq 4) "4 strips after adding audio+fx (got $($counts.all))"
    Assert ($counts.out -eq 2) '2 output strips (new ones on left)'
    Write-Host "add: audio + fx via UI (total $($counts.all))"

    # --- 6. audio 軌:來源選 asioIn pair ---
    [void](EvalJs @"
(() => {
  const s = document.querySelector('.strip select');
  if (!s || s.options.length < 3) return false;
  s.value = s.options[2].value; // 3/4(第二組 pair,channel 2)
  s.dispatchEvent(new Event('change', { bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    Write-Host "engine tracks: $(EvalJs 'JSON.stringify(window.__engine.tracks.map(t => ({id: t.trackId, kind: t.kind, src: t.source})))' 5000)"
    $src = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').source" 5000
    Assert ($null -ne $src -and $src.type -eq 'asioIn') "audio source wired (got $($src | ConvertTo-Json -Compress))"
    Assert ($src.channel -eq 2) "asioIn channel = 2 (got $($src.channel))"

    # --- 7. 目的地多選:分兩次勾(每次點擊後 Svelte 重渲會換節點)→ engine dests = 2 ---
    [void](EvalJs @"
(() => {
  const d = document.querySelector('.strip details.dests');
  d.open = true;
  const boxes = d.querySelectorAll('input[type=checkbox]');
  boxes[0].click();
  return boxes.length;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    [void](EvalJs @"
(() => {
  const boxes = document.querySelectorAll('.strip details.dests input[type=checkbox]');
  const unchecked = [...boxes].find(b => !b.checked);
  if (unchecked) { unchecked.click(); return true; }
  return false;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    $dests = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').dests" 5000
    Assert ($dests.length -eq 2) "dests multi-select wired (got $($dests.length))"
    Write-Host "routing: audio dests = $($dests.length) tracks"

    # --- 8. VST:展開 → 掃描 → 加第一個 → 電源 bypass ---
    [void](EvalJs "(() => { const d = document.querySelector('.strip details.vst'); d.open = true; return true; })()" 5000)
    [void](EvalJs "(() => { const b = document.querySelector('.strip details.vst button.add'); b.click(); return true; })()" 5000)
    $classes = 0
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 300
        $classes = EvalJs "document.querySelectorAll('.strip details.vst .classes button').length" 5000
        if ($classes -gt 0) { break }
    }
    Assert ($classes -gt 0) "scan list rendered in vst details (got $classes)"
    [void](EvalJs "(() => { document.querySelector('.strip details.vst .classes button').click(); return true; })()" 5000)
    Start-Sleep -Milliseconds 600
    $plugs = EvalJs "(() => ({ n: window.__engine.tracks.find(t => t.kind === 'audio').plugins.length, rows: document.querySelectorAll('.strip .plug').length, power: document.querySelectorAll('.strip .plug .power').length }))()" 5000
    Assert ($plugs.n -eq 1) "plugin added via stub engine (got $($plugs.n))"
    Assert ($plugs.power -ge 1) 'power button rendered'
    [void](EvalJs "(() => { document.querySelector('.strip .plug .power').click(); return true; })()" 5000)
    Start-Sleep -Milliseconds 500
    $byp = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').plugins[0].bypassed" 5000
    Assert ($byp -eq $true) "power button = bypass toggle (got $byp)"
    Write-Host "vst: scan/add/bypass all wired (power rows: $($plugs.power))"

    # --- 9. 色盤 + 顏色條 + meter canvas + 推桿 ---
    $per = EvalJs "(() => ({ strips: document.querySelectorAll('.strip').length, colors: document.querySelectorAll('.strip input[type=color]').length, bars: document.querySelectorAll('.strip .colorbar').length, meters: document.querySelectorAll('.strip canvas').length, ranges: document.querySelectorAll('.strip input[type=range]').length }))()" 5000
    Assert ($per.colors -eq $per.strips) "color picker per strip (got $($per.colors))"
    Assert ($per.bars -eq $per.strips) "color bar per strip (got $($per.bars))"
    Assert ($per.meters -eq $per.strips) "meter canvas per strip (got $($per.meters))"
    Assert ($per.ranges -eq $per.strips) "gain slider per strip (got $($per.ranges))"
    [void](EvalJs @"
(() => {
  const c = document.querySelector('.strip input[type=color]');
  c.value = '#ff8800'; c.dispatchEvent(new Event('change', { bubbles: true }));
  const r = document.querySelector('.strip input[type=range]');
  r.value = '0.5'; r.dispatchEvent(new Event('change', { bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    $st = EvalJs "(() => { const t = window.__engine.tracks.find(t => t.kind === 'audio'); return { color: t.color, gain: t.gain }; })()" 5000
    Assert ($st.color -eq 0xff8800) "color wired (got $($st.color))"
    Assert ([Math]::Abs($st.gain - 0.5) -lt 0.001) "gain wired (got $($st.gain))"
    Write-Host "per-strip: color=0x$('{0:x}' -f $st.color) gain=$($st.gain) - all wired"

    # --- 10. console errors ---
    $script:errors | Select-Object -First 3 | ForEach-Object { Write-Host "console-error: $_" }
    Assert ($script:errors.Count -eq 0) "no console errors (got $($script:errors.Count))"
} finally {
    if ($ws) { $ws.Dispose() }
    if ($edgeProc -and -not $edgeProc.HasExited) { Stop-Process -Id $edgeProc.Id -Force -ErrorAction SilentlyContinue }
    Get-Process msedge -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 'rmx-m5a-edge' } | Stop-Process -Force -ErrorAction SilentlyContinue
    if ($preview -and -not $preview.HasExited) { Stop-Process -Id $preview.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
}
Write-Host 'SMOKE PASSED'
