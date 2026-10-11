// Only registers in secure HTTPS / localhost contexts. Never cache API responses.
const CACHE='iss-control-ui-v1';
const ASSETS=['/','/manifest.webmanifest','/icon-192.png','/icon-512.png'];
self.addEventListener('install',event=>{
  event.waitUntil(caches.open(CACHE).then(cache=>cache.addAll(ASSETS)).then(()=>self.skipWaiting()));
});
self.addEventListener('activate',event=>{
  event.waitUntil(caches.keys().then(keys=>Promise.all(keys.filter(k=>k!==CACHE).map(k=>caches.delete(k)))).then(()=>self.clients.claim()));
});
self.addEventListener('fetch',event=>{
  const u=new URL(event.request.url);
  if(event.request.method!=='GET'||u.pathname.startsWith('/api/'))return;
  event.respondWith(caches.match(event.request).then(r=>r||fetch(event.request)));
});
