// کلاینت بسیار کوچک MQTT 3.1.1 روی WebSocket (بدون وابستگی خارجی)
// فقط همان چیزی که لازم است: CONNECT، SUBSCRIBE، PUBLISH (QoS 0)، PING.
const enc = new TextEncoder();
const dec = new TextDecoder();

const ustr = s => { const b = enc.encode(s); return [b.length >> 8, b.length & 255, ...b]; };
function remLen(n) {
  const o = [];
  do { let d = n % 128; n = Math.floor(n / 128); if (n > 0) d |= 128; o.push(d); } while (n > 0);
  return o;
}
const packet = (first, body) => new Uint8Array([first, ...remLen(body.length), ...body]);

export class MqttWs {
  /** opts: {url, user, pass, subs:[topic], onMessage(topic,text,retained), onStatus('connecting'|'online'|'offline'|'auth')} */
  constructor(opts) {
    this.o = opts; this.ws = null; this.buf = new Uint8Array(0);
    this.stopped = true; this.backoff = 2000; this.lastRx = 0; this.timers = [];
    this.pid = 1; this.online = false; this.authFail = false;
  }
  start() { this.stopped = false; this._open(); }
  stop() {
    this.stopped = true; this._clear();
    if (this.ws) { try { this.ws.close(); } catch (e) {} this.ws = null; }
    this._status('offline');
  }
  _status(s) { this.online = (s === 'online'); if (this.o.onStatus) this.o.onStatus(s); }
  _clear() { this.timers.forEach(clearTimeout); this.timers.forEach(clearInterval); this.timers = []; }

  _open() {
    this._clear(); this.buf = new Uint8Array(0);
    if (!this.authFail) this._status('connecting');
    let ws;
    try { ws = new WebSocket(this.o.url, 'mqtt'); } catch (e) { this._retry(); return; }
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => {
      const clientId = 'app-' + Math.random().toString(16).slice(2, 10);
      let flags = 0x02;                                  // clean session
      const pl = [...ustr(clientId)];
      if (this.o.user) { flags |= 0x80; pl.push(...ustr(this.o.user)); if (this.o.pass) { flags |= 0x40; pl.push(...ustr(this.o.pass)); } }
      const body = [0, 4, 77, 81, 84, 84, 4, flags, 0, 30, ...pl];   // "MQTT" v3.1.1 keepalive=30
      ws.send(packet(0x10, body));
      this.lastRx = Date.now();
      this.timers.push(setInterval(() => {
        if (Date.now() - this.lastRx > 50000) { try { ws.close(); } catch (e) {} return; }
        if (ws.readyState === 1) ws.send(new Uint8Array([0xC0, 0]));       // PINGREQ
      }, 15000));
    };
    ws.onmessage = ev => { this.lastRx = Date.now(); this._feed(new Uint8Array(ev.data)); };
    ws.onerror = () => {};
    ws.onclose = () => { if (this.ws === ws) { this.ws = null; this._clear(); if (!this.stopped) { if (!this.authFail) this._status('offline'); this._retry(); } } };
  }
  _retry() {
    if (this.stopped) return;
    this.timers.push(setTimeout(() => this._open(), this.backoff));
    this.backoff = Math.min(this.backoff * 1.7, 30000);
  }
  _feed(chunk) {
    const nb = new Uint8Array(this.buf.length + chunk.length); nb.set(this.buf); nb.set(chunk, this.buf.length); this.buf = nb;
    for (;;) {
      if (this.buf.length < 2) return;
      let mult = 1, len = 0, i = 1, b;
      do {
        if (i >= this.buf.length) return;
        b = this.buf[i++]; len += (b & 127) * mult; mult *= 128;
      } while ((b & 128) && i < 5);
      if (this.buf.length < i + len) return;
      const first = this.buf[0], body = this.buf.slice(i, i + len);
      this.buf = this.buf.slice(i + len);
      this._packet(first, body);
    }
  }
  _packet(first, body) {
    const type = first >> 4;
    if (type === 2) {                                    // CONNACK
      const rc = body[1];
      if (rc === 0) {
        this.backoff = 2000; this.authFail = false;
        const pl = [];
        for (const tp of this.o.subs) pl.push(...ustr(tp), 0);
        this.ws.send(packet(0x82, [this.pid >> 8, this.pid & 255, ...pl]));
        this.pid = (this.pid % 65000) + 1;
        this._status('online');
      } else {
        this.authFail = true; this.backoff = 30000;
        this._status('auth');                            // نام کاربری/رمز نادرست یا مجاز نیست
        try { this.ws.close(); } catch (e) {}
      }
    } else if (type === 3) {                             // PUBLISH
      const qos = (first >> 1) & 3, retained = !!(first & 1);
      const tl = (body[0] << 8) | body[1];
      const topic = dec.decode(body.slice(2, 2 + tl));
      let p = 2 + tl;
      if (qos > 0) { const id = [body[p], body[p + 1]]; p += 2; if (qos === 1) this.ws.send(new Uint8Array([0x40, 2, ...id])); }
      if (this.o.onMessage) this.o.onMessage(topic, dec.decode(body.slice(p)), retained);
    }
  }
  publish(topic, text) {
    if (!this.ws || this.ws.readyState !== 1 || !this.online) return false;
    this.ws.send(packet(0x30, [...ustr(topic), ...enc.encode(text)]));
    return true;
  }
}
