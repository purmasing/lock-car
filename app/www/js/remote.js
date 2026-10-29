// کنترل از راه دور از طریق MQTT (همان کانال سرور smartcarlock.ir که دستگاه با SIM800 به آن وصل است)
import { MqttWs } from './mqtt.js';

export class Remote {
  constructor() { this.cli = null; this.cfg = null; this.acks = new Map(); this.onState = null; this.onStatus = null; this.status = 'offline'; }

  configured(c) { return !!(c && c.url && c.devToken); }

  start(cfg, onState, onStatus) {
    this.stop();
    if (!this.configured(cfg)) return false;
    this.cfg = cfg; this.onState = onState; this.onStatus = onStatus;
    const base = 'car/' + cfg.devToken;
    this.base = base;
    this.cli = new MqttWs({
      url: cfg.url, user: cfg.user, pass: cfg.pass,
      subs: [base + '/status', base + '/ack'],
      onStatus: s => { this.status = s; if (this.onStatus) this.onStatus(s); },
      onMessage: (topic, text, retained) => this._msg(topic, text, retained),
    });
    this.cli.start();
    return true;
  }
  stop() {
    if (this.cli) { this.cli.stop(); this.cli = null; }
    this.acks.forEach(a => { clearTimeout(a.timer); a.reject(new Error('stopped')); });
    this.acks.clear(); this.status = 'offline';
  }

  _msg(topic, text, retained) {
    let j; try { j = JSON.parse(text); } catch (e) { return; }
    if (topic.endsWith('/status')) {
      if (this.onState) this.onState(normalizeRemote(j), retained);
    } else if (topic.endsWith('/ack')) {
      const a = this.acks.get(j.id);
      if (!a) return;
      if (j.status === 'ok') { clearTimeout(a.timer); this.acks.delete(j.id); a.resolve('ok'); }
      else if (j.status === 'pending') { a.pending = true; if (a.onPending) a.onPending(); }
      else { clearTimeout(a.timer); this.acks.delete(j.id); a.reject(new Error(j.status || 'fail')); }
    }
  }

  /** ارسال فرمان و انتظار برای تأیید دستگاه */
  send(cmd, extra = {}, onPending) {
    return new Promise((resolve, reject) => {
      if (!this.cli) return reject(new Error('offline'));
      const id = Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
      const payload = JSON.stringify({ id, cmd, ...extra });     // بدون فاصله؛ پارسر دستگاه ساده است
      if (!this.cli.publish(this.base + '/cmd', payload)) return reject(new Error('offline'));
      const timer = setTimeout(() => { this.acks.delete(id); reject(new Error('timeout')); }, 20000);
      this.acks.set(id, { resolve, reject, timer, onPending });
    });
  }
  toggle(pin, on) { return this.send('toggle', { pin, on }); }
}

// قالب وضعیتِ MQTT → همان قالب یکسان اپ
function normalizeRemote(j) {
  const g = j.gps || {};
  return {
    source: 'remote',
    outputs: [!!j.doors, false, !!j.fuel, !!j.switch],
    weak: null,
    wifi: null,
    gsm: { pct: j.sim_pct || 0, csq: j.sim_csq, ready: !!j.sim_ready, internet: true, ip: j.car_ip || '', carrier: j.carrier || '',
           reg: !!j.gprs_reg, att: !!j.gprs_att, busy: false, call: false, rx: null, tx: null },
    sec: { en: !!j.security, state: j.sec_locked ? 3 : 0, left: 0, locked: !!j.locked, alarm: !!j.alarm },
    gps: { en: g.enabled !== false, fix: !!g.fix, searching: !!g.searching, sats: g.sats || 0, lat: g.lat || 0, lng: g.lng || 0,
           alarm: !!(g.safeZoneAlarm || j.safe_zone_alarm), violated: !!(g.safeZoneViolated || j.safe_zone_violated) },
  };
}
