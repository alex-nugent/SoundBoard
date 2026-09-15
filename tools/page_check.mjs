// Drives the settings page in headless Chrome against tools/mock_portal.py
// through the DevTools protocol (no dependencies: node 22+ has WebSocket).
//   node tools/page_check.mjs            (starts its own mock on port 8089 with --pads; a fresh one each run)
import { spawn } from 'node:child_process';
const URL_ = process.argv[2] || 'http://localhost:8089/';
const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const mock = spawn('python3', [new URL('./mock_portal.py', import.meta.url).pathname, '8089', '--pads'], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-sandbox', '--remote-debugging-port=9333', '--window-size=420,900', '--user-data-dir=/tmp/sb-page-check', 'about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ws, id = 0; const pending = new Map(); const logs = [];
async function connect() {
  for (let i = 0; i < 40; i++) {
    try { const r = await fetch('http://localhost:9333/json'); const list = await r.json(); const page = list.find(t => t.type === 'page'); if (page) return page.webSocketDebuggerUrl; } catch (e) { }
    await sleep(250);
  }
  throw new Error('chrome did not come up');
}
function send(method, params) { return new Promise((res, rej) => { const i = ++id; pending.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params: params || {} })); }); }
async function evaluate(expr) {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error('page: ' + (r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text));
  return r.result.value;
}
let fails = 0;
function check(name, ok, detail) { console.log((ok ? 'ok   ' : 'FAIL ') + name + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); if (!ok) fails++; }
try {
  const wsUrl = await connect();
  ws = new WebSocket(wsUrl);
  await new Promise(r => ws.onopen = r);
  ws.onmessage = ev => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id).res(m.result); pending.delete(m.id); } else if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map(a => a.value || a.description).join(' ')); else if (m.method === 'Runtime.exceptionThrown') logs.push('EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.exception && m.params.exceptionDetails.exception.description)); };
  await send('Runtime.enable'); await send('Page.enable');
  await send('Page.navigate', { url: URL_ });
  for (let i = 0; i < 40 && !(await evaluate('typeof S !== "undefined" && !!S.cfg && !!S.status')); i++) await sleep(250);
  check('page loaded with schema, config and status', await evaluate('!!S.schema && !!S.cfg && !!S.status'));
  check('setting rows rendered', await evaluate('document.querySelectorAll(".row").length') > 60, await evaluate('document.querySelectorAll(".row").length'));
  check('every schema group has a card', await evaluate('S.schema.settings.every(r => [...document.querySelectorAll("section[data-group]")].some(s => s.dataset.group.split(",").includes(r.group)))'));
  check('level cards', await evaluate('document.querySelectorAll(".level").length') === await evaluate('S.cfg.levels.length'));
  check('entries per level = sound pads', await evaluate('document.querySelectorAll(".entry").length === S.cfg.levels.length * nSoundPads()'));
  check('jack map text', /P1 → J/.test(await evaluate('document.querySelector("#jackMap").textContent')), await evaluate('document.querySelector("#jackMap").textContent'));
  check('status tiles', await evaluate('document.querySelectorAll("#statusTiles div").length') >= 10);
  check('save bar hidden at start', await evaluate('document.querySelector("#savebar").hidden'));
  // A scalar change through the DOM control.
  await evaluate(`(() => { const i = [...document.querySelectorAll('.row')].map(r => r.querySelector('input,select')).find(c => c && c.type === 'number' && c.min === '5'); i.value = Number(i.value) === 25 ? 15 : 25; i.dispatchEvent(new Event('change')); })()`);
  check('one change after editing a number', await evaluate('patch().n') === 1, await evaluate('patch().doc'));
  check('save bar shown', await evaluate('!document.querySelector("#savebar").hidden'));
  // Levels: add, goToLevel remap on move and delete.
  await evaluate('window.confirm = () => true; window.prompt = (m, d) => d;');
  const n0 = await evaluate('S.draft.levels.length');
  await evaluate('addLevel()');
  check('add level', await evaluate('S.draft.levels.length') === n0 + 1);
  await evaluate(`S.draft.levels[0].buttons[0].action = 'goToLevel'; S.draft.levels[0].buttons[0].goToLevel = ${n0 + 1}; S.draft.levels[0].buttons[0].label = 'check ' + Date.now(); moveLevel(${n0}, ${n0 - 1});`);
  check('goToLevel follows a move', await evaluate('S.draft.levels[0].buttons[0].goToLevel') === n0);
  await evaluate(`deleteLevel(${n0 - 1})`);
  check('goToLevel of a deleted level becomes 1', await evaluate('S.draft.levels[0].buttons[0].goToLevel') === 1 && await evaluate('S.draft.levels.length') === n0);
  check('patch carries levels', await evaluate('"levels" in patch().doc'));
  await evaluate('save()');
  await sleep(500);
  check('saved: draft equals config', await evaluate('same(S.draft, S.cfg) && patch().n === 0'));
  check('saved value on the mock', await evaluate('S.cfg.levels[0].buttons[0].action') === 'goToLevel');
  // Sounds: convert a synthetic recording (stereo, 8 kHz, with silence at both ends), upload it, rename, delete.
  const conv = await evaluate(`(async () => {
    const rate = 8000, n = rate * 2, ch = 2, v = new DataView(new ArrayBuffer(44 + n * ch * 2));
    const s = (o, t) => { for (let i = 0; i < t.length; i++) v.setUint8(o + i, t.charCodeAt(i)); };
    s(0,'RIFF'); v.setUint32(4, 36 + n*ch*2, true); s(8,'WAVE'); s(12,'fmt '); v.setUint32(16,16,true); v.setUint16(20,1,true); v.setUint16(22,ch,true); v.setUint32(24,rate,true); v.setUint32(28,rate*ch*2,true); v.setUint16(32,ch*2,true); v.setUint16(34,16,true); s(36,'data'); v.setUint32(40,n*ch*2,true);
    for (let i = 0; i < n; i++) { const t = i / rate; const x = (t > 0.5 && t < 1.5) ? Math.sin(2*Math.PI*440*t) * 0.3 : 0; v.setInt16(44 + i*4, x*32767, true); v.setInt16(46 + i*4, x*32767, true); }
    const f = new File([v.buffer], 'memo.wav', { type: 'audio/wav' });
    const w = await convertToWav(f);
    const h = new DataView(await w.blob.arrayBuffer());
    return { seconds: w.seconds, rate: h.getUint32(24, true), channels: h.getUint16(22, true), bits: h.getUint16(34, true), bytes: w.blob.size };
  })()`);
  check('converter: mono 44.1 kHz 16-bit', conv.rate === 44100 && conv.channels === 1 && conv.bits === 16, conv);
  check('converter: trimmed to the sound plus 20 ms each side', conv.seconds > 1.0 && conv.seconds < 1.1, conv.seconds);
  const before = await evaluate('S.sounds.length');
  await evaluate(`(async () => { const rate = 44100, n = 4410, v = new DataView(new ArrayBuffer(44 + n*2)); const s=(o,t)=>{for(let i=0;i<t.length;i++)v.setUint8(o+i,t.charCodeAt(i));};
    s(0,'RIFF'); v.setUint32(4,36+n*2,true); s(8,'WAVE'); s(12,'fmt '); v.setUint32(16,16,true); v.setUint16(20,1,true); v.setUint16(22,1,true); v.setUint32(24,rate,true); v.setUint32(28,rate*2,true); v.setUint16(32,2,true); v.setUint16(34,16,true); s(36,'data'); v.setUint32(40,n*2,true);
    for (let i=0;i<n;i++) v.setInt16(44+i*2, Math.sin(i/10)*20000, true);
    window.prompt = () => 'check_upload'; await addSound(new File([v.buffer], 'check upload.wav', { type: 'audio/wav' })); })()`);
  for (let i = 0; i < 20 && (await evaluate('S.sounds.length')) === before; i++) await sleep(250);
  check('upload added a sound', await evaluate('S.sounds.some(s => s.name === "check_upload.wav")'), await evaluate('S.sounds.map(s => s.name)'));
  await evaluate('window.prompt = () => "renamed_check.wav"; renameSound(S.sounds.find(s => s.name === "check_upload.wav"))');
  await sleep(400);
  check('rename', await evaluate('S.sounds.some(s => s.name === "renamed_check.wav")'));
  await evaluate('deleteSound(S.sounds.find(s => s.name === "renamed_check.wav"))');
  await sleep(400);
  check('delete', await evaluate('!S.sounds.some(s => s.name === "renamed_check.wav")'));
  // Cues, owner label, pattern editors write into the draft.
  await evaluate(`(() => { const s = document.querySelector('#cues select'); s.value = 'hello.wav'; s.dispatchEvent(new Event('change')); const o = document.querySelector('#ownerLabel input'); o.value = 'Return to Alex'; o.dispatchEvent(new Event('change')); const p = document.querySelector('#vibPattern'); p.value = '100, 50, 100'; p.dispatchEvent(new Event('change')); })()`);
  check('structured editors change the draft', await evaluate('S.draft.audio.cues.startup === "hello.wav" && S.draft.device.ownerLabel[0] === "Return to Alex" && JSON.stringify(S.draft.levelChange.vibration.pattern) === "[100,50,100]"'));
  check('patch has three structured keys', await evaluate('patch().n') === 3, await evaluate('Object.keys(patch().doc)'));
  await evaluate('discard()'); await sleep(200);
  check('discard clears', await evaluate('patch().n') === 0);
  // Firmware card: status, check, the available version and the Install button.
  for (let i = 0; i < 20 && !(await evaluate('!!S.fw')); i++) await sleep(250);
  check('firmware status line', /Running/.test(await evaluate('document.querySelector("#fwStatus").textContent')));
  await evaluate('document.querySelector("#btnFwCheck").click()');
  for (let i = 0; i < 40 && !(await evaluate('S.fw && S.fw.available')); i++) await sleep(500);
  check('check found a release', await evaluate('S.fw && S.fw.available && S.fw.available.version') === 'v0.11.0');
  check('install button offered', await evaluate('!document.querySelector("#btnFwInstall").hidden && document.querySelector("#btnFwInstall").textContent') === 'Install v0.11.0');
  // Diagnostics and Identify pads (the mock fakes a press every ~3 s with --pads).
  await evaluate('startDiag()'); await sleep(1500);
  check('diag pads rendered', await evaluate('document.querySelectorAll("#diagPads .pad").length') === 4);
  await evaluate('startIdentify()');
  for (let i = 0; i < 80 && !(await evaluate('S.identify && S.identify.step === 4')); i++) await sleep(250);
  check('identify collected four channels', await evaluate('S.identify && S.identify.step === 4'), await evaluate('S.identify && S.identify.chans'));
  check('identify offers Save', await evaluate('document.querySelector("#identify button").textContent') === 'Save the pad order');
  await evaluate('stopIdentify()');
  check('no page exceptions', !logs.some(l => l.startsWith('EXCEPTION')), logs.filter(l => l.startsWith('EXCEPTION')));
} catch (e) { console.log('FAIL ' + e.message); fails++; }
chrome.kill(); mock.kill();
console.log(fails ? fails + ' failure(s)' : 'all checks passed');
process.exit(fails ? 1 : 0);
