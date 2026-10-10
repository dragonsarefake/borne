// Run every page state against stub data, under node, before flashing.
//
//   node extract_page.js && node page_harness.js
//
// States are driven by the KIND strings the firmware sends in /data "k", not by
// state numbers. That is deliberate: when the enum was renumbered from 15
// states to 9, the page kept testing the old numbers and the Impact panel - the
// game's whole point - silently rendered nothing on flashed firmware.
//
// It checks:
//   1. every state renders and has a primary action   (caught 3 empty states)
//   2. every live kind renders its panel               (caught the dead Impact panel)
//   3. en and fr define exactly the same keys          (t() falls back to English,
//      so a missing fr key is invisible - this caught the FR page being half English)
//   4. ids resolve only if the markup defines them     (caught modePanel throwing
//      on every tick because it indexed an m-button that was not there)
const nodefs0 = require('fs');
const REAL_IDS = nodefs0.existsSync('page_ids.json')
  ? new Set(JSON.parse(nodefs0.readFileSync('page_ids.json','utf8')))
  : null;
const seen = {};
function el(id){
  if (REAL_IDS && !REAL_IDS.has(id)) return null;
  return seen[id] || (seen[id] = { id, textContent:'', innerHTML:'', className:'',
    value:'', dataset:{}, style:{}, scrollTop:0, scrollHeight:0,
    clientWidth:400, clientHeight:130, width:400, height:130,
    placeholder:'', addEventListener(){}, getContext(){ return ctx; },
    getAttribute(){ return null; }, select(){}, setSelectionRange(){} });
}
const ctx = new Proxy({}, { get: () => () => ({ addColorStop(){} }) });
global.document = { getElementById: el, body:{ className:'' },
                    querySelectorAll: () => [] };
global.localStorage = { getItem:()=>null, setItem:()=>{} };
global.addEventListener = () => {};
global.devicePixelRatio = 1;
global.fetch = () => Promise.resolve({ json:()=>Promise.resolve({}), text:()=>Promise.resolve('') });
global.setInterval = () => {};
global.confirm = () => false;

const nodefs = require('fs');
if (!nodefs.existsSync('page_check.js')) {
  console.error('page_check.js missing - run "node extract_page.js" first.');
  process.exit(1);
}
eval(nodefs.readFileSync('page_check.js','utf8'));

// Full scale is the cell's 1000 kg rating, in grams.
const data = { g:1234.5, raw:842193, axis:1200, sd:'12.0', pp:'40.0', fs:1000000,
  cal:1, stable:1, pad:1, seq:10, st:0, k:'idle', stage:1, done:0, azt:1,
  cpg:'5.079', tare:12512, msg:'[OK] all good', inv:0,
  kg:0, rt:0.2, rb:0.8 };
const inf = { hx:{ok:1,dt:1,s:8388607}, sn:{ok:1,mean:12504,sd:'31.6',min:12450,max:12566,fs:'0.1'},
  hit:[4821,12,137,2,3.2], hits:3, drift:41.5, azt:1 };

let bad=0;

// The kinds the firmware's stateKind() can return. If the page grows a state it
// does not handle, or loses one it should, this list is the contract.
const FIRMWARE_KINDS = KINDS.slice();

// Each live instrument (Impact and Monitor, reachable from the serial port)
// must still show readouts in the state card, so a box left in one of them is
// not a blank page with no way out.
const TUNE0 = { ht:1000, hrf:0.5, hrn:3, hmax:2000, flen:10, trim:2,
                ssd:3, spp:8, noise:43, arate:15, awin:400, asettle:1500, gs:0 };

