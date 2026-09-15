// SoundBoard V4 settings page (FirmwareSpec.md §15.3). One file, no
// framework: the phone is on the board's network with no internet. The
// settings forms come from /api/schema; the levels, sounds, cues, owner
// label, vibration pattern and pad-order editors are written by hand.
'use strict';
const $ = (s, el) => (el || document).querySelector(s);
const $$ = (s, el) => Array.from((el || document).querySelectorAll(s));
const S = {
  schema: null, cfg: null, draft: null, sounds: [], status: null, diag: null,
  // The sign-in sheet cannot pick files (§15.3). Older iOS sheets say CaptiveNetworkSupport; newer ones are a bare
  // WebKit view with no Safari token, which real Safari always carries. Android's sheet is a WebView.
  captive: /CaptiveNetworkSupport|Android.*WebView|; wv\)/i.test(navigator.userAgent) || (/iPhone|iPad|iPod/.test(navigator.userAgent) && !/Safari\//.test(navigator.userAgent)),
  previewed: {}, identify: null, upload: null, diagTimer: null, statusTimer: null
};
const STRUCT = ['levels', 'audio.cues', 'device.ownerLabel', 'levelChange.vibration.pattern', 'hardware.padChannels', 'pads.roles', 'touch.padPressPct'];
const ENTRY_DEF = { sound: '', label: '', type: '', key: '', keyMode: 'type', action: 'none', goToLevel: 1, volumePct: 100, vibrate: null, jack: null };
const SECTIONS = [['status', 'Status'], ['levels', 'Levels'], ['buttons', 'Buttons'], ['sounds', 'Sounds'], ['audio', 'Audio'], ['btspk', 'BT speaker'],
  ['keyboard', 'Keyboard'], ['vibration', 'Vibration'], ['jacks', 'Jacks'], ['display', 'Display'], ['power', 'Power'], ['device', 'Device'],
  ['backup', 'Backup'], ['firmware', 'Firmware'], ['diag', 'Diagnostics']];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
function getPath(o, p) { return p.split('.').reduce((a, k) => (a == null ? undefined : a[k]), o); }
function setPath(o, p, v) {
  const ks = p.split('.'); let a = o;
  for (let i = 0; i < ks.length - 1; i++) { if (a[ks[i]] == null || typeof a[ks[i]] !== 'object') a[ks[i]] = {}; a = a[ks[i]]; }
  a[ks[ks.length - 1]] = v;
}
const clone = o => JSON.parse(JSON.stringify(o));
const same = (a, b) => JSON.stringify(a === undefined ? null : a) === JSON.stringify(b === undefined ? null : b);
function el(tag, attrs, children) {
  const e = document.createElement(tag);
  if (attrs) for (const k in attrs) {
    if (k === 'class') e.className = attrs[k];
    else if (k === 'text') e.textContent = attrs[k];
    else if (k === 'html') e.innerHTML = attrs[k];
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), attrs[k]);
    else if (attrs[k] !== undefined && attrs[k] !== null && attrs[k] !== false) e.setAttribute(k, attrs[k] === true ? '' : attrs[k]);
  }
  (children || []).forEach(c => { if (c == null) return; e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c); });
  return e;
}
let toastT = null;
function toast(msg, kind) {
  const t = $('#toast'); t.textContent = msg; t.className = 'toast on ' + (kind || '');
  clearTimeout(toastT); toastT = setTimeout(() => { t.className = 'toast'; }, kind === 'err' ? 6000 : 2500);
}
async function api(path, opts) {
  const r = await fetch(path, Object.assign({ cache: 'no-store' }, opts || {}));
  let j = null; const txt = await r.text();
  try { j = JSON.parse(txt); } catch (e) { j = null; }
  if (!r.ok) throw new Error((j && j.error) || ('HTTP ' + r.status));
  return j === null ? txt : j;
}
function formBody(o) { const b = new URLSearchParams(); for (const k in o) if (o[k] != null) b.append(k, o[k]); return { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: b.toString() }; }
const post = (path, o) => api(path, formBody(o));
async function action(name, extra) {
  try { const r = await post('/api/action', Object.assign({ name: name }, extra || {})); if (r && r.message) toast(r.message, 'ok'); return r; }
  catch (e) { toast(e.message, 'err'); throw e; }
}
const secs = n => (n < 60 ? n + ' s' : n < 3600 ? Math.floor(n / 60) + ' min' : Math.floor(n / 3600) + ' h ' + Math.floor((n % 3600) / 60) + ' min');
const kb = b => (b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : Math.round(b / 1024) + ' KB');
function nSoundPads() { return (S.draft.pads.roles || []).filter(r => r === 'sound').length; }
function soundPositions() { const out = []; (S.draft.pads.roles || []).forEach((r, i) => { if (r === 'sound') out.push(i); }); return out; }
function row(schemaRow) { return S.schema.settings.find(r => r.path === schemaRow); }
function defOf(r) { if (r.type === 'bool') return r.def === 'true'; if (r.type === 'enum' || r.type === 'string') return r.def; return Number(r.def); }
function valOf(r) { const v = getPath(S.draft, r.path); return v === undefined || v === null ? defOf(r) : v; }

