import { t, setLang, getLang } from './i18n.js';
import { S, saveAll } from './store.js';
import * as L from './local.js';
import { Remote } from './remote.js';

const $ = (s, r = document) => r.querySelector(s);
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const barColor = p => (p < 30 ? '#f44336' : (p <= 65 ? '#ffeb3b' : '#4caf50'));
const root = $('#app');
const remote = new Remote();

const A = {
  mode: 'none',            // local | remote | none
  view: null, stale: false, rstatus: 'offline',
  screen: 'main', stack: [],
  cfg: null, thr: 50,
  fails: 0, poll: null, watch: null, lastSig: '', busy: false,
};

// ---------------------------------------------------------------- ابزارها
function toast(msg, kind = '') {
  const el = $('#toast');
  el.textContent = msg; el.className = 'toast show ' + kind;
  clearTimeout(toast.t); toast.t = setTimeout(() => { el.className = 'toast'; }, 2800);
}
const outName = i => (S.names[i] && S.names[i].length) ? S.names[i] : t('out' + i);
const errText = e => {
  if (!e) return t('saveFailed');
  if (e.code === 'unauthorized') return t('unauthorized');
  if (e.code === 'net' || e.code === 'timeout') return t('netErr');
  if (e.code === 'duplicate') return t('dup');
  return t('saveFailed');
};
function haversine(la1, lo1, la2, lo2) {
  const R = 6371000, r = Math.PI / 180;
  const dp = (la2 - la1) * r, dl = (lo2 - lo1) * r;
  const a = Math.sin(dp / 2) ** 2 + Math.cos(la1 * r) * Math.cos(la2 * r) * Math.sin(dl / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}
const field = (id, label, val = '', attrs = '') =>
  `<div class="fg"><label for="${id}">${esc(label)}</label><input id="${id}" value="${esc(val)}" ${attrs}></div>`;
const check = (id, label, on) =>
  `<div class="switch-row"><label for="${id}">${label}</label><input type="checkbox" id="${id}" ${on ? 'checked' : ''}></div>`;
const row = (label, id, val = '--', cls = '') =>
  `<div class="srow"><span>${label}</span><span class="sv ${cls}" id="${id}">${val}</span></div>`;

// ---------------------------------------------------------------- مسیریابی
function go(name) { A.stack.push(A.screen); A.screen = name; render(); }
function back() { if (A.stack.length) { A.screen = A.stack.pop(); render(); return true; } return false; }
function reset(name) { A.stack = []; A.screen = name; render(); }

function render() {
  A.lastSig = '';
  const fn = SCREENS[A.screen] || SCREENS.main;
  fn();
  window.scrollTo(0, 0);
}
const shell = (title, body, opts = {}) => {
  root.innerHTML = `
  <div class="page">
    <div class="topbar">
      ${opts.noBack ? '<span></span>' : `<button class="ghost" data-act="back">${getLang() === 'fa' ? '→' : '←'} ${esc(t('back'))}</button>`}
      <h1>${title}</h1><span></span>
    </div>
    <div class="card">${body}</div>
  </div>`;
};

// ---------------------------------------------------------------- اتصال
const remoteConfigured = () => remote.configured(S.remote);

async function chooseMode() {
  stopPolling(); remote.stop();
  const wantLocal = L.isNative && S.mode !== 'remote' && !!S.token;
  const wantRemote = S.mode !== 'local' && remoteConfigured();

  if (wantLocal) {
    await L.bindWifi();
    try {
      const p = await L.ping(2500);
      if (p.auth) { A.mode = 'local'; A.fails = 0; startPolling(); loadConfigQuiet(); startWatch(); return; }
      S.token = '';                                       // توکن دیگر معتبر نیست
    } catch (e) { /* خارج از برد وای‌فای */ }
    if (wantRemote) await L.unbindWifi();
  }
  if (wantRemote) { await startRemote(); startWatch(); return; }
  A.mode = 'none'; A.view = null; startWatch();
}

async function startRemote() {
  A.mode = 'remote'; A.view = null; A.stale = true;
  remote.start(S.remote,
    (v, retained) => { A.view = v; A.stale = retained; onView(); },
    st => { A.rstatus = st; if (st !== 'online') A.stale = true; onView(true); });
}

async function loadConfigQuiet() {
  try {
    const c = await L.call('config', {}, { timeout: 5000 });
    A.cfg = c; A.thr = c.thr;
    S.names = c.names; 
    if (!S.remote.devToken && c.mqtt.token) { S.remote.devToken = c.mqtt.token; if (c.mqtt.user) S.remote.user = S.remote.user || c.mqtt.user; saveAll(); }
    if (A.screen === 'main') render();
  } catch (e) {}
}

function startPolling() {
  stopPolling(); tick();
  A.poll = setInterval(tick, 3000);
}
function stopPolling() { if (A.poll) { clearInterval(A.poll); A.poll = null; } }

async function tick() {
  if (A.mode !== 'local' || document.hidden || A.busy) return;
  try {
    const s = await L.call('state', {}, { timeout: 3500 });
    A.view = L.normalizeState(s); A.stale = false; A.fails = 0;
    onView();
  } catch (e) {
    if (e.code === 'unauthorized') { S.token = ''; await chooseMode(); reset(S.token ? 'main' : (remoteConfigured() ? 'main' : 'pair')); return; }
    if (++A.fails >= 3) { A.stale = true; onView(true); }
  }
}

// هر چند ثانیه: در حالت خودکار بین مستقیم و راه دور جابه‌جا می‌شود
function startWatch() {
  if (A.watch) clearInterval(A.watch);
  A.watch = setInterval(async () => {
    if (S.mode !== 'auto' || document.hidden) return;
    if (A.mode === 'local' && A.fails >= 3 && remoteConfigured()) {
      stopPolling(); await L.unbindWifi(); await startRemote(); render();
    } else if (A.mode === 'remote' && L.isNative && S.token) {
      await L.bindWifi();
      try {
        const p = await L.ping(1500);
        if (p.auth) { remote.stop(); A.mode = 'local'; A.fails = 0; startPolling(); loadConfigQuiet(); render(); return; }
      } catch (e) {}
      await L.unbindWifi();
    }
  }, 12000);
}

document.addEventListener('visibilitychange', () => { if (!document.hidden) { if (A.mode === 'local') tick(); } });

// ---------------------------------------------------------------- صفحه اصلی
function onView(force) {
  if (A.screen === 'main') {
    const sig = JSON.stringify([A.view, A.stale, A.mode, A.rstatus, S.names, A.thr]);
    if (force || sig !== A.lastSig) { A.lastSig = sig; renderMain(); }
  } else if (A.screen === 'sim') liveSim();
  else if (A.screen === 'gps') liveGps();
}

function bar(icon, label, pct, text) {
  return `<div class="sig"><div class="sl">${icon} ${label}</div>
    <div class="bar"><div class="fill" style="width:${pct}%;background:${barColor(pct)}"></div></div>
    <div class="sp">${text}</div></div>`;
}

function badge() {
  if (A.mode === 'none') return `<span class="badge off">${t('modeOffline')}</span>`;
  const lab = A.mode === 'local' ? t('modeLocal') : t('modeRemote');
  const ok = A.mode === 'local' ? !A.stale : (A.rstatus === 'online' && !A.stale);
  const txt = (A.mode === 'remote' && A.rstatus === 'connecting') ? t('connecting') : lab;
  return `<span class="badge ${ok ? 'ok' : 'warn'}">${txt}</span>`;
}

function renderMain() {
  const v = A.view;
  let bars = '';
  if (v) {
    if (v.wifi) bars += bar('📶', t('wifi'), v.wifi.pct, v.wifi.pct + '%');
    if (v.gsm) {
      bars += bar('📱', t('gsm'), v.gsm.pct, v.gsm.pct + '%');
      if (v.gsm.rx !== null) {
        const g = v.gsm;
        const st = !g.internet ? '—' : g.ip ? '✔' : (g.att && g.reg) ? '…' : g.reg ? '…' : '✖';
        bars += `<div class="sig mini"><span><b>2G</b></span><span>GPRS ${st}</span><span>↓ ${Number(g.rx).toFixed(2)} KB</span><span>↑ ${Number(g.tx).toFixed(2)} KB</span></div>`;
      }
    }
    if (v.gps && v.gps.en) {
      const p = v.gps, pct = Math.min(100, (p.sats / 12) * 100);
      const col = p.fix ? (p.sats >= 8 ? '#4caf50' : p.sats >= 5 ? '#ffeb3b' : '#ff9800') : '#f44336';
      bars += `<div class="sig ${p.alarm ? 'alarm' : ''}"><div class="sl">📍 ${t('gps')}</div>
        <div class="bar"><div class="fill" style="width:${pct}%;background:${col}"></div></div>
        <div class="sp">${p.alarm ? '⚠️ ' + t('alarm') : (p.fix ? p.sats + '/12' : t('search'))}</div></div>`;
    }
  }

  let weak = '';
  if (A.mode === 'local' && v && v.weak !== null) {
    weak = `<div class="sig weak"><input type="checkbox" id="weak" data-chg="weak" ${v.weak ? 'checked' : ''}>
      <label for="weak">⚠️ ${esc(t('autoDisc', { n: A.thr }))}</label></div>`;
  }

  const cards = [0, 1, 2, 3].map(i => {
    const on = v ? v.outputs[i] : false;
    return `<div class="out" data-act="toggle" data-pin="${i}">
      <div class="on">${esc(outName(i))}${A.mode === 'local' ? `<span class="pen" data-act="rename" data-pin="${i}">✏️</span>` : ''}</div>
      <div class="st ${on ? 'on' : 'off'}">${on ? t('on') + ' ✓' : t('off') + ' ○'}</div></div>`;
  }).join('');

  let extra = '';
  if (A.mode === 'remote') {
    extra = `<div class="rbtns">
      <button data-act="rcmd" data-cmd="lock">🔒 ${t('rLock')}</button>
      <button data-act="rcmd" data-cmd="unlock">🔓 ${t('rUnlock')}</button>
      <button data-act="rcmd" data-cmd="alarm_on">🔔 ${t('rAlarmOn')}</button>
      <button data-act="rcmd" data-cmd="alarm_off">🔕 ${t('rAlarmOff')}</button>
      <button data-act="rcmd" data-cmd="location">📍 ${t('rLoc')}</button></div>`;
  }
  const wait = (A.mode === 'remote' && A.stale && A.view) ? `<div class="note">${t('waitLive')}</div>` : '';

  root.innerHTML = `<div class="page">
    <div class="topbar">
      <button class="ghost" data-act="lang">${getLang() === 'fa' ? 'English' : 'فارسی'}</button>
      <h1>${esc(t('appTitle'))}</h1>${badge()}
    </div>
    ${bars}${weak}
    <div class="card">${wait}<div class="grid">${cards}</div>${extra}
      <button class="primary" data-act="menu">⚙️ ${t('settings')}</button></div></div>`;
}

// ---------------------------------------------------------------- عملیات
async function doToggle(pin) {
  if (A.busy) return;
  const el = root.querySelector(`.out[data-pin="${pin}"]`); if (el) el.classList.add('busy');
  try {
    if (A.mode === 'local') {
      A.busy = true;
      const r = await L.call('toggle', { pin }, { timeout: 4000 });
      A.busy = false;
      if (A.view) {
        A.view.outputs[pin] = (pin === 1) ? true : r.state === 1;
        onView(true);
        if (pin === 1) setTimeout(() => { if (A.view) { A.view.outputs[1] = false; onView(true); } }, 1000);
      }
      setTimeout(tick, 300);
    } else if (A.mode === 'remote') {
      const cur = A.view ? A.view.outputs[pin] : false;
      await remote.toggle(pin, pin === 1 ? true : !cur);
      toast(t('cmdOk'), 'ok');
    }
    if (navigator.vibrate) navigator.vibrate(40);
  } catch (e) {
    A.busy = false;
    toast(e.message === 'timeout' ? t('cmdTimeout') : (e.code ? errText(e) : t('cmdFail')), 'err');
  }
  if (el) el.classList.remove('busy');
}

function openRename(pin) {
  const m = $('#modal');
  m.innerHTML = `<div class="mbox"><h3>${esc(t('editName'))}</h3>
    <input id="nm" maxlength="30" value="${esc(outName(pin))}" autocomplete="off">
    <div class="mbtn"><button class="ok" data-act="nmsave" data-pin="${pin}">${t('save')}</button>
    <button class="no" data-act="mclose">${t('cancel')}</button></div></div>`;
  m.className = 'modal show';
  setTimeout(() => { const i = $('#nm'); i.focus(); i.select(); }, 60);
}
const closeModal = () => { const m = $('#modal'); m.className = 'modal'; m.innerHTML = ''; };

// ---------------------------------------------------------------- فعال‌سازی
function pairScreen() {
  shell('🔐 ' + esc(t('pairTitle')), `
    <p class="hint">${esc(t('pairInfo'))}</p>
    ${field('code', t('pairCode'), '', 'inputmode="numeric" autocomplete="off" dir="ltr"')}
    <button class="primary green" data-act="dopair">${t('pairBtn')}</button>
    <div id="msg" class="msg"></div>
    <button class="ghost wide" data-act="remoteonly">${t('remoteOnly')}</button>`, { noBack: true });
}
async function doPair() {
  const code = $('#code').value.trim(), msg = $('#msg');
  if (!code) return;
  msg.className = 'msg'; msg.textContent = '...';
  try {
    await L.bindWifi();
    const r = await L.pair(code);
    S.token = r.token;
    toast(t('pairOk'), 'ok');
    await chooseMode(); reset('main');
  } catch (e) {
    msg.className = 'msg err';
    msg.textContent = e.code === 'wrong_code' ? t('pairWrong') : e.code === 'locked' ? t('pairLocked', { n: e.wait }) : t('pairNoDevice');
  }
}

// ---------------------------------------------------------------- منوی تنظیمات
function menuScreen() {
  const items = [['ap', '📡', t('menuAp')], ['sim', '📱', t('menuSim')], ['gps', '🛰️', t('menuGps')], ['mqtt', '🌐', t('menuMqtt')], ['app', '🛠️', t('menuApp')]];
  const local = A.mode === 'local';
  shell('⚙️ ' + esc(t('settings')), items
    .filter(i => local || i[0] === 'app')
    .map(i => `<button class="menu-item" data-act="goto" data-to="${i[0]}"><span>${i[1]}</span><span class="mt">${esc(i[2])}</span><span class="ar">${getLang() === 'fa' ? '◀' : '▶'}</span></button>`).join(''));
}

// ---------------------------------------------------------------- فرم‌های دستگاه (فقط حالت مستقیم)
async function withConfig(title, build, mount) {
  shell(title, `<div class="msg">…</div>`);
  try {
    const c = await L.call('config', {}, { timeout: 6000 });
    A.cfg = c; A.thr = c.thr;
    shell(title, build(c));
    if (mount) mount(c);
  } catch (e) { shell(title, `<div class="msg err">${esc(errText(e))}</div>`); }
}
async function save(path, params, okMsg) {
  const m = $('#msg'); if (m) { m.className = 'msg'; m.textContent = t('saving'); }
  try {
    const r = await L.call(path, params, { timeout: 8000 });
    if (m) { m.className = 'msg ok'; m.textContent = okMsg || t('saved'); }
    return r;
  } catch (e) { if (m) { m.className = 'msg err'; m.textContent = errText(e); } return null; }
}

function apScreen() {
  withConfig('📡 ' + esc(t('apTitle')), c => `
    ${field('ssid', t('ssid'), c.ap.ssid, 'maxlength="32"')}
    ${field('pwd', t('pwd'), c.ap.pwd, 'minlength="8" maxlength="63" dir="ltr"')}
    ${field('ip', t('ip'), c.ap.ip, 'dir="ltr"')}
    <button class="primary" data-act="apsave">💾 ${t('apBtn')}</button>
    <div class="warn">⚠️ ${esc(t('apWarn'))}</div>
    <div class="box"><h3>📶 ${t('thrTitle')}</h3><p class="hint">${esc(t('thrDesc'))}</p>
      ${field('thr', t('thrLabel'), c.thr, 'type="number" min="0" max="100" dir="ltr"')}
      <button class="primary green" data-act="thrsave">💾 ${t('thrSave')}</button><div id="msg" class="msg"></div></div>`);
}

function simScreen() {
  withConfig('📱 ' + esc(t('simTitle')), c => {
    const labels = ['📍 Location', '🔒 Lock', '🔓 Unlock', '🔔 Alarm ON', '🔕 Alarm OFF', '🛑 Stop Location', '📊 Status'];
    return `<div class="status" id="live"></div>
    <h3>🔧 APN</h3>
    ${field('apn', t('apn'), c.sim.apn, 'dir="ltr" placeholder="mcinet / mtnirancell / rightel"')}
    <button class="orange" data-act="apn">🔍 ${t('autoDetect')}</button><p class="hint">${esc(t('apnHint'))}</p>
    <h3>📞 ${t('phonesTitle')}</h3><p class="hint">${esc(t('phonesHint'))}</p>
    ${c.sim.phones.map((p, i) => field('ph' + i, (i + 1) + '.', p, 'type="tel" dir="ltr" placeholder="+989121234567"')).join('')}
    <h3>✉️ ${t('smsTitle')}</h3><p class="hint">${esc(t('smsHint'))}</p>
    ${c.sim.cmds.map((p, i) => field('c' + i, labels[i] || ('#' + i), p, 'maxlength="30" dir="ltr"')).join('')}
    <h3>🛡️ ${t('secTitle')}</h3>
    ${check('sec', esc(t('secLabel')), c.sim.sec)}<p class="hint">${esc(t('secHint'))}</p>
    <button class="primary green" data-act="simsave">💾 ${t('save')}</button><div id="msg" class="msg"></div>`;
  }, () => liveSim());
}
function liveSim() {
  const el = $('#live'), v = A.view; if (!el || !v) return;
  const g = v.gsm, s = v.sec;
  let gsm = t('init'), gc = 'warn', net = '-', nc = '', car = '-', ip = '-', call = '-';
  if (g) {
    if (g.ready) { gsm = g.pct + '% (CSQ ' + g.csq + ')'; gc = g.pct >= 65 ? 'ok' : g.pct >= 30 ? 'warn' : 'err'; }
    if (g.ip) { net = t('online'); nc = 'ok'; } else if (g.busy) { net = t('connecting'); nc = 'warn'; }
    else if (g.att && g.reg) { net = t('attachedNoIp'); nc = 'warn'; } else if (g.reg) { net = t('registered'); nc = 'warn'; } else { net = t('noNet'); nc = 'err'; }
    car = g.carrier || '-'; ip = g.ip || '-'; call = g.call ? 'ACTIVE' : t('idle');
  }
  let sec = t('disabled'), sc = '';
  if (s.en) { sec = t('secNames')[s.state] || '?'; if (s.state === 1) sec += ' (' + s.left + 's)'; sc = s.state === 0 ? 'ok' : s.state === 3 ? 'err' : 'warn'; }
  el.innerHTML = row('📶 ' + t('simGsm'), 'a', gsm, gc) + row('🌐 ' + t('simNet'), 'b', net, nc) + row('📡 ' + t('simCarrier'), 'c', esc(car)) +
    row('🔗 ' + t('simIp'), 'd', esc(ip)) + row('📞 ' + t('simCall'), 'e', call) + row('🛡️ ' + t('simSec'), 'f', sec, sc) +
    row('🔒 ' + t('simLock'), 'g', s.locked ? t('lockedS') : t('free'), s.locked ? 'err' : 'ok') +
    row('🔔 ' + t('simAlarm'), 'h', s.alarm ? 'ON' : 'off', s.alarm ? 'warn' : '');
}

function gpsScreen() {
  withConfig('🛰️ ' + esc(t('gpsTitle')), c => `
    <div id="alarmb" class="alarm-banner" style="display:none">${t('szAlarm')}</div>
    <div class="status" id="live"></div>
    ${check('anti', '🛰️ ' + esc(t('antiCut')), c.gps.anti)}<p class="hint">${esc(t('antiCutHint'))}</p>
    <div class="fg"><label for="site">${t('mapSite')}</label><select id="site">
      <option value="0">Google Maps</option><option value="1">نشان (Neshan)</option><option value="2">بلد (Balad)</option></select></div>
    <h3>🔒 ${t('szTitle')}</h3>
    ${check('sz', esc(t('szEnable')), c.gps.sz)}
    ${field('lat', t('szLat'), c.gps.lat, 'dir="ltr" inputmode="decimal"')}
    ${field('lng', t('szLng'), c.gps.lng, 'dir="ltr" inputmode="decimal"')}
    ${field('rad', t('szRadius'), c.gps.rad, 'type="number" min="1" max="100000" dir="ltr"')}
    <button class="orange" data-act="usecur">📍 ${t('useCurrent')}</button><p class="hint">${esc(t('szHint'))}</p>
    <button class="primary green" data-act="gpssave">💾 ${t('save')}</button><div id="msg" class="msg"></div>`,
  c => { $('#site').value = c.gps.site; liveGps(); });
}
function liveGps() {
  const el = $('#live'), v = A.view; if (!el || !v || !v.gps) return;
  const p = v.gps;
  let st = t('gpsNoFix'), sc = 'err';
  if (p.fix) { st = t('gpsFix'); sc = 'ok'; } else if (p.searching) { st = t('gpsSearching'); sc = 'warn'; }
  let dist = '-', dc = '';
  const c = A.cfg;
  if (p.fix && c && (c.gps.lat || c.gps.lng)) {
    const d = haversine(p.lat, p.lng, c.gps.lat, c.gps.lng);
    dist = Math.round(d) + ' m (' + c.gps.rad + ' m)'; dc = d > c.gps.rad ? 'err' : 'ok';
  }
  el.innerHTML = row(t('gpsStatus'), 'a', st, sc) + row(t('gpsSats'), 'b', p.sats) +
    row(t('gpsLat'), 'c', p.fix ? p.lat.toFixed(6) : '-') + row(t('gpsLng'), 'd', p.fix ? p.lng.toFixed(6) : '-') + row(t('gpsDist'), 'e', dist, dc);
  const b = $('#alarmb'); if (b) b.style.display = p.alarm ? 'block' : 'none';
}

function mqttScreen() {
  withConfig('🌐 ' + esc(t('mqttTitle')), c => {
    const m = c.mqtt;
    const st = m.on ? [t('mqttConnected'), 'ok'] : m.en ? [t('mqttDisc'), 'warn'] : [t('mqttOff'), ''];
    return `<div class="status">${row('MQTT', 'a', st[0], st[1])}${row('Device Token', 'b', m.token ? esc(m.token.slice(0, 8)) + '…' : '-')}</div>
    ${check('en', esc(t('mqttEnable')), m.en)}
    ${field('host', t('mqttHost'), m.host, 'disabled dir="ltr"')}
    ${field('port', t('mqttPort'), m.port, 'type="number" min="1" max="65535" dir="ltr"')}
    ${field('user', t('mqttUser'), m.user, 'autocomplete="off" dir="ltr"')}
    ${field('pass', t('mqttPass'), '', `type="password" autocomplete="new-password" dir="ltr" placeholder="${m.hasPass ? esc(t('mqttPassKeep')) : ''}"`)}
    ${field('tok', t('mqttToken'), m.token, 'dir="ltr" placeholder="32-char token"')}<p class="hint">${esc(t('mqttTokenHint'))}</p>
    <button class="primary green" data-act="mqttsave">💾 ${t('save')}</button><div id="msg" class="msg"></div>`;
  });
}

// ---------------------------------------------------------------- تنظیمات برنامه
function appScreen() {
  const r = S.remote;
  shell('🛠️ ' + esc(t('appTitle2')), `
    <div class="fg"><label>${t('language')}</label><select id="lang"><option value="fa">فارسی</option><option value="en">English</option></select></div>
    ${L.isNative ? `
    <div class="fg"><label>${t('connMode')}</label><select id="mode">
      <option value="auto">${t('mAuto')}</option><option value="local">${t('mLocal')}</option><option value="remote">${t('mRemote')}</option></select></div>
    ${field('host', t('localHost'), S.host, 'dir="ltr" inputmode="decimal"')}` : `<p class="hint">${esc(t('pwaNote'))}</p>`}
    <h3>☁️ ${t('remoteTitle')}</h3>
    ${field('ws', t('wsUrl'), r.url, 'dir="ltr" placeholder="wss://smartcarlock.ir:8084/mqtt"')}<p class="hint">${esc(t('wsHint'))}</p>
    ${field('ruser', t('brokerUser'), r.user, 'autocomplete="off" dir="ltr"')}
    ${field('rpass', t('brokerPass'), r.pass, 'type="password" autocomplete="off" dir="ltr"')}
    ${field('rtok', t('devToken'), r.devToken, 'dir="ltr"')}
    <button class="primary green" data-act="appsave">💾 ${t('remoteSave')}</button><div id="msg" class="msg"></div>
    ${L.isNative && S.token ? `<button class="danger wide" data-act="unpair">${t('unpair')}</button>` : ''}`);
  $('#lang').value = getLang();
  if (L.isNative) $('#mode').value = S.mode;
}
async function saveApp() {
  const lang = $('#lang').value;
  S.remote = { url: $('#ws').value.trim(), user: $('#ruser').value.trim(), pass: $('#rpass').value, devToken: $('#rtok').value.trim() };
  if (L.isNative) { S.mode = $('#mode').value; S.host = $('#host').value.trim() || '192.168.4.1'; }
  else S.mode = 'remote';
  S.lang = lang; setLang(lang); saveAll();
  if (S.mode === 'remote' && !remoteConfigured()) { toast(t('needRemote'), 'err'); return; }
  await chooseMode();
  reset(A.mode === 'none' && L.isNative && !S.token && !remoteConfigured() ? 'pair' : 'main');
}

// ---------------------------------------------------------------- صفحه‌ها
const SCREENS = {
  main: () => { if (A.mode === 'none' && !S.token && !remoteConfigured()) { A.screen = 'pair'; pairScreen(); } else renderMain(); },
  pair: pairScreen, menu: menuScreen, ap: apScreen, sim: simScreen, gps: gpsScreen, mqtt: mqttScreen, app: appScreen,
};

// ---------------------------------------------------------------- رویدادها
const ACT = {
  back: () => back(),
  lang: () => { S.lang = getLang() === 'fa' ? 'en' : 'fa'; setLang(S.lang); render(); },
  toggle: el => doToggle(+el.dataset.pin),
  rename: (el, e) => { e.stopPropagation(); openRename(+el.dataset.pin); },
  mclose: closeModal,
  nmsave: async el => {
    const pin = +el.dataset.pin, name = $('#nm').value.trim(); if (!name) return;
    try { await L.call('name', { pin, name }); S.names[pin] = name; saveAll(); closeModal(); render(); }
    catch (e) { toast(errText(e), 'err'); }
  },
  rcmd: async el => {
    try { toast(t('cmdPending')); await remote.send(el.dataset.cmd, {}, () => toast(t('cmdPending'))); toast(t('cmdOk'), 'ok'); }
    catch (e) { toast(e.message === 'timeout' ? t('cmdTimeout') : t('cmdFail'), 'err'); }
  },
  menu: () => go('menu'),
  goto: el => go(el.dataset.to),
  dopair: doPair,
  remoteonly: () => { A.stack = ['main']; A.screen = 'app'; render(); },
  apsave: async () => {
    const r = await save('ap', { ssid: $('#ssid').value, password: $('#pwd').value, ip: $('#ip').value }, t('apReboot'));
    if (r && !r.changed) { toast(t('apNoChange')); }
  },
  thrsave: async () => {
    const v = parseInt($('#thr').value, 10);
    if (isNaN(v) || v < 0 || v > 100) { const m = $('#msg'); m.className = 'msg err'; m.textContent = t('thrInvalid'); return; }
    const r = await save('threshold', { value: v }); if (r) A.thr = r.thr;
  },
  apn: async () => {
    const m = $('#msg'); m.className = 'msg'; m.textContent = '...';
    try { const r = await L.call('apn', {}, { timeout: 25000 }); $('#apn').value = r.apn; m.className = 'msg ok'; m.textContent = 'APN: ' + r.apn; }
    catch (e) { m.className = 'msg err'; m.textContent = t('saveFailed'); }
  },
  simsave: () => {
    const p = { sec: $('#sec').checked ? 1 : 0, apn: $('#apn').value };
    A.cfg.sim.phones.forEach((_, i) => { p['ph' + i] = $('#ph' + i).value; });
    A.cfg.sim.cmds.forEach((_, i) => { p['c' + i] = $('#c' + i).value; });
    return save('sim', p);
  },
  usecur: () => {
    const p = A.view && A.view.gps;
    if (!p || !p.fix) { toast(t('noFixYet'), 'err'); return; }
    $('#lat').value = p.lat.toFixed(6); $('#lng').value = p.lng.toFixed(6);
  },
  gpssave: async () => {
    const r = await save('gps', { site: $('#site').value, sz: $('#sz').checked ? 1 : 0, lat: $('#lat').value, lng: $('#lng').value, rad: $('#rad').value, anti: $('#anti').checked ? 1 : 0 });
    if (r) {
      A.cfg.gps = { site: +$('#site').value, sz: $('#sz').checked, lat: parseFloat($('#lat').value) || 0, lng: parseFloat($('#lng').value) || 0, rad: +$('#rad').value, anti: $('#anti').checked };
      if (r.warn) { const m = $('#msg'); m.className = 'msg warn'; m.textContent = '⚠️ ' + t('szNoCoords'); }
    }
  },
  mqttsave: () => {
    const p = { enabled: $('#en').checked ? 1 : 0, port: $('#port').value, user: $('#user').value, token: $('#tok').value };
    if ($('#pass').value !== '') p.pass = $('#pass').value;          // فقط اگر کاربر رمز جدید نوشته (رمز قبلی پاک نشود)
    return save('mqtt', p);
  },
  appsave: saveApp,
  unpair: async () => {
    if (!confirm(t('unpairConfirm'))) return;
    try { await L.call('unpair'); } catch (e) {}
    S.token = ''; await chooseMode(); reset(remoteConfigured() ? 'main' : 'pair');
  },
};
root.addEventListener('click', e => {
  const el = e.target.closest('[data-act]'); if (!el) return;
  const f = ACT[el.dataset.act]; if (f) f(el, e);
});
root.addEventListener('change', async e => {
  if (e.target.dataset.chg === 'weak') {
    try { await L.call('weak', { enabled: e.target.checked ? 1 : 0 }); } catch (err) { toast(errText(err), 'err'); e.target.checked = !e.target.checked; }
  }
});
$('#modal').addEventListener('click', e => {
  if (e.target.id === 'modal') closeModal();
  const el = e.target.closest('[data-act]'); if (el && ACT[el.dataset.act]) ACT[el.dataset.act](el, e);
});
$('#modal').addEventListener('keydown', e => { if (e.key === 'Enter') { const b = $('#modal [data-act="nmsave"]'); if (b) b.click(); } if (e.key === 'Escape') closeModal(); });

// دکمه‌ی بازگشت اندروید
if (L.isNative && window.Capacitor.Plugins.App) {
  window.Capacitor.Plugins.App.addListener('backButton', () => {
    if ($('#modal').classList.contains('show')) { closeModal(); return; }
    if (!back()) window.Capacitor.Plugins.App.exitApp();
  });
}

// ---------------------------------------------------------------- شروع
(async function boot() {
  setLang(S.lang);
  if (!L.isNative && S.mode !== 'remote') S.mode = 'remote';
  if ('serviceWorker' in navigator && !L.isNative && location.protocol === 'https:') navigator.serviceWorker.register('sw.js').catch(() => {});
  root.innerHTML = '<div class="page"><div class="msg">…</div></div>';
  await chooseMode();
  if (A.mode === 'none' && !S.token && !remoteConfigured()) reset(L.isNative ? 'pair' : 'app'); else reset('main');
})();
