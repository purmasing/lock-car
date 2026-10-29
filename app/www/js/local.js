// اتصال مستقیم به دستگاه روی وای‌فای (فقط در اپ نیتیو؛ مرورگر HTTPS اجازه‌ی HTTP محلی را نمی‌دهد)
import { S } from './store.js';

const cap = window.Capacitor;
export const isNative = !!(cap && cap.isNativePlatform && cap.isNativePlatform());
const nativeHttp = isNative && cap.Plugins && cap.Plugins.CapacitorHttp;
const wifiBind = isNative && cap.Plugins && cap.Plugins.WifiBind;   // فقط اندروید

export class ApiError extends Error {
  constructor(code, extra) { super(code); this.code = code; Object.assign(this, extra || {}); }
}

// اندروید: وقتی وای‌فای اینترنت ندارد، ترافیک را به سیم‌کارت می‌فرستد؛ پروسه را به وای‌فای می‌چسبانیم
export async function bindWifi() { try { if (wifiBind) await wifiBind.bind(); } catch (e) {} }
export async function unbindWifi() { try { if (wifiBind) await wifiBind.unbind(); } catch (e) {} }

function withTimeout(p, ms) {
  return new Promise((res, rej) => {
    const id = setTimeout(() => rej(new ApiError('timeout')), ms);
    p.then(v => { clearTimeout(id); res(v); }, e => { clearTimeout(id); rej(e); });
  });
}

async function rawGet(url, headers, timeout) {
  if (nativeHttp) {
    const r = await withTimeout(nativeHttp.get({ url, headers, connectTimeout: timeout, readTimeout: timeout }), timeout + 500);
    let d = r.data;
    if (typeof d === 'string') { try { d = JSON.parse(d); } catch (e) { d = {}; } }
    return { status: r.status, data: d || {} };
  }
  const ctl = new AbortController();
  const id = setTimeout(() => ctl.abort(), timeout);
  try {
    const r = await fetch(url, { headers, signal: ctl.signal, cache: 'no-store' });
    let d = {}; try { d = await r.json(); } catch (e) {}
    return { status: r.status, data: d };
  } finally { clearTimeout(id); }
}

/** فراخوانی API دستگاه. params به‌صورت query ارسال می‌شود. */
export async function call(path, params = {}, opt = {}) {
  const timeout = opt.timeout || 4000;
  const q = Object.keys(params).filter(k => params[k] !== undefined && params[k] !== null)
    .map(k => encodeURIComponent(k) + '=' + encodeURIComponent(params[k])).join('&');
  const url = 'http://' + (opt.host || S.host) + '/api/' + path + (q ? '?' + q : '');
  const headers = {};
  const token = opt.token !== undefined ? opt.token : S.token;
  if (token) headers['X-Token'] = token;

  let r;
  try { r = await rawGet(url, headers, timeout); }
  catch (e) { throw (e instanceof ApiError ? e : new ApiError('net')); }

  if (r.status === 401) throw new ApiError('unauthorized');
  if (r.status === 429) throw new ApiError('locked', { wait: r.data.wait || 60 });
  if (r.status >= 400 || r.data.ok === false) throw new ApiError(r.data.err || 'fail', { data: r.data });
  return r.data;
}

export const ping = (timeout = 2000) => call('ping', {}, { timeout });
export const pair = code => call('pair', { code }, { token: '', timeout: 6000 });

/** وضعیت دستگاه را به قالب یکسان تبدیل می‌کند (همان قالبی که از MQTT هم ساخته می‌شود) */
export function normalizeState(s) {
  return {
    source: 'local',
    outputs: s.o.map(v => v === 1),
    weak: !!s.w,
    wifi: { pct: s.wp, rssi: s.rs },
    gsm: s.g.en ? {
      pct: s.g.pct, csq: s.g.csq, ready: s.g.rdy, internet: s.g.inet, ip: s.g.ip, carrier: s.g.car,
      reg: s.g.reg, att: s.g.att, busy: s.g.busy, call: s.g.call, rx: s.g.rx, tx: s.g.tx,
    } : null,
    sec: { en: s.s.en, state: s.s.st, left: s.s.left, locked: s.s.lock, alarm: s.s.alm },
    gps: { en: s.p.en, fix: s.p.fix, searching: s.p.srch, sats: s.p.sat, lat: s.p.lat, lng: s.p.lng, alarm: s.p.sza, violated: s.p.szv },
  };
}
