# M5a UI smoke:build dist → vite preview → headless Edge (CDP 9223) →
# __TAURI_INTERNALS__ stub(記憶體 engine 模擬器)驗證儀表板全部 UI 接線:
# 水平帶(輸入/輸出群組)/新增鈕、預設輸出軌、來源選擇、目的地多選、VST 展開/掃描/bypass、
# 色盤、垂直推桿(ctrl+click 重置)、meter canvas(垂直+刻度)、雙擊改名、拖曳排序、console 無 error。
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
      case 'track_move': {
        const i = this.tracks.findIndex(t => t.trackId === p.trackId);
        if (i < 0) return { tracks: this.tracks };
        const [t] = this.tracks.splice(i, 1);
        this.tracks.splice(Math.min(p.newIndex, this.tracks.length), 0, t);
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
      case 'move_plugin': {
        for (const t of this.tracks) {
          const i = t.plugins.findIndex(pl => pl.instanceId === p.instanceId);
          if (i >= 0) {
            const [pl] = t.plugins.splice(i, 1);
            t.plugins.splice(Math.min(p.newIndex, t.plugins.length), 0, pl);
            break;
          }
        }
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

    # --- 3. 儀表板骨架:左右欄 + 新增鈕(M5b 起 App enabled)---
    $layout = EvalJs "(() => ({ cols: document.querySelectorAll('.colhead').length, leftBtns: document.querySelector('.colhead') ? document.querySelector('.colhead').querySelectorAll('button').length : 0, leftDisabled: document.querySelector('.colhead') ? document.querySelector('.colhead').querySelectorAll('button:disabled').length : 0, outColBtns: document.querySelectorAll('.colhead')[1] ? document.querySelectorAll('.colhead')[1].querySelectorAll('button').length : 0 }))()" 5000
    Assert ($layout.cols -eq 2) "two track columns (got $($layout.cols))"
    Assert ($layout.leftBtns -eq 3) "3 input add buttons (got $($layout.leftBtns))"
    Assert ($layout.leftDisabled -eq 0) 'no add buttons disabled (App live since M5b)'
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

    # --- 7. 目的地多選:置中 dialog 勾選(每次點擊後 Svelte 重渲會換節點)→ engine dests = 2 ---
    [void](EvalJs "(() => { document.querySelector('.strip .destsbtn').click(); return true; })()" 5000)
    Start-Sleep -Milliseconds 400
    $dopen = EvalJs "(() => { const d = document.querySelector('.destlistdlg'); return !!(d && d.open && d.querySelector('input[type=checkbox]')); })()" 5000
    Assert ($dopen) 'dests dialog open with checkboxes'
    [void](EvalJs "(() => { document.querySelector('.destlistdlg input[type=checkbox]').click(); return true; })()" 5000)
    Start-Sleep -Milliseconds 600
    [void](EvalJs @"
(() => {
  const boxes = document.querySelectorAll('.destlistdlg input[type=checkbox]');
  const unchecked = [...boxes].find(b => !b.checked);
  if (unchecked) { unchecked.click(); return true; }
  return false;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    $dests = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').dests" 5000
    Assert ($dests.length -eq 2) "dests multi-select wired (got $($dests.length))"
    [void](EvalJs "document.querySelector('.destlistdlg').close()" 5000)
    Start-Sleep -Milliseconds 300
    Write-Host "routing: audio dests = $($dests.length) tracks (dialog picker)"

    # --- 8. VST:掃描 → 置中 dialog 列表 → 加第一個 → 電源 bypass ---
    [void](EvalJs "(() => { const d = document.querySelector('.strip details.vst'); d.open = true; return true; })()" 5000)
    [void](EvalJs "(() => { const b = document.querySelector('.strip details.vst button.add'); b.click(); return true; })()" 5000)
    $dlg = $false
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 400
        $dlg = EvalJs "(() => { const d = document.querySelector('.scanlistdlg'); return !!(d && d.open && d.querySelector('.classes button')); })()" 5000
        if ($dlg) { break }
    }
    Assert ($dlg) "scan dialog open with class buttons"
    $ctr = EvalJs "(() => { const r = document.querySelector('.scanlistdlg').getBoundingClientRect(); return { cx: Math.abs(r.left + r.width / 2 - innerWidth / 2) < 2, cy: Math.abs(r.top + r.height / 2 - innerHeight / 2) < 2 }; })()" 5000
    Assert ($ctr.cx -and $ctr.cy) "scan dialog centered in window (cx=$($ctr.cx) cy=$($ctr.cy))"
    [void](EvalJs "(() => { document.querySelector('.scanlistdlg .classes button').click(); return true; })()" 5000)
    Start-Sleep -Milliseconds 600
    $plugs = EvalJs "(() => ({ n: window.__engine.tracks.find(t => t.kind === 'audio').plugins.length, rows: document.querySelectorAll('.strip .plug').length, power: document.querySelectorAll('.strip .plug .power').length, closed: (() => { const d = document.querySelector('.scanlistdlg'); return d && !d.open; })() }))()" 5000
    Assert ($plugs.n -eq 1) "plugin added via stub engine (got $($plugs.n))"
    Assert ($plugs.closed) "scan dialog closes after add"
    Assert ($plugs.power -ge 1) 'power button rendered'
    [void](EvalJs "(() => { document.querySelector('.strip .plug .power').click(); return true; })()" 5000)
    Start-Sleep -Milliseconds 500
    $byp = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').plugins[0].bypassed" 5000
    Assert ($byp -eq $true) "power button = bypass toggle (got $byp)"
    # 電源 icon:img(黑/綠 SVG)已渲染
    $picon = EvalJs "document.querySelectorAll('.strip .plug .power img').length" 5000
    Assert ($picon -ge 1) "power icon img rendered (got $picon)"
    Write-Host "vst: scan dialog centered + add/bypass all wired (power rows: $($plugs.power), icons: $picon)"

    # --- 8b. 第二顆插件 → 拖曳換序(move_plugin;▲▼ 已移除)---
    # 第一次 add 成功後 dialog 自動關 → 再點掃描重開
    [void](EvalJs "(() => { document.querySelector('.strip details.vst button.add').click(); return true; })()" 5000)
    $dlg2 = $false
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 400
        $dlg2 = EvalJs "(() => { const d = document.querySelector('.scanlistdlg'); return !!(d && d.open && d.querySelector('.classes button')); })()" 5000
        if ($dlg2) { break }
    }
    Assert ($dlg2) "scan dialog reopened"
    [void](EvalJs "(() => { document.querySelector('.scanlistdlg .classes button').click(); return true; })()" 5000)
    Start-Sleep -Milliseconds 600
    $pn = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').plugins.length" 5000
    Assert ($pn -eq 2) "second plugin added (got $pn)"
    [void](EvalJs @"
(() => {
  const rows = document.querySelectorAll('.strip .plug');
  const dt = new DataTransfer();
  rows[1].dispatchEvent(new DragEvent('dragstart', { bubbles: true, dataTransfer: dt }));
  const r0 = rows[0].getBoundingClientRect();
  rows[0].dispatchEvent(new DragEvent('dragover', { bubbles: true, clientY: r0.top + 2, dataTransfer: dt }));
  rows[0].dispatchEvent(new DragEvent('drop', { bubbles: true, clientY: r0.top + 2, dataTransfer: dt }));
  rows[1].dispatchEvent(new DragEvent('dragend', { bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    $chain = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').plugins.map(p => p.instanceId)" 5000
    Assert ($chain[0] -eq 2) "plugin drag reorder (chain: $($chain -join ','))"
    $pbtns = EvalJs "document.querySelectorAll('.strip .plug')[0].querySelectorAll('button').length" 5000
    Assert ($pbtns -eq 2) "plug row has only power+x buttons (got $pbtns)"
    Write-Host "vst: drag reorder chain=[$($chain -join ',')]  row buttons=$pbtns (power+x)"

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

    # --- 10. 垂直 fader(writing-mode)+ ctrl+click 重置 1.0 + head 只剩 × ---
    $vert = EvalJs @"
(() => {
  const r = document.querySelector('.strip input[type=range]');
  const cs = getComputedStyle(r);
  const headBtns = document.querySelector('.strip .head').querySelectorAll('button').length;
  return { wm: cs.writingMode, h: r.offsetHeight, w: r.offsetWidth, headBtns,
    draggable: [...document.querySelectorAll('.strip')].every(s => s.draggable === true) };
})()
"@ 5000
    Assert ($vert.wm -eq 'vertical-lr') "range vertical writing-mode (got $($vert.wm))"
    Assert ($vert.h -gt $vert.w) "range taller than wide (got $($vert.h)x$($vert.w))"
    Assert ($vert.headBtns -eq 1) "head has only x button (got $($vert.headBtns))"
    Assert ($vert.draggable -eq $true) 'strip draggable for reorder'
    # ctrl+click = 重置 1.0(此時 gain 已被上段設 0.5)
    [void](EvalJs "(() => { const r = document.querySelector('.strip input[type=range]'); r.dispatchEvent(new MouseEvent('click', { ctrlKey: true, bubbles: true })); return true; })()" 5000)
    Start-Sleep -Milliseconds 600
    $g1 = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').gain" 5000
    Assert ([Math]::Abs($g1 - 1) -lt 0.001) "ctrl+click resets gain to 1 (got $g1)"
    # 滾輪微調:deltaY > 0 = -0.02
    [void](EvalJs "(() => { const r = document.querySelector('.strip input[type=range]'); r.dispatchEvent(new WheelEvent('wheel', { deltaY: 120, bubbles: true, cancelable: true })); return true; })()" 5000)
    Start-Sleep -Milliseconds 400
    $gw = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').gain" 5000
    Assert ([Math]::Abs($gw - 0.98) -lt 0.001) "wheel adjusts gain -0.02 (got $gw)"
    # 字體:IBM Plex Sans TC 已載入(lazy load → 先 load 再 check;無引號避開 PS 轉義)
    $fontOk = $false
    for ($i = 0; $i -lt 10 -and -not $fontOk; $i++) {
        $fontOk = EvalJs "document.fonts.load('13px IBM Plex Sans TC').then(f => f.length > 0 && document.fonts.check('13px IBM Plex Sans TC'))" 5000
        if (-not $fontOk) { Start-Sleep -Milliseconds 400 }
    }
    Assert ($fontOk) 'IBM Plex Sans TC loaded'
    Write-Host "fader: vertical wm=$($vert.wm) $($vert.h)x$($vert.w), ctrl+click -> $g1, wheel -> $gw, plex-tc=$fontOk"

    # --- 11. 雙擊改名:Enter 送出、Esc 取消 ---
    [void](EvalJs @"
(() => {
  const n = document.querySelector('.strip .name');
  n.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 400
    $hasEdit = EvalJs "document.querySelector('.strip .nameedit') != null" 5000
    Assert ($hasEdit) 'dblclick name shows editor'
    [void](EvalJs @"
(() => {
  const e = document.querySelector('.strip .nameedit');
  e.value = 'Renamed';
  e.dispatchEvent(new Event('input', { bubbles: true }));
  e.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    $nm = EvalJs "window.__engine.tracks.find(t => t.kind === 'audio').name" 5000
    Assert ($nm -eq 'Renamed') "rename wired via track_set name (got $nm)"
    # Esc:不送
    [void](EvalJs @"
(() => {
  const n = document.querySelector('.strip .name');
  n.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 400
    [void](EvalJs @"
(() => {
  const e = document.querySelector('.strip .nameedit');
  e.value = 'ZZZ';
  e.dispatchEvent(new Event('input', { bubbles: true }));
  e.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 400
    $nm2 = EvalJs "(() => ({ n: window.__engine.tracks.find(t => t.kind === 'audio').name, edit: document.querySelector('.strip .nameedit') != null }))()" 5000
    Assert ($nm2.n -eq 'Renamed') "esc keeps name (got $($nm2.n))"
    Assert ($nm2.edit -eq $false) 'esc closes editor'
    Write-Host "rename: '$nm' applied, esc keeps + closes"

    # --- 12. 拖曳排序:同帶重排、跨帶擋下(synthetic DnD)---
    [void](EvalJs @"
(() => {
  const lane = document.querySelectorAll('.lanes')[0]; // 輸入帶
  const dt = new DataTransfer();
  lane.querySelector('.strip .head').dispatchEvent(new DragEvent('dragstart', { bubbles: true, dataTransfer: dt }));
  const strips = lane.querySelectorAll('.strip');
  const r2 = strips[strips.length - 1].getBoundingClientRect();
  const x = r2.right + 5; // 尾端
  lane.dispatchEvent(new DragEvent('dragover', { bubbles: true, clientX: x, dataTransfer: dt }));
  lane.dispatchEvent(new DragEvent('drop', { bubbles: true, clientX: x, dataTransfer: dt }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    $ord = EvalJs "(() => ({ stub: window.__engine.tracks.map(t => t.kind), dom: [...document.querySelectorAll('.lanes')[0].querySelectorAll('.strip .name')].map(e => e.textContent) }))()" 5000
    $inStub = ($ord.stub | Where-Object { $_ -ne 'output' }) -join ','
    Assert ($inStub -eq 'fx,audio') "same-lane drag reorders stub (input order: $inStub)"
    Assert ($ord.dom.length -eq 2 -and $ord.dom[0] -like 'fx*') "DOM follows new order ($($ord.dom -join ','))"
    # 跨帶:輸出軌拖去輸入帶 = 不動
    [void](EvalJs @"
(() => {
  const lanes = document.querySelectorAll('.lanes');
  const dt = new DataTransfer();
  lanes[1].querySelector('.strip .head').dispatchEvent(new DragEvent('dragstart', { bubbles: true, dataTransfer: dt }));
  lanes[0].dispatchEvent(new DragEvent('dragover', { bubbles: true, clientX: 10, dataTransfer: dt }));
  lanes[0].dispatchEvent(new DragEvent('drop', { bubbles: true, clientX: 10, dataTransfer: dt }));
  lanes[1].dispatchEvent(new DragEvent('dragend', { bubbles: true }));
  return true;
})()
"@ 5000)
    Start-Sleep -Milliseconds 600
    $ord2 = EvalJs "window.__engine.tracks.map(t => t.kind)" 5000
    Assert ("$($ord.stub)" -eq "$($ord2)") "cross-group drag rejected (before=$($ord.stub -join ',') after=$($ord2 -join ','))"
    Write-Host "dnd: same-lane reorder OK ($inStub), cross-group rejected OK"


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