// ---------------------------------------------------------------------------
// Dirty tracking, save, discard
// ---------------------------------------------------------------------------
function patch() {
  const p = {}; let n = 0;
  for (const r of S.schema.settings) { const a = getPath(S.draft, r.path), b = getPath(S.cfg, r.path); if (!same(a, b)) { setPath(p, r.path, a); n++; } }
  for (const sp of STRUCT) { const a = getPath(S.draft, sp), b = getPath(S.cfg, sp); if (!same(a, b)) { setPath(p, sp, a === undefined ? null : a); n++; } }
  return { doc: p, n: n };
}
function updateDirty() {
  const n = patch().n;
  $('#savebar').hidden = n === 0;
  $('#saveText').textContent = n === 1 ? '1 change not saved' : n + ' changes not saved';
}
window.addEventListener('beforeunload', e => { if (patch().n) { e.preventDefault(); e.returnValue = ''; } });
async function save() {
  const p = patch();
  if (!p.n) return;
  $('#btnSave').disabled = true;
  try {
    const r = await api('/api/config', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(p.doc) });
    S.previewed = {};
    await loadConfig();
    renderAll();
    loadStatus();
    toast(r.warnings && r.warnings.length ? 'Saved with ' + r.warnings.length + ' warning(s), see the top of the page' : 'Saved (revision ' + r.revision + ')', 'ok');
    showWarnings(r.warnings || []);
  } catch (e) { toast('Not saved: ' + e.message, 'err'); }
  $('#btnSave').disabled = false;
}
async function discard() {
  const back = Object.keys(S.previewed);
  S.draft = clone(S.cfg);
  for (const path of back) { try { await post('/api/action', { name: 'preview', path: path, value: String(getPath(S.cfg, path)) }); } catch (e) { } }
  S.previewed = {};
  renderAll();
  toast('Changes discarded');
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
// Every optional key present on both copies, so a missing key never counts as a change.
function normalise(doc) {
  doc.pads = doc.pads || {}; doc.pads.roles = doc.pads.roles || ['level', 'sound', 'sound', 'sound'];
  const n = doc.pads.roles.filter(r => r === 'sound').length;
  doc.levels = (doc.levels || []).map(l => {
    const b = (l.buttons || []).map(fillEntry);
    while (b.length < n) b.push(fillEntry());
    return Object.assign({ name: '', jacks: true }, l, { buttons: b.slice(0, n) });
  });
  if (!doc.levels.length) doc.levels.push({ name: '', buttons: Array.from({ length: n }, () => fillEntry()), jacks: true });
  doc.audio = doc.audio || {}; doc.audio.cues = Object.assign({ startup: '', click: '', saved: '', lowBattery: '' }, doc.audio.cues || {});
  doc.device = doc.device || {}; doc.device.ownerLabel = doc.device.ownerLabel || [];
  doc.levelChange = doc.levelChange || {}; doc.levelChange.vibration = doc.levelChange.vibration || {}; doc.levelChange.vibration.pattern = doc.levelChange.vibration.pattern || [];
  doc.hardware = doc.hardware || {}; doc.hardware.padChannels = doc.hardware.padChannels || [2, 3, 4, 5];
  return doc;
}
async function loadConfig() { S.cfg = normalise(await api('/api/config')); S.draft = clone(S.cfg); }
async function loadSounds() { const r = await api('/api/sounds'); S.sounds = r.sounds || []; S.soundsMeta = r; }
async function loadStatus() {
  try { S.status = await api('/api/status'); renderStatus(); }
  catch (e) { $('#statusTiles').innerHTML = ''; $('#statusTiles').appendChild(el('div', { text: 'The board is not answering: ' + e.message })); }
}
function showWarnings(list) {
  const w = $('#warnings'); w.innerHTML = '';
  if (!list.length) { w.hidden = true; return; }
  w.appendChild(el('div', { html: '<b>Configuration warnings</b>' }));
  list.forEach(t => w.appendChild(el('div', { text: t })));
  w.hidden = false;
}
// The phone's sign-in sheet (§15.3, CP-10): a landing card only. It cannot pick files and iOS holds the browser
// back until the sheet is dismissed, so the sheet just says how to get to the real page.
function renderLanding() {
  const main = $('main'); main.innerHTML = '';
  $('#nav').hidden = true;
  const ua = navigator.userAgent, ios = /iPhone|iPad|iPod/.test(ua);
  main.appendChild(el('section', {}, [
    el('h2', { text: 'SoundBoard setup' }),
    el('p', { html: 'You are connected to the board. This is the phone\'s sign-in sheet, which cannot show the whole settings page.' }),
    el('ol', { class: 'steps' }, [
      el('li', { html: ios ? 'Tap <b>Done</b> at the top, then choose <b>Use Without Internet</b>.' : 'Choose <b>Use this network as is</b> (or tap Done / Continue).' }),
      el('li', { html: 'Open your browser and go to <b>http://192.168.4.1</b>' })
    ]),
    el('p', { class: 'muted', html: 'The board shows the same address on its screen. Setup turns itself off after ' + '10 minutes without a change.' }),
    el('p', {}, [el('a', { href: '#', class: 'muted', text: 'Show the full page here anyway', onclick: e => { e.preventDefault(); try { sessionStorage.setItem('sbFull', '1'); } catch (x) { } S.captive = false; location.reload(); } })])
  ]));
}
async function init() {
  if (S.captive && !sessionStorage.getItem('sbFull')) { renderLanding(); return; }
  const nav = $('#nav');
  SECTIONS.forEach(([id, name]) => nav.appendChild(el('a', { href: '#' + id, text: name })));
  try {
    S.schema = await api('/api/schema');
    await loadConfig();
    await loadSounds();
  } catch (e) { toast('Could not load the board: ' + e.message, 'err'); return; }
  $('#hdrVer').textContent = S.schema.version || '';
  renderAll();
  bindStatic();
  await loadStatus();
  S.statusTimer = setInterval(loadStatus, 5000);
  loadFirmware();
  setInterval(() => { if (!(S.fw && FW_BUSY.includes(S.fw.job))) loadFirmware(); }, 15000);
  if ('IntersectionObserver' in window) {
    const io = new IntersectionObserver(es => es.forEach(en => { if (en.isIntersecting) startDiag(); else stopDiag(); }), { threshold: 0.05 });
    io.observe($('#diag'));
  } else startDiag();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
function renderAll() {
  $('#hdrName').textContent = S.draft.device && S.draft.device.name ? S.draft.device.name : '';
  $$('section[data-group]').forEach(sec => renderGroup(sec));
  renderLevels(); renderSounds(); renderCues(); renderOwnerLabel(); renderPattern(); renderJacks();
  updateDirty();
}
function control(r) {
  const v = valOf(r); let c;
  const commit = val => {
    setPath(S.draft, r.path, val); updateDirty();
    if (r.path.startsWith('display.') && (r.path === 'display.theme' || r.path === 'display.flip' || r.path === 'display.brightnessPct' || r.path === 'display.dimPct')) {
      S.previewed[r.path] = true; post('/api/action', { name: 'preview', path: r.path, value: String(val) }).catch(e => toast(e.message, 'err'));
    }
  };
  if (r.type === 'bool') { c = el('input', { type: 'checkbox' }); c.checked = !!v; c.addEventListener('change', () => commit(c.checked)); }
  else if (r.type === 'enum') { c = el('select', {}, (r.enums || []).map(o => el('option', { value: o, text: o }))); c.value = v; c.addEventListener('change', () => commit(c.value)); }
  else if (r.type === 'string') {
    c = el('input', { type: 'text', maxlength: r.maxLen || 64, autocapitalize: 'off', autocorrect: 'off', spellcheck: 'false' }); c.value = v;
    c.addEventListener('change', () => { if (r.min && c.value.length < r.min) { toast(r.label + ': at least ' + r.min + ' characters', 'err'); c.value = v; return; } commit(c.value); });
  } else {
    c = el('input', { type: 'number', min: r.zeroOff ? 0 : r.min, max: r.max, step: r.step, inputmode: 'decimal' }); c.value = v;
    c.addEventListener('change', () => {
      let x = Number(c.value); if (isNaN(x)) { c.value = v; return; }
      if (!(r.zeroOff && x === 0)) { if (x < r.min) x = r.min; if (x > r.max) x = r.max; }
      if (r.type !== 'f32') x = Math.round(x);
      c.value = x; commit(x);
    });
  }
  return c;
}
function settingRow(r) {
  const lab = el('label', { text: r.label });
  const notes = [];
  if (r.reboot) notes.push('takes effect after a restart');
  if (r.path === 'setup.password') notes.push('8 to 24 characters; shown on the board\'s setup card');
  if (notes.length) lab.appendChild(el('span', { class: 'note', text: notes.join('; ') }));
  return el('div', { class: 'row' }, [lab, control(r)]);
}
function renderGroup(sec) {
  const groups = sec.dataset.group.split(',');
  const rows = S.schema.settings.filter(r => groups.includes(r.group));
  const box = $('.rows', sec); box.innerHTML = '';
  rows.filter(r => !r.advanced).forEach(r => box.appendChild(settingRow(r)));
  const adv = rows.filter(r => r.advanced);
  if (adv.length) {
    const d = el('details', { class: 'adv' }, [el('summary', { text: 'Advanced' })]);
    adv.forEach(r => d.appendChild(settingRow(r)));
    box.appendChild(d);
  }
}
function soundSelect(value, withBuiltin) {
  const opts = [el('option', { value: '', text: withBuiltin ? '(built-in tone)' : '(none)' })];
  const names = S.sounds.map(s => s.name);
  if (value && !names.includes(value)) opts.push(el('option', { value: value, text: value + ' (missing)' }));
  names.forEach(n => opts.push(el('option', { value: n, text: n })));
  const s = el('select', {}, opts); s.value = value || ''; return s;
}

// ---- Status ---------------------------------------------------------------
function renderStatus() {
  const st = S.status; if (!st) return;
  const t = $('#statusTiles'); t.innerHTML = '';
  const tile = (k, v) => t.appendChild(el('div', {}, [el('small', { text: k }), el('b', { text: v })]));
  tile('Battery', st.battery.valid ? st.battery.pct + ' %  ' + (st.battery.mv / 1000).toFixed(2) + ' V' + (st.battery.usb ? ' ⚡' : '') : '--');
  tile('Level', st.level.index + ' / ' + st.level.count + (st.level.name ? '  ' + st.level.name : ''));
  tile('Volume', st.volume.muted ? 'MUTE' : st.volume.pct + ' %');
  tile('Card', st.card.mounted ? 'OK, ' + st.card.files + ' sounds' : 'not mounted');
  tile('Configuration', 'from ' + st.card.source + ', rev ' + st.card.revision + (st.card.cardLess ? ' (mirror only)' : ''));
  tile('BT speaker', !st.bt.enabled ? 'off' : st.bt.linked ? 'linked' + (st.bt.peer ? ' to ' + st.bt.peer : '') : st.bt.pairing === 'searching' || st.bt.pairing === 'wiping' ? 'pairing, ' + st.bt.pairLeftS + ' s' : 'no speaker');
  tile('Keyboard', !st.keyboard.enabled ? 'off' : st.keyboard.paused ? 'paused for setup' : st.keyboard.link === 'ready' ? 'tablet connected' : st.keyboard.link);
  tile('Speakers', st.speakers ? 'on' : 'off');
  tile('Firmware', st.version);
  tile('Up for', secs(st.uptimeS) + ', ' + st.reset);
  tile('Setup', st.setup.clients + ' phone' + (st.setup.clients === 1 ? '' : 's') + ', off after ' + st.setup.idleOffMin + ' min idle (' + secs(st.setup.idleS) + ' now)');
  tile('Memory', Math.round(st.heap / 1024) + ' KB free');
  const f = $('#faults'); f.innerHTML = '';
  (st.faults || []).forEach(x => f.appendChild(el('div', { text: x.token + ': ' + x.text })));
  showWarnings(st.warnings || []);
  $('#banner').hidden = !st.setup.recovery;
  const bt = $('#btStatus');
  bt.innerHTML = '';
  bt.appendChild(el('span', { html: !st.bt.enabled ? 'The Bluetooth speaker is <b>off</b>.' :
    st.bt.pairing === 'searching' || st.bt.pairing === 'wiping' ? 'Searching for a speaker in pairing mode, <b>' + st.bt.pairLeftS + ' s</b> left.' :
    st.bt.linked ? 'Linked to <b>' + esc(st.bt.peer || 'a speaker') + '</b>.' : 'Enabled, module ' + esc(st.bt.module) + ', <b>no speaker linked</b>.' }));
  $('#btnPair').disabled = !st.bt.enabled || st.bt.pairing === 'searching' || st.bt.pairing === 'wiping';
  $('#btnForgetSpk').disabled = $('#btnPair').disabled;
  $('#kbdStatus').innerHTML = !st.keyboard.enabled ? 'Typing is <b>off</b>.' : st.keyboard.paused ? 'Paused while setup is on (setup.pauseKeyboard).' :
    (st.keyboard.link === 'ready' ? 'A tablet is <b>connected</b>' : 'Advertising as <b>' + esc(st.name) + '</b>, no tablet connected') + '; ' + st.keyboard.bonds + ' host' + (st.keyboard.bonds === 1 ? '' : 's') + ' remembered.';
  if (st.bt.pairing === 'searching' || st.bt.pairing === 'wiping') { clearInterval(S.statusTimer); S.statusTimer = setInterval(loadStatus, 2000); }
  else if (S.statusTimer && S.statusFast) { clearInterval(S.statusTimer); S.statusTimer = setInterval(loadStatus, 5000); }
  S.statusFast = st.bt.pairing === 'searching' || st.bt.pairing === 'wiping';
  $$('.level').forEach((lv, i) => lv.classList.toggle('current', i === st.level.index - 1));
}
function esc(s) { return String(s == null ? '' : s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
// ---- Firmware (§16) ---------------------------------------------------------------
const FW_BUSY = ['connecting', 'checking', 'downloading', 'verifying', 'writing', 'rebooting'];
async function loadFirmware() {
  let f;
  try { f = await api('/api/firmware/status'); } catch (e) { $('#fwStatus').textContent = e.message; return; }
  S.fw = f;
  $('#fwStatus').innerHTML = 'Running <b>' + esc(f.version) + '</b> from slot ' + esc(f.slot) + ' (' + esc(f.state) + ').' +
    (f.other && f.other.present ? ' The other slot holds <b>' + esc(f.other.version || '?') + '</b> (' + esc(f.other.state) + ').' : '') +
    ' Releases from <b>' + esc(f.repo) + '</b>, ' + esc(f.channel) + '.';
  $('#staStatus').textContent = f.sta.connected ? 'joined ' + f.sta.ssid + ' (' + f.sta.ip + ', ' + f.sta.rssi + ' dBm)' : f.sta.configured ? 'not joined yet' : 'no network set';
  const busy = FW_BUSY.includes(f.job);
  $('#fwJob').innerHTML = f.job === 'idle' ? '' : (f.job === 'failed' ? '<b>Failed:</b> ' + esc(f.error) : esc(f.text || f.job));
  $('#fwBarBox').hidden = !(f.job === 'downloading' || f.job === 'writing' || f.job === 'rebooting');
  $('#fwBar').style.width = f.pct + '%';
  $('#btnFwCancel').hidden = !busy || f.job === 'rebooting' || f.job === 'writing';
  const a = f.available;
  $('#fwAvail').innerHTML = a ? '<b>' + esc(a.version) + '</b>' + (a.same ? ' is what the board runs.' : ' is available (' + kb(a.size) + ').') + (a.notes ? ' ' + esc(a.notes) : '') : '';
  $('#btnFwInstall').hidden = !(a && !busy);
  $('#btnFwInstall').textContent = a ? 'Install ' + a.version : 'Install';
  $('#fwGate').textContent = f.usb ? '' : 'Plug in USB power first: installing needs it.';
  ['btnFwInstall', 'btnFwInstallVer', 'btnFwUpload', 'btnFwRollback'].forEach(id => { $('#' + id).disabled = !f.usb || busy; });
  $('#btnFwCheck').disabled = busy; $('#btnWifiJoin').disabled = busy && f.job !== 'connecting'; $('#btnWifiScan').disabled = busy;
  if (!(f.other && f.other.eligible)) $('#btnFwRollback').disabled = true;
  $('#btnFwRollback').textContent = f.other && f.other.present ? 'Go back to ' + (f.other.version || 'the other slot') : 'Go back to the previous version';
  if (f.job === 'rebooting') { waitForReboot(f.text); return; }
  clearTimeout(S.fwTimer);
  if (busy) S.fwTimer = setTimeout(loadFirmware, 1000);
}
function waitForReboot(text) {
  clearTimeout(S.fwTimer); clearInterval(S.statusTimer); S.statusTimer = null;
  $('#fwJob').innerHTML = '<b>' + esc(text || 'restarting') + '.</b> Stay on the board\'s Wi-Fi; this page reloads when the board is back (about 20 s).';
  const t0 = Date.now();
  const poll = async () => {
    if (Date.now() - t0 > 120000) { $('#fwJob').innerHTML = 'The board has not come back on this network. Rejoin it and reload the page.'; return; }
    try { const r = await fetch('/api/status', { cache: 'no-store' }); if (r.ok) { location.reload(); return; } } catch (e) { }
    setTimeout(poll, 3000);
  };
  setTimeout(poll, 8000);
}
async function fwPost(path, args, okText) {
  try { await post(path, args || {}); if (okText) toast(okText); await loadFirmware(); } catch (e) { toast(e.message, 'err'); loadFirmware(); }
}
function bindFirmware() {
  $('#btnWifiScan').onclick = async () => {
    $('#btnWifiScan').disabled = true; $('#wifiList').innerHTML = '<div class="muted">scanning…</div>';
    try {
      const r = await post('/api/firmware/wifi/scan', {});
      const box = $('#wifiList'); box.innerHTML = '';
      (r.networks || []).sort((a, b) => b.rssi - a.rssi).forEach(n => box.appendChild(el('div', { class: 'item' }, [
        el('span', { class: 'n', text: n.ssid }), el('span', { class: 'muted', text: n.rssi + ' dBm' + (n.open ? ', open' : '') }),
        el('button', { class: 'ghost small', text: 'Use', onclick: () => { const i = $$('#firmware .row input').find(x => x.previousSibling && /Home Wi-Fi network/.test(x.parentNode.textContent)); if (i) { i.value = n.ssid; i.dispatchEvent(new Event('change')); } box.innerHTML = ''; } })
      ])));
      if (!r.networks || !r.networks.length) box.innerHTML = '<div class="muted">no 2.4 GHz networks found</div>';
    } catch (e) { toast(e.message, 'err'); $('#wifiList').innerHTML = ''; }
    $('#btnWifiScan').disabled = false;
  };
  $('#btnWifiJoin').onclick = () => {
    const ssid = (getPath(S.draft, 'wifi.ssid') || '').trim(), pw = getPath(S.draft, 'wifi.password') || '';
    if (!ssid) { toast('Type the home Wi-Fi network name first', 'err'); return; }
    if (patch().n) toast('Save keeps the network for next time');
    fwPost('/api/firmware/wifi/join', { ssid: ssid, password: pw }, 'Joining ' + ssid + ' …');
  };
  $('#btnFwCheck').onclick = () => fwPost('/api/firmware/check', {}, 'Checking …');
  $('#btnFwInstall').onclick = () => { const a = S.fw && S.fw.available; if (a && confirm('Install ' + a.version + '? The board restarts when it is written; sounds stop meanwhile.')) fwPost('/api/firmware/install', { version: a.version }); };
  $('#btnFwInstallVer').onclick = () => { const v = $('#fwVersion').value.trim(); if (!v) { toast('Type a version like v0.11.0'); return; } if (confirm('Install ' + v + '?')) fwPost('/api/firmware/install', { version: v }); };
  $('#btnFwCancel').onclick = () => fwPost('/api/firmware/cancel', {}, 'Cancelled');
  $('#btnFwRollback').onclick = () => { if (confirm('Go back to the other slot\'s firmware? The board restarts.')) fwPost('/api/firmware/rollback', {}); };
  $('#btnFwUpload').onclick = () => {
    const f = $('#fwFile').files[0]; if (!f) { toast('Choose a .bin first'); return; }
    if (!confirm('Install ' + f.name + ' (' + kb(f.size) + ')? The board restarts when it is written.')) return;
    const fd = new FormData(); fd.append('file', f, f.name);
    const xhr = new XMLHttpRequest(); xhr.open('POST', '/api/firmware/upload');
    $('#fwBarBox').hidden = false; $('#fwJob').textContent = 'sending ' + f.name + ' …';
    xhr.upload.onprogress = e => { if (e.lengthComputable) $('#fwBar').style.width = Math.round(e.loaded * 100 / e.total) + '%'; };
    xhr.onload = () => { let r = null; try { r = JSON.parse(xhr.responseText); } catch (e) { } if (xhr.status === 200) loadFirmware(); else { toast('Not installed: ' + ((r && r.error) || ('HTTP ' + xhr.status)), 'err'); loadFirmware(); } };
    xhr.onerror = () => { toast('Upload failed (connection lost?)', 'err'); loadFirmware(); };
    xhr.send(fd);
  };
}

// ---- Levels ---------------------------------------------------------------
function fillEntry(e) { const o = Object.assign({}, ENTRY_DEF, e || {}); if (o.goToLevel == null) o.goToLevel = 1; return o; }
function normaliseLevels() { normalise(S.draft); }
function remapGoto(fn) { S.draft.levels.forEach(l => l.buttons.forEach(b => { if (b.action === 'goToLevel') b.goToLevel = fn(b.goToLevel || 1); })); }
function renderLevels() {
  normaliseLevels();
  const box = $('#levelsList'); box.innerHTML = '';
  const pos = soundPositions();
  const cur = S.status ? S.status.level.index - 1 : -1;
  S.draft.levels.forEach((l, i) => {
    const name = el('input', { type: 'text', maxlength: S.schema.limits.levelName, placeholder: 'name shown on the board', value: l.name || '' });
    name.addEventListener('change', () => { l.name = name.value; updateDirty(); });
    const head = el('div', { class: 'lvhead' }, [
      el('b', { text: 'Level ' + (i + 1) }), name,
      el('button', { class: 'ghost small', text: '▲', title: 'move up', onclick: () => moveLevel(i, i - 1) }),
      el('button', { class: 'ghost small', text: '▼', title: 'move down', onclick: () => moveLevel(i, i + 1) }),
      el('button', { class: 'ghost small', text: 'Set as current', onclick: () => action('setLevel', { level: i + 1 }).then(loadStatus) }),
      el('button', { class: 'danger small', text: 'Delete', onclick: () => deleteLevel(i) })
    ]);
    const card = el('div', { class: 'level' + (i === cur ? ' current' : '') }, [head]);
    l.buttons.forEach((b, bi) => card.appendChild(entryEditor(l, b, bi, pos[bi], i)));
    const vib = el('input', { type: 'text', placeholder: 'blank = the level number as pulses', value: (l.vibration || []).join(',') });
    vib.addEventListener('change', () => { const p = parsePattern(vib.value); if (p === null) { toast('Pattern: numbers in ms separated by commas', 'err'); return; } if (p.length) l.vibration = p; else delete l.vibration; updateDirty(); });
    card.appendChild(el('div', { class: 'field' }, [el('label', { text: 'This level\'s vibration pattern (on, off, on … ms)' }),
      el('div', { class: 'inline' }, [vib, el('button', { class: 'ghost small', text: 'Buzz', onclick: () => { const p = parsePattern(vib.value); if (p && p.length) action('buzz', { pattern: p.join(',') }); else action('buzz', { level: i + 1 }); } })])]));
    const jk = el('input', { type: 'checkbox' }); jk.checked = l.jacks !== false; jk.addEventListener('change', () => { l.jacks = jk.checked; updateDirty(); });
    card.appendChild(el('label', { class: 'check' }, [jk, 'jacks close on this level']));
    box.appendChild(card);
  });
}
function parsePattern(s) {
  s = s.trim(); if (!s) return [];
  const parts = s.split(/[,\s]+/).filter(x => x !== '');
  const out = []; for (const p of parts) { const n = Number(p); if (!isFinite(n) || n < 0) return null; out.push(Math.round(n)); }
  return out.slice(0, S.schema.limits.maxPattern);
}
function entryEditor(level, b, bi, pos, li) {
  const f = (labelText, ctrl, wide) => el('div', { class: 'f' + (wide ? ' wide' : '') }, [el('label', { text: labelText }), ctrl]);
  const sound = soundSelect(b.sound, false); sound.addEventListener('change', () => { b.sound = sound.value; updateDirty(); });
  const label = el('input', { type: 'text', maxlength: S.schema.limits.label, placeholder: b.sound ? b.sound.replace(/\.wav$/i, '') : 'label', value: b.label || '' });
  label.addEventListener('change', () => { b.label = label.value; updateDirty(); });
  const typed = el('input', { type: 'text', maxlength: S.schema.limits.typed, placeholder: 'text typed to the tablet', value: b.type || '' });
  typed.addEventListener('change', () => { b.type = typed.value; updateDirty(); });
  const key = el('select', {}, [el('option', { value: '', text: '(no key)' })].concat(S.schema.keys.map(k => el('option', { value: k, text: k }))));
  key.value = (b.key || '').toUpperCase();
  const mode = el('select', {}, [el('option', { value: 'hold', text: 'held while the pad is held' }), el('option', { value: 'tap', text: 'tapped once' })]);
  mode.value = b.keyMode === 'tap' ? 'tap' : 'hold';
  const modeBox = f('Key sent', mode);
  const syncMode = () => { modeBox.hidden = !key.value; };
  key.addEventListener('change', () => { b.key = key.value; b.keyMode = key.value ? mode.value : 'type'; syncMode(); updateDirty(); });
  mode.addEventListener('change', () => { b.keyMode = mode.value; updateDirty(); });
  syncMode();
  const act = el('select', {}, S.schema.actions.map(a => el('option', { value: a, text: actionLabel(a) })));
  act.value = b.action || 'none';
  const gotoSel = el('select', {}, S.draft.levels.map((l, i) => el('option', { value: i + 1, text: 'Level ' + (i + 1) + (l.name ? ' ' + l.name : '') })));
  gotoSel.value = b.goToLevel || 1;
  const gotoBox = f('Go to', gotoSel);
  const syncGoto = () => { gotoBox.hidden = act.value !== 'goToLevel'; };
  act.addEventListener('change', () => { b.action = act.value; syncGoto(); updateDirty(); });
  gotoSel.addEventListener('change', () => { b.goToLevel = Number(gotoSel.value); updateDirty(); });
  syncGoto();
  const vol = el('input', { type: 'number', min: 0, max: 100, step: 5, value: b.volumePct == null ? 100 : b.volumePct });
  vol.addEventListener('change', () => { b.volumePct = Math.max(0, Math.min(100, Math.round(Number(vol.value) || 0))); vol.value = b.volumePct; updateDirty(); });
  const vib = el('select', {}, [el('option', { value: '', text: 'as the Vibration setting' }), el('option', { value: 'true', text: 'pulse on this pad' }), el('option', { value: 'false', text: 'no pulse' })]);
  vib.value = b.vibrate === true ? 'true' : b.vibrate === false ? 'false' : '';
  vib.addEventListener('change', () => { b.vibrate = vib.value === '' ? null : vib.value === 'true'; updateDirty(); });
  const jack = el('select', {}, [el('option', { value: '', text: 'as the Jacks setting' })].concat(S.schema.jackModes.map(j => el('option', { value: j, text: j }))));
  jack.value = b.jack || '';
  jack.addEventListener('change', () => { b.jack = jack.value || null; updateDirty(); });
  const play = el('button', { class: 'ghost small', text: 'Play', onclick: () => { if (b.sound) post('/api/play', { name: b.sound }).catch(e => toast(e.message, 'err')); else toast('No sound on this pad'); } });
  const head = el('div', { class: 'ehead' }, [el('b', { text: 'P' + (pos + 1) }), el('span', { class: 'lbl', text: 'pad ' + (pos + 1) + ' from the left' }), play]);
  const adv = el('details', { class: 'adv' }, [el('summary', { text: 'More' }), el('div', { class: 'grid2' }, [f('Volume (% of the master)', vol), f('Vibration', vib), f('Jack', jack)])]);
  return el('div', { class: 'entry' }, [head, el('div', { class: 'grid2' }, [f('Sound', sound), f('Label on the screen', label), f('Typed text', typed, true), f('Or a single key', key), modeBox, f('Action', act), gotoBox]), adv]);
}
function actionLabel(a) { return { none: 'none', volumeUp: 'volume up', volumeDown: 'volume down', mute: 'mute / unmute', nextLevel: 'next level', previousLevel: 'previous level', goToLevel: 'go to a level' }[a] || a; }
function moveLevel(i, j) {
  const L = S.draft.levels; if (j < 0 || j >= L.length) return;
  const t = L[i]; L[i] = L[j]; L[j] = t;
  remapGoto(g => (g === i + 1 ? j + 1 : g === j + 1 ? i + 1 : g));
  renderLevels(); updateDirty();
}
function deleteLevel(i) {
  const L = S.draft.levels;
  if (L.length === 1) { toast('The board needs at least one level'); return; }
  if (!confirm('Delete level ' + (i + 1) + (L[i].name ? ' "' + L[i].name + '"' : '') + '?')) return;
  L.splice(i, 1);
  remapGoto(g => (g === i + 1 ? 1 : g > i + 1 ? g - 1 : g));
  renderLevels(); updateDirty();
}
function addLevel() {
  if (S.draft.levels.length >= S.schema.limits.maxLevels) { toast('At most ' + S.schema.limits.maxLevels + ' levels'); return; }
  S.draft.levels.push({ name: '', buttons: Array.from({ length: nSoundPads() }, () => fillEntry()), jacks: true });
  renderLevels(); updateDirty();
  const cards = $$('.level'); cards[cards.length - 1].scrollIntoView({ behavior: 'smooth' });
}

// ---- Sounds ---------------------------------------------------------------
function renderSounds() {
  const box = $('#soundList'); box.innerHTML = '';
  const meta = S.soundsMeta || {};
  $('#soundsHelp').textContent = meta.cardMounted === false ? 'No card is mounted: sounds cannot be added.' : S.sounds.length + ' sound' + (S.sounds.length === 1 ? '' : 's') + ' on the card. A recording from the phone is converted here (mono, 44.1 kHz, 16-bit) before it is sent.';
  if (S.captive) { $('#btnAddSound').hidden = true; $('#soundHint').textContent = 'This sign-in sheet cannot pick files: open Safari (or Chrome) at http://192.168.4.1 to add sounds.'; }
  S.sounds.forEach(s => {
    const bad = s.state === 'bad' || s.state === 'missing';
    const info = s.seconds + ' s, ' + s.channels + ' ch, ' + (s.rate / 1000).toFixed(1) + ' kHz, ' + kb(s.bytes) + (bad ? ' — ' + (s.reason || s.state) : '') + (s.usedBy && s.usedBy.length ? ' · used by ' + s.usedBy.join(', ') : ' · not used');
    box.appendChild(el('div', { class: 'item' }, [
      el('span', { class: 'n', text: s.name }),
      el('button', { class: 'ghost small', text: 'Play', onclick: () => post('/api/play', { name: s.name }).catch(e => toast(e.message, 'err')) }),
      el('button', { class: 'ghost small', text: 'Rename', onclick: () => renameSound(s) }),
      el('button', { class: 'danger small', text: 'Delete', onclick: () => deleteSound(s) }),
      el('span', { class: 'i' + (bad ? ' bad' : ''), text: info })
    ]));
  });
}
async function refreshSounds() { await loadSounds(); renderSounds(); renderLevels(); renderCues(); }
async function deleteSound(s) {
  const used = s.usedBy && s.usedBy.length;
  if (!confirm('Delete ' + s.name + '?' + (used ? ' It is used by ' + s.usedBy.join(', ') + '; those pads go blank and silent.' : ''))) return;
  try { await post('/api/sounds/delete', { name: s.name }); toast('Deleted ' + s.name, 'ok'); await loadConfig(); await refreshSounds(); updateDirty(); } catch (e) { toast(e.message, 'err'); }
}
async function renameSound(s) {
  let to = prompt('New name for ' + s.name, s.name); if (!to) return;
  to = to.trim(); if (!/\.wav$/i.test(to)) to += '.wav';
  if (!/^[A-Za-z0-9._-]{5,40}$/.test(to)) { toast('Letters, digits, . _ - only, ending in .wav', 'err'); return; }
  try { await post('/api/sounds/rename', { name: s.name, to: to }); toast('Renamed', 'ok'); await loadConfig(); await refreshSounds(); updateDirty(); } catch (e) { toast(e.message, 'err'); }
}
async function addSound(file) {
  const box = $('#uploadBox'), txt = $('#uploadText'), bar = $('#uploadBar');
  box.hidden = false; bar.style.width = '0%'; txt.textContent = 'Converting ' + file.name + ' …'; $('#btnCancelUpload').disabled = true;
  let wav;
  try { wav = await convertToWav(file); }
  catch (e) { box.hidden = true; toast(e.message, 'err'); return; }
  let name = file.name.replace(/\.[^.]+$/, '').replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, 36) || 'sound';
  name = prompt('Name for this sound (' + wav.seconds.toFixed(1) + ' s):', name);
  if (!name) { box.hidden = true; return; }
  name = name.trim().replace(/\.wav$/i, '').replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, 36) + '.wav';
  if (S.sounds.some(s => s.name.toLowerCase() === name.toLowerCase())) { box.hidden = true; toast('A sound called ' + name + ' exists: delete or rename it first', 'err'); return; }
  txt.textContent = 'Sending ' + name + ' (' + kb(wav.blob.size) + ') …'; $('#btnCancelUpload').disabled = false;
  const fd = new FormData(); fd.append('file', wav.blob, name);
  const xhr = new XMLHttpRequest(); S.upload = xhr;
  xhr.open('POST', '/api/sounds/upload');
  xhr.upload.onprogress = e => { if (e.lengthComputable) bar.style.width = Math.round(e.loaded * 100 / e.total) + '%'; };
  xhr.onload = async () => {
    S.upload = null; box.hidden = true;
    let r = null; try { r = JSON.parse(xhr.responseText); } catch (e) { }
    if (xhr.status === 200 && r && r.ok) { toast('Added ' + r.name + ' (' + r.info + ')', 'ok'); await refreshSounds(); }
    else toast('Not added: ' + ((r && r.error) || ('HTTP ' + xhr.status)), 'err');
  };
  xhr.onerror = () => { S.upload = null; box.hidden = true; toast('Upload failed (connection lost?)', 'err'); refreshSounds(); };
  xhr.onabort = () => { S.upload = null; box.hidden = true; toast('Upload cancelled'); refreshSounds(); };
  xhr.send(fd);
}
async function convertToWav(file) {
  const AC = window.AudioContext || window.webkitAudioContext, OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
  if (!AC || !OAC) throw new Error('This browser cannot convert audio; export the recording as WAV first.');
  const buf = await file.arrayBuffer();
  const ctx = new AC();
  let decoded;
  try {
    decoded = await new Promise((res, rej) => { const p = ctx.decodeAudioData(buf, res, rej); if (p && p.then) p.then(res, rej); });
  } catch (e) { throw new Error('Format not supported, export as MP3 or WAV.'); }
  finally { if (ctx.close) ctx.close().catch(() => { }); }
  if (decoded.duration > S.schema.limits.recordingS) throw new Error('The recording is ' + decoded.duration.toFixed(0) + ' s; the limit is ' + S.schema.limits.recordingS + ' s.');
  const rate = 44100, frames = Math.max(1, Math.ceil(decoded.duration * rate));
  const off = new OAC(1, frames, rate);
  const src = off.createBufferSource(); src.buffer = decoded; src.connect(off.destination); src.start(0);
  const rendered = await new Promise((res, rej) => { off.oncomplete = e => res(e.renderedBuffer); const p = off.startRendering(); if (p && p.then) p.then(res, rej); });
  const d = rendered.getChannelData(0);
  const thr = Math.pow(10, -50 / 20), keep = Math.round(0.02 * rate);
  let a = 0, b = d.length - 1;
  while (a < d.length && Math.abs(d[a]) < thr) a++;
  while (b > a && Math.abs(d[b]) < thr) b--;
  if (a >= b) throw new Error('The recording is silent.');
  a = Math.max(0, a - keep); b = Math.min(d.length - 1, b + keep);
  let peak = 0; for (let i = a; i <= b; i++) { const v = Math.abs(d[i]); if (v > peak) peak = v; }
  const gain = peak > 0 ? Math.pow(10, -1 / 20) / peak : 1;
  const n = b - a + 1, out = new DataView(new ArrayBuffer(44 + n * 2));
  const str = (o, s) => { for (let i = 0; i < s.length; i++) out.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); out.setUint32(4, 36 + n * 2, true); str(8, 'WAVE'); str(12, 'fmt '); out.setUint32(16, 16, true);
  out.setUint16(20, 1, true); out.setUint16(22, 1, true); out.setUint32(24, rate, true); out.setUint32(28, rate * 2, true); out.setUint16(32, 2, true); out.setUint16(34, 16, true);
  str(36, 'data'); out.setUint32(40, n * 2, true);
  for (let i = 0; i < n; i++) { let v = d[a + i] * gain; if (v > 1) v = 1; if (v < -1) v = -1; out.setInt16(44 + i * 2, v < 0 ? v * 32768 : v * 32767, true); }
  return { blob: new Blob([out.buffer], { type: 'audio/wav' }), seconds: n / rate };
}

// ---- Cues, owner label, pattern, jacks ---------------------------------------
function renderCues() {
  const box = $('#cues'); box.innerHTML = '';
  const cues = S.draft.audio.cues;
  [['startup', 'Start-up sound'], ['click', 'Click (level change, volume)'], ['saved', 'Saved'], ['lowBattery', 'Low battery (blank = none)']].forEach(([k, label]) => {
    const sel = soundSelect(cues[k], k !== 'lowBattery'); sel.addEventListener('change', () => { cues[k] = sel.value; updateDirty(); });
    const play = el('button', { class: 'ghost small', text: 'Play', onclick: () => post('/api/play', { cue: k }).catch(e => toast(e.message, 'err')) });
    box.appendChild(el('div', { class: 'row' }, [el('label', { text: label }), sel, play]));
  });
}
function renderOwnerLabel() {
  const box = $('#ownerLabel'); box.innerHTML = '';
  const lines = S.draft.device.ownerLabel = (S.draft.device.ownerLabel || []).slice(0, S.schema.limits.ownerLines);
  for (let i = 0; i < S.schema.limits.ownerLines; i++) {
    const inp = el('input', { type: 'text', maxlength: S.schema.limits.ownerLine, placeholder: i === 0 ? 'If found please call' : i === 1 ? '(phone number)' : '', value: lines[i] || '' });
    inp.addEventListener('change', () => { const arr = []; $$('#ownerLabel input').forEach(x => arr.push(x.value)); while (arr.length && !arr[arr.length - 1]) arr.pop(); S.draft.device.ownerLabel = arr; updateDirty(); });
    box.appendChild(el('div', { class: 'row' }, [el('label', { text: 'Line ' + (i + 1) }), inp]));
  }
}
function renderPattern() {
  const inp = $('#vibPattern');
  inp.value = ((S.draft.levelChange.vibration || {}).pattern || []).join(',');
  inp.onchange = () => { const p = parsePattern(inp.value); if (p === null) { toast('Pattern: numbers in ms separated by commas', 'err'); return; } setPath(S.draft, 'levelChange.vibration.pattern', p); updateDirty(); };
}
function renderJacks() {
  const ch = S.draft.hardware.padChannels || [2, 3, 4, 5];
  $('#jackMap').innerHTML = ch.map((c, i) => 'P' + (i + 1) + ' → J' + (c - 1)).join(' · ');
  const t = $('#jackTests'); t.innerHTML = '';
  for (let j = 1; j <= 4; j++) t.appendChild(el('button', { class: 'ghost small', text: 'Close J' + j + ' for 1 s', onclick: () => action('closeJack', { jack: j }) }));
}

// ---- Diagnostics ---------------------------------------------------------------
function startDiag() { if (S.diagTimer) return; pollDiag(); S.diagTimer = setInterval(pollDiag, S.identify ? 250 : 1000); }
function stopDiag() { if (S.identify) return; clearInterval(S.diagTimer); S.diagTimer = null; }
async function pollDiag() {
  try { S.diag = await api('/api/diag'); } catch (e) { return; }
  renderDiag();
  if (S.identify) identifyStep();
}
function renderDiag() {
  const d = S.diag; if (!d) return;
  const p = $('#diagPads'); p.innerHTML = '';
  d.pads.forEach(x => p.appendChild(el('div', { class: 'pad' + (x.pressed ? ' on' : '') + (x.stuck ? ' stuck' : '') }, [
    el('small', { text: 'P' + x.pos + ' · ch ' + x.ch + ' · ' + x.role }), el('b', { text: (x.delta >= 0 ? '+' : '') + Number(x.delta).toFixed(2) + ' %' }), el('small', { text: x.stuck ? 'stuck' : x.pressed ? 'pressed' : 'raw ' + x.raw })
  ])));
  const t = $('#diagInfo'); t.innerHTML = '';
  const tile = (k, v) => t.appendChild(el('div', {}, [el('small', { text: k }), el('b', { text: v })]));
  tile('Touch', d.touch);
  tile('Cache', d.cache.cached + ' of ' + d.cache.count + ' cached, ' + Math.round(d.cache.usedKB / 1024) + ' of ' + Math.round(d.cache.budgetKB / 1024) + ' MB' + (d.cache.loader ? ', loading' : ''));
  tile('Audio', d.audio.rail + (d.audio.playing ? ', playing' : '') + ' · underruns ' + d.audio.underruns + ', starved ' + d.audio.starved + ', stalls ' + d.audio.stalls);
  tile('Battery', d.battery.method + ' K ' + d.battery.k + ' · ' + d.battery.mv + ' mV ' + d.battery.pct + ' %' + (d.battery.usb ? ' ⚡' : ''));
  tile('BT module', d.kcx.module + ', ' + d.kcx.link + ' · last: ' + (d.kcx.last || '—'));
  tile('Reset', d.reset + ' · boot #' + d.bootCount + ', crashes ' + d.crashes);
  tile('Memory', Math.round(d.heap / 1024) + ' KB free (min ' + Math.round(d.heapMin / 1024) + '), PSRAM ' + Math.round(d.psram / 1024) + ' KB');
  tile('Stacks', 'app min ' + d.appStackMin + ' B, net min ' + d.netStackMin + ' B');
}
function startIdentify() {
  if (!S.diag) { toast('Wait for the pad readings'); return; }
  S.identify = { step: 0, seq: S.diag.pad.seq, chans: [] };
  clearInterval(S.diagTimer); S.diagTimer = setInterval(pollDiag, 250);
  renderIdentify();
}
function identifyStep() {
  const I = S.identify, d = S.diag;
  if (d.pad.seq !== I.seq) {
    I.seq = d.pad.seq;
    if (I.step < 4) { I.chans.push(d.pad.ch); I.step++; }
    renderIdentify();
  }
}
function renderIdentify() {
  const I = S.identify, box = $('#identify'); box.hidden = false; box.innerHTML = '';
  if (I.step < 4) {
    box.appendChild(el('div', { html: 'Touch the pads one at a time, left to right. <b>Now touch pad ' + (I.step + 1) + '</b>' + (I.step ? ' — so far: ' + I.chans.map((c, i) => 'P' + (i + 1) + ' is channel ' + c).join(', ') : '') + '.' }));
    box.appendChild(el('div', { class: 'row-btns' }, [el('button', { class: 'ghost small', text: 'Stop', onclick: stopIdentify })]));
    return;
  }
  const ok = new Set(I.chans).size === 4 && I.chans.every(c => c >= 2 && c <= 5);
  box.appendChild(el('div', { html: I.chans.map((c, i) => 'P' + (i + 1) + ' is channel ' + c).join(', ') + '.' + (ok ? ' Save this pad order?' : ' <b>The same channel was seen twice: try again.</b>') }));
  box.appendChild(el('div', { class: 'row-btns' }, [
    ok ? el('button', { text: 'Save the pad order', onclick: () => { setPath(S.draft, 'hardware.padChannels', I.chans.slice()); stopIdentify(); renderJacks(); updateDirty(); save(); } }) : null,
    el('button', { class: 'ghost', text: ok ? 'Cancel' : 'Try again', onclick: () => { if (ok) stopIdentify(); else startIdentify(); } })
  ]));
}
function stopIdentify() { S.identify = null; $('#identify').hidden = true; clearInterval(S.diagTimer); S.diagTimer = setInterval(pollDiag, 1000); }
async function loadLog() { try { $('#logTail').textContent = await api('/api/log?tail=200'); const l = $('#logTail'); l.scrollTop = l.scrollHeight; } catch (e) { toast(e.message, 'err'); } }

// ---- Static buttons ---------------------------------------------------------------
function bindStatic() {
  $('#btnSave').onclick = save;
  $('#btnDiscard').onclick = discard;
  $('#btnAddLevel').onclick = addLevel;
  $('#btnAddSound').onclick = () => $('#soundFile').click();
  $('#soundFile').onchange = e => { const f = e.target.files[0]; e.target.value = ''; if (f) addSound(f); };
  $('#btnCancelUpload').onclick = () => { if (S.upload) S.upload.abort(); };
  $('#btnPair').onclick = () => action('pairSpeaker').then(() => { toast('Put the speaker in pairing mode; searching for 60 s'); loadStatus(); });
  $('#btnForgetSpk').onclick = () => { if (confirm('Forget every speaker the module has saved, then search for one in pairing mode?')) action('forgetSpeakers').then(loadStatus); };
  $('#btnForgetHosts').onclick = () => { if (confirm('Forget every tablet or phone paired for typing? Forget the board on the tablet too, then pair again.')) action('forgetHosts').then(() => { toast('Keyboard hosts forgotten', 'ok'); loadStatus(); }); };
  $('#btnBuzz').onclick = () => action('buzz');
  $('#btnBuzzPattern').onclick = () => { const p = parsePattern($('#vibPattern').value); if (p && p.length) action('buzz', { pattern: p.join(',') }); else toast('Type a pattern first'); };
  $('#btnWifiOff').onclick = () => { if (confirm('Turn off setup? The phone leaves the board\'s network and this page closes.')) action('wifiOff').then(() => toast('Setup is turning off')); };
  const reset = () => { if (confirm('Put every setting back to its default? The levels, sounds, pad order, owner label and Wi-Fi are kept.')) action('resetSettings').then(async () => { toast('Settings reset', 'ok'); await loadConfig(); renderAll(); }); };
  $('#btnReset').onclick = reset; $('#btnResetRecovery').onclick = reset;
  $('#btnCfgUpload').onclick = async () => {
    const f = $('#cfgFile').files[0]; if (!f) { toast('Choose a config.json first'); return; }
    if (patch().n && !confirm('Unsaved changes on this page are lost. Continue?')) return;
    const fd = new FormData(); fd.append('file', f, 'config.json'); fd.append('fromThisBoard', $('#cfgFromBoard').checked ? '1' : '0');
    try { const r = await api('/api/config/upload', { method: 'POST', body: fd }); toast('Configuration uploaded (revision ' + r.revision + ')' + (r.warnings.length ? ', ' + r.warnings.length + ' warning(s)' : ''), 'ok'); showWarnings(r.warnings || []); await loadConfig(); await refreshSounds(); renderAll(); }
    catch (e) { toast('Not applied: ' + e.message, 'err'); }
  };
  $('#btnIdentify').onclick = startIdentify;
  $('#btnRecal').onclick = () => { if (confirm('Recalibrate the pads? Keep hands off the board for 3 s.')) action('recalibrate').then(() => toast('Recalibrating: hands off the pads')); };
  $('#btnBattK').onclick = () => { const v = Number($('#battVolts').value); if (!(v >= 3 && v <= 4.5)) { toast('A meter reading between 3 and 4.5 V', 'err'); return; } action('setBatteryK', { volts: v }).then(pollDiag); };
  $('#btnBattClear').onclick = () => { if (confirm('Use the design divider instead of the calibrated K?')) action('setBatteryK', { volts: 0 }).then(pollDiag); };
  $('#btnLog').onclick = loadLog;
  bindFirmware();
  loadLog();
}
document.addEventListener('DOMContentLoaded', init);
