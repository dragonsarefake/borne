// Serve the real page out of loadcell_testbed.ino with a fake box behind it,
// so it can be LOOKED AT in a browser at phone size before anything is flashed.
//
//   node preview.js            then open http://localhost:8711
//   node preview.js 9000       to use another port
//
// Why this exists: the page harness only proves the render functions do not
// throw against stub data. It went green on a full UI rewrite that turned out
// to be unusable, because nobody had seen it. This is the missing half - the
// page exactly as the ESP32 serves it, driven by realistic /data, over real
// HTTP, in a real browser.
//
// Switch what the fake box is doing:  http://localhost:8711/_s/<scenario>
// Scenarios:  commissioned  fresh  midsetup  armed  monitor  noisy  french
const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = parseInt(process.argv[2] || '8711', 10);
const SRC = path.join(__dirname, 'loadcell_testbed.ino');

function page() {
  const ino = fs.readFileSync(SRC, 'utf8');
  const a = ino.indexOf('R"HTML(');
  const b = ino.indexOf(')HTML"');
  if (a < 0 || b < 0) throw new Error('PAGE[] literal not found in the sketch');
  return ino.slice(a + 'R"HTML('.length, b);
}

// ---------------------------------------------------------------- fixtures --
// Base is a commissioned, calibrated cabinet sitting idle - the state a real
// box spends almost all of its life in.
const BASE = {
  g: -1.7, raw: 12712, axis: 0, ax: 0, db: 50, sd: '6.2', pp: '23.0', fs: 50000,
  cal: 1, stable: 1, pad: 1, seq: 0, st: 0, k: 'idle', stage: 1, done: 7,
  azt: 1, cpg: '5.079', tare: 12325, msg: '', inv: 1, kg: 0,
  rt: 0.1806, rb: 0.8472,
  tn: { ht: 1000, hrf: 0.5, hrn: 3, hmax: 2000, flen: 10, trim: 2,
        ssd: 3, spp: 8, noise: 39.6, arate: 15, awin: 400, asettle: 1500, gs: 0,
        caln: 2, linpct: '0.004', ling: 41.2,
        pts: [[5000, 37720], [20000, 114300]] }
};
const INFO = {
  hx: { ok: 1, dt: 1, s: 12712 },
  sn: { ok: 1, mean: 12504, sd: '39.6', min: 12450, max: 12566, fs: '0.1' },
  hit: [42600, 18, 137, 2, 3.24], hits: 7, drift: -12.4, azt: 1
};

function hits(n) {
  const h = [];
  for (let i = 0; i < n; i++) {
    const peak = 18000 + Math.round(Math.abs(Math.sin(i * 1.7)) * 52000);
    h.push([peak, +(peak / 13000).toFixed(2), 12 + (i % 9), 90 + (i % 40),
            i % 5 === 0 ? 2 : 3, i * 7]);
  }
  return { n, seq: 100 + n, cal: 1, h };
}

const SCENARIOS = {
  // what you should see almost all the time
  commissioned: { d: {}, i: {}, h: hits(23) },
  // a box straight off the flasher - nothing measured, nothing calibrated
  fresh: { d: { cal: 0, done: 0, k: 'idle', cpg: '0.000', tare: 0, g: 0,
                tn: { ...BASE.tn, noise: 0, caln: 0, pts: [] } },
           i: { hx: { ok: 1, dt: 1, s: 8388607 }, sn: { ok: 0 }, hit: null, hits: 0 },
           h: { n: 0, seq: 0, cal: 0, h: [] } },
  // part way through the three steps
  midsetup: { d: { k: 'zero', done: 1, stage: 2, cal: 0, cpg: '0.000' },
              i: {}, h: hits(0) },
  // somebody is hitting it: the trace and both axis gauges move
  armed: { d: { g: 0.4 }, i: {}, h: hits(41) },
  monitor: { d: { k: 'mon' }, i: {}, h: hits(5) },
  // the reading will not settle - the case that hid the drift bug for weeks
  noisy: { d: { stable: 0, sd: '61.0', pp: '210.0' }, i: {}, h: hits(3) },
  french: { d: {}, i: {}, h: hits(23) }
};

let current = 'commissioned';
let t0 = Date.now();

function dataFor() {
  const sc = SCENARIOS[current] || SCENARIOS.commissioned;
  const d = { ...BASE, ...sc.d, tn: { ...BASE.tn, ...(sc.d.tn || {}) } };
  // Make the trace move, or the graph is a flat line and tells you nothing.
  const t = (Date.now() - t0) / 1000;
  if (d.cal) {
    const idle = Math.sin(t * 1.3) * 6 + Math.sin(t * 7.1) * 2;
    d.g = current === 'armed'
      ? +(Math.max(0, Math.sin(t * 0.9) ** 12 * 61000) + idle).toFixed(1)
      : +idle.toFixed(1);
    // Same rule as the firmware: inside the noise band the axis is exactly 0.
    d.axis = d.g > d.db ? Math.round(Math.min(1, d.g / d.fs) * 32767) : 0;
    d.raw = Math.round(d.tare + d.g * parseFloat(d.cpg));
  }
  d.drift = -12.4;
  d.name = 'esp32borne#1';
  d.fw = '1.0.0';
  return d;
}