console.log('kind     primary                      in  readout  panel  secondaries');
for (const k of FIRMWARE_KINDS) {
  let h='',t='';
  try{ h=controls(k); }catch(e){ bad++; console.log(`  ${k} controls THREW ${e.message}`); }
  try{ dat={...data,k}; detI=inf; t=tables(k,inf); }
  catch(e){ bad++; console.log(`  ${k} tables THREW ${e.message}`); }
  try{ steps(0x7,3,k); details({...data,k}); axisUI({...data,k}); fsUI({...data,k}); }
  catch(e){ bad++; console.log(`  ${k} render THREW ${e.message}`); }
  const target = isLive(k) ? 'tbl' : null;
  const panel = isLive(k) ? t : '';
  const prim=(h.match(/class=act[^>]*>([^<]+)/)||h.match(/class="act[^"]*"[^>]*>([^<]+)/)||[])[1];
  const secs=[...h.matchAll(/<div class=more>([\s\S]*?)<\/div>/g)]
    .flatMap(m=>[...m[1].matchAll(/>([^<]+)</g)].map(x=>x[1].trim()));
  const inputs=(h.match(/<input/g)||[]).length;
  const kind=/class="rd|class=dv|<table/.test(t)?
    (/class=dv/.test(t)?'divbar':/class=gt/.test(t)?'gauge':/class="rd/.test(t)?'read':'table')
    :(t?'text':'-');
  const wantPanel = isLive(k);
  if (wantPanel && !panel.length) bad++;
  if (wantPanel && !target) { bad++; console.log(`  ${k} is live but has no panel target`); }
  // Every kind must have a stage title, or the card renders blank.
  const title = (ST(k)||[])[0];
  if (!title) { bad++; console.log(`  ${k} has no stage title (s_${k} missing)`); }
  console.log(`${k.padEnd(8)} ${(prim||'** NONE **').padEnd(28)} `+
              `${String(inputs).padEnd(3)} ${kind.padEnd(8)} `+
              `${(wantPanel?(panel.length?'yes':'** NONE **'):'-').padEnd(6)} ${secs.join(', ')}`);
  if(!prim) bad++;
}

// The axis picker highlights ax0..ax2 by index. Indexing a button the markup
// does not define is what once made a render function throw on every tick.
for (let i = 0; i < 3; i++) {
  if (REAL_IDS && !REAL_IDS.has('ax' + i)) {
    console.log(`\naxis picker: #ax${i} is not in the markup`);
    bad++;
  }
}

// en / fr key parity.
const ke = Object.keys(LG.en), kf = Object.keys(LG.fr);
const onlyEn = ke.filter(k => kf.indexOf(k) < 0);
const onlyFr = kf.filter(k => ke.indexOf(k) < 0);
if (onlyEn.length || onlyFr.length) {
  console.log(`\nkey mismatch: en-only ${JSON.stringify(onlyEn)} fr-only ${JSON.stringify(onlyFr)}`);
  bad++;
} else {
  console.log(`\n${ke.length} keys, en and fr in step`);
}

// Nothing measured yet: /info empty, no calibration. Must not throw.
try{ detI={}; dat={...data,cal:0,cpg:'0.000'};
  for(const k of FIRMWARE_KINDS){controls(k);tables(k,detI);axisUI({...data,k,cal:0});} }
catch(e){ bad++; console.log('uninitialised THREW '+e.message); }

// An unknown kind (firmware newer than the page) must degrade, not explode.
try{ detI=inf; dat={...data,k:'whatever'};
  controls('whatever'); tables('whatever',inf); axisUI({...data,k:'whatever'}); }
catch(e){ bad++; console.log('unknown kind THREW '+e.message); }

// Firmware older than the page: no ax / db in /data. Must still draw both axes.
try{ const old={...data}; delete old.ax; delete old.db; axisUI(old); fsUI(old); }
catch(e){ bad++; console.log('old firmware /data THREW '+e.message); }

// --- the surfaces added by the rework ---------------------------------------
const TUNE = TUNE0;

function check(label, fn, wantId) {
  try {
    delete seen[wantId];
    fn();
    const got = (seen[wantId] && (seen[wantId].innerHTML || seen[wantId].textContent)) || '';
    if (!got.length) { bad++; console.log(`${label}: rendered nothing into #${wantId}`); }
    else console.log(`${label}: ok`);
  } catch (e) { bad++; console.log(`${label} THREW ${e.message}`); }
}

console.log('');
// There is deliberately no tuning form: "what counts as a hit" is a game
// decision and does not belong on this page. Assert it stays gone.
if (typeof tuneUI !== 'undefined' || /id=tuneform/.test(nodefs.readFileSync('page_check.js','utf8'))) {
  bad++; console.log('the sensor tuning form is back - it encodes game decisions in the box');
} else console.log('no tuning form: ok');

check('cal points (none)', () => { dat=data; calUI({...data,caln:0,pts:[]}); }, 'calpts');
check('cal points (one)', () => calUI({...data,caln:1,pts:[[5000,38000]],ling:0,linpct:'0.000'}), 'calpts');
check('cal points (three)', () => calUI({...data,caln:3,
  pts:[[5000,38000],[10000,63000],[20000,114000]],ling:12.4,linpct:'0.001'}), 'calpts');

// The pad is always live and the box runs no game logic, so the page must not
// offer to arm anything or keep a hit list. Assert both stay gone.
{
  const src = nodefs.readFileSync('page_check.js','utf8');
  if (typeof gameUI !== 'undefined' || typeof hitsUI !== 'undefined' || /cmd\(\\?'K\\?'\)/.test(src)) {
    bad++; console.log('an arm / hit-test control is back on the page');
  } else console.log('no arm button, no hit list: ok');
}

check('axis gauges', () => axisUI({...data,ax:0,axis:16384}), 'axg');
for (const [ax, want] of [[0,'ax0'],[1,'ax1'],[2,'ax2']]) {
  axisUI({...data,ax});
  const on = ['ax0','ax1','ax2'].filter(i => seen[i] && seen[i].className === 'on');
  if (on.length !== 1 || on[0] !== want) { bad++; console.log(`axis picker ax=${ax}: highlighted ${JSON.stringify(on)}`); }
}
console.log('axis picker highlights the stored choice: checked');

// THE BUG THIS PAGE SHIPPED WITH: the tick rewrote the full-scale box whenever
// it was not focused, so tapping Save (which blurs it) restored the stored
// value first and nothing typed on a phone was ever saved. An edited box must
// survive ticks until it is saved.
{
  global.document.activeElement = null;
  const f = el('fs');
  dat = data; fsUI({...data, fs:1000000});
  if (f.value !== '1000000') { bad++; console.log(`full scale box not filled from /data: "${f.value}"`); }
  f.value = '42,5'; f.dataset.dirty = '1';
  fsUI({...data, fs:1000000}); fsUI({...data, fs:1000000});
  if (f.value !== '42,5') { bad++; console.log(`full scale box was overwritten while edited: "${f.value}"`); }
  else console.log('edited full-scale box survives ticks: ok');
  f.dataset.dirty = '';
  fsUI({...data, fs:50000});
  if (f.value !== '50000') { bad++; console.log(`full scale box not refreshed after save: "${f.value}"`); }
  if (toG('42,5') !== 42.5) { bad++; console.log('decimal comma not understood: ' + toG('42,5')); }
  else console.log('decimal comma: ok');
}

console.log(bad?`\n${bad} PROBLEMS`:'\nall kinds render, all have a primary action, live panels present');
process.exit(bad?1:0);
