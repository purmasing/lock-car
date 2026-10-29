// کش پوسته‌ی برنامه برای نسخه‌ی وب (PWA): بعد از اولین بار حتی بدون اینترنت باز می‌شود
const V = 'carctl-v1';
const FILES = ['./', 'index.html', 'css/app.css', 'js/app.js', 'js/i18n.js', 'js/store.js', 'js/local.js', 'js/mqtt.js', 'js/remote.js',
  'manifest.webmanifest', 'icons/icon-192.png', 'icons/icon-512.png', 'icons/apple-touch-icon.png'];
self.addEventListener('install', e => { e.waitUntil(caches.open(V).then(c => c.addAll(FILES)).then(() => self.skipWaiting())); });
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== V).map(k => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', e => {
  const u = new URL(e.request.url);
  if (e.request.method !== 'GET' || u.origin !== location.origin) return;
  // اول شبکه (تا نسخه‌ی جدید برسد)، اگر نبود کش
  e.respondWith(fetch(e.request).then(r => { const cp = r.clone(); caches.open(V).then(c => c.put(e.request, cp)); return r; }).catch(() => caches.match(e.request)));
});
