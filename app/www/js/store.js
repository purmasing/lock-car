// تنظیمات برنامه (روی خود گوشی ذخیره می‌شود)
const KEY = 'carctl.v1';
const defaults = {
  lang: (navigator.language || 'fa').toLowerCase().startsWith('fa') ? 'fa' : 'en',
  mode: 'auto',              // auto | local | remote
  host: '192.168.4.1',
  token: '',                 // توکن دستگاه برای اتصال مستقیم
  names: ['', '', '', '', ''],
  remote: { url: '', user: '', pass: '', devToken: '' },
};
let data = load();
function load() {
  try {
    const o = JSON.parse(localStorage.getItem(KEY) || '{}');
    return { ...defaults, ...o, remote: { ...defaults.remote, ...(o.remote || {}) } };
  } catch (e) { return { ...defaults }; }
}
export const S = new Proxy({}, {
  get: (_, k) => data[k],
  set: (_, k, v) => { data[k] = v; try { localStorage.setItem(KEY, JSON.stringify(data)); } catch (e) {} return true; },
});
export function saveAll() { try { localStorage.setItem(KEY, JSON.stringify(data)); } catch (e) {} }