const LOG = [
  'ESP32 load-cell testbed  fw 1.0.0',
  'Gamepad : "esp32borne#1"',
  'WiFi    : hidden network "esp32borne#1"',
  'Setup complete - going straight to readings.',
  ''
].join('\n');

const DOC = `# Interface

## Hauteurs des rangees

| rangee | boutons | hauteur |
|---|---|---|
| 1 | L1 R1 | 0.1806 |
| 5 | L5 R5 | 0.8472 |

## La force

Le plateau est une MANETTE (joystick). La force est UN AXE, lineaire.

    axe 0.0  = rien sur le plateau
    axe 1.0  = 1000 kg
`;

function json(res, obj) {
  const b = Buffer.from(JSON.stringify(obj));
  res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': b.length });
  res.end(b);
}

http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  const p = u.pathname;
  const sc = SCENARIOS[current] || SCENARIOS.commissioned;

  if (p.startsWith('/_s/')) {
    const name = p.slice(4);
    if (SCENARIOS[name]) { current = name; t0 = Date.now(); }
    res.writeHead(302, { Location: '/' });
    return res.end();
  }
  if (p === '/_list') return json(res, { current, scenarios: Object.keys(SCENARIOS) });
  if (p === '/_probe') {
    console.log('PROBE[' + current + '] ' + (u.searchParams.get('d') || ''));
    res.writeHead(200); return res.end('ok');
  }

  if (p === '/data') return json(res, dataFor());
  if (p === '/info') return json(res, { ...INFO, ...sc.i });
  if (p === '/hits') {
    if (u.searchParams.has('clear')) sc.h = { n: 0, seq: 0, cal: 1, h: [] };
    return json(res, sc.h);
  }
  if (p === '/log') {
    const since = parseInt(u.searchParams.get('since') || '0', 10);
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    return res.end(since >= LOG.length ? '' : LOG.slice(since));
  }
  if (p === '/doc') {
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    return res.end(DOC);
  }
  if (p === '/cmd' || p === '/set') {
    // Enough to keep the page honest: mode keys move the state kind, so the
    // arm / stop buttons actually do something you can see.
    const c = (u.searchParams.get('c') || '').toUpperCase();
    const d = SCENARIOS[current].d;
    if (c === 'K') d.k = 'hit';
    else if (c === 'M') d.k = 'mon';
    else if (c === 'X') d.k = 'idle';
    else if (c === 'S') d.k = 'menu';
    else if (c === 'I') d.inv = (d.inv === undefined ? BASE.inv : d.inv) ? 0 : 1;
    // /set really stores, with the firmware's own clamps. A fake that accepts
    // and forgets is how a box that would not keep a typed value looked fine.
    if (p === '/set') {
      const q = u.searchParams, f = k => parseFloat(q.get(k));
      console.log('SET ' + u.search);
      if (q.has('fs') && f('fs') > 0) d.fs = f('fs');
      if (q.has('ax') && f('ax') >= 0 && f('ax') <= 2) d.ax = f('ax') | 0;
      if (q.has('kg')) d.kg = f('kg') ? 1 : 0;
      if (q.has('azt')) d.azt = f('azt') ? 1 : 0;
      if (q.has('gs') && f('gs') >= 0 && f('gs') <= 4)
        d.tn = { ...BASE.tn, ...(d.tn || {}), gs: f('gs') | 0 };
      if (q.has('sh') && f('sh') > 0 && f('rt') >= 0 && f('rb') > f('rt') && f('rb') <= f('sh')) {
        d.rt = f('rt') / f('sh'); d.rb = f('rb') / f('sh');
      }
    }
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    return res.end(p === '/set' ? 'saved' : 'ok');
  }

  // A phone-sized frame in a desktop window. An iframe establishes its own
  // viewport, so width=device-width inside resolves to the frame width - which
  // is exact and repeatable, unlike headless Chrome's --window-size.
  //   /phone            one 390x844 handset
  //   /phone?w=360&h=780
  //   /wall             several widths side by side, to catch what breaks where
  if (p === '/phone' || p === '/wall') {
    const sizes = p === '/wall'
      ? [[320, 720, 'iPhone SE'], [390, 844, 'iPhone 14'], [412, 915, 'Pixel 7']]
      : [[parseInt(u.searchParams.get('w') || '390', 10),
          parseInt(u.searchParams.get('h') || '844', 10), 'phone']];
    const q = u.searchParams.has('open') ? '?open=' + u.searchParams.get('open') : '';
    const frames = sizes.map(([w, h, label]) =>
      `<figure style="margin:0">
         <figcaption style="font:12px system-ui;color:#8d95a3;padding:0 0 6px">
           ${label} &middot; ${w}&times;${h}</figcaption>
         <iframe src="/${q}" width="${w}" height="${h}" frameborder="0"
           style="border:1px solid #333;border-radius:18px;background:#000"></iframe>
       </figure>`).join('');
    const body = `<!doctype html><meta charset=utf-8>
<body style="margin:0;padding:18px;background:#0f1114;display:flex;gap:18px;
 align-items:flex-start;font:14px system-ui">
<div style="display:flex;gap:18px">${frames}</div>
<div style="font:12px/1.6 system-ui;color:#8d95a3">
  <b style="color:#eceef1">${current}</b><br>
  ${Object.keys(SCENARIOS).map(s =>
    `<a href="/_s/${s}" style="color:${s === current ? '#4c8dff' : '#8d95a3'}">${s}</a>`
  ).join('<br>')}
</div></body>`;
    const bb = Buffer.from(body, 'utf8');
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8',
                         'Content-Length': bb.length, 'Cache-Control': 'no-store' });
    return res.end(bb);
  }

  if (p === '/' || p === '/index.html') {
    let html;
    try { html = page(); }
    catch (e) {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      return res.end('could not read the page out of the sketch: ' + e.message);
    }
    // French scenario: preset the language the page would otherwise ask for.
    let pre = (current === 'french')
      ? '<script>try{localStorage.setItem("lang","fr")}catch(e){}</script>'
      : '<script>try{if(!localStorage.getItem("lang"))localStorage.setItem("lang","en")}catch(e){}</script>';
    // ?probe=1 reports layout facts a screenshot can only hint at, into the
    // <title> so --dump-dom can read them: anything wider than the viewport
    // means the page scrolls sideways on a phone, which is always a bug.
    // ?open=dset,dcon unfolds those sections on load (or ?open=all), so a
    // headless capture can reach what is folded away.
    if (u.searchParams.has('open')) {
      const ids = u.searchParams.get('open').replace(/[^A-Za-z0-9,]/g, '');
      pre += '<script>setTimeout(function(){var w="' + ids + '";' +
        'var n=document.querySelectorAll("details");for(var i=0;i<n.length;i++)' +
        'if(w=="all"||(","+w+",").indexOf(","+n[i].id+",")>=0)n[i].open=true;},500);</script>';
    }
    // ?fstest=1 replays the bug a phone hit: type a full scale, tap away, wait
    // longer than a tick, then press Save. The value must still be in the box
    // and must be what reaches /set. Result goes to the server console.
    if (u.searchParams.has('fstest')) {
      pre += '<script>setTimeout(function(){var f=document.getElementById("fs");' +
        'var was=f.value;f.focus();f.value="42,5";' +
        'f.dispatchEvent(new Event("input",{bubbles:true}));f.blur();' +
        'setTimeout(function(){var held=f.value;saveFs();' +
        'setTimeout(function(){fetch("/_probe?d="+encodeURIComponent(' +
        '"FSTEST was="+was+" afterBlur="+held+" afterSave="+f.value+" stored="+dat.fs));' +
        '},900);},900);},900);</script>';
    }
    // ?axis=1 reports the graph range once a second, so its stability can be
    // measured over time instead of judged from a single screenshot.
    if (u.searchParams.has('axis')) {
      pre += '<script>setInterval(function(){var e=document.getElementById("gax");' +
        'if(e)fetch("/_probe?d=AXIS "+encodeURIComponent(e.textContent));},1000);</script>';
    }
    if (u.searchParams.has('probe')) {
      pre += '<script>setTimeout(function(){' +
        'var de=document.documentElement,w=de.clientWidth,over=[];' +
        'var all=document.querySelectorAll("*");' +
        'for(var i=0;i<all.length;i++){var r=all[i].getBoundingClientRect();' +
        'if(r.width>0&&r.right>w+1)over.push((all[i].id||all[i].tagName+"."+' +
        '(all[i].className||"")).slice(0,34)+"@"+Math.round(r.right));}' +
        'fetch("/_probe?d="+encodeURIComponent("VIEW "+w+" DOC "+de.scrollWidth+' +
        '" OVER "+(over.slice(0,14).join(" | ")||"none")));},900);</script>';
    }
    html = html.replace('</head>', pre + '</head>');
    const b = Buffer.from(html, 'utf8');
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8',
                         'Content-Length': b.length, 'Cache-Control': 'no-store' });
    return res.end(b);
  }

  res.writeHead(404); res.end('no');
}).listen(PORT, '127.0.0.1', () => {
  console.log('preview on http://localhost:' + PORT);
  console.log('scenario: ' + current);
  console.log('switch:   http://localhost:' + PORT + '/_s/<' +
              Object.keys(SCENARIOS).join('|') + '>');
});
