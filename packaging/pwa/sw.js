// Offline cache for the engine files only. Game content never goes here.
//
// Cache first: an installed app starts from the copy on the device without
// waiting for the network (a launch used to re-download ~40 MB, or hang on a
// LAN server it could not reach). Each build writes version.json; launching
// checks it in the background and downloads a newer build into its own
// cache, which the next launch uses. A page keeps the build it started with,
// so the engine files it loads always match each other.
const ENGINE = ['./', 'index.html', 'manifest.webmanifest', 'mp6native.js', 'mp6native.wasm', 'mp6native.data',
                'icons/icon-180.png', 'icons/icon-192.png', 'icons/icon-512.png', 'icons/icon-maskable-512.png'];
const META = 'mp6-meta';
const PREFIX = 'mp6-engine-';
const scoped = (path) => new URL(path, self.registration.scope).href;

let current = null;
async function currentVersion() {
  if (current === null) {
    const hit = await (await caches.open(META)).match('current');
    current = hit ? await hit.text() : '';
  }
  return current;
}
async function setCurrent(v) {
  await (await caches.open(META)).put('current', new Response(v));
  current = v;
}

function fetchWithin(url, ms, init = {}) {
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), ms);
  return fetch(url, { ...init, signal: abort.signal }).finally(() => clearTimeout(timer));
}


async function remoteVersion(ms) {
  const res = await fetchWithin(scoped('version.json'), ms, { cache: 'no-store' });
  if (!res.ok) throw new Error('version.json ' + res.status);
  const info = await res.json();
  return { v: String(info.v), bytes: Number(info.bytes) || 0 };
}

async function tell(msg) {
  for (const c of await self.clients.matchAll({ includeUncontrolled: true })) c.postMessage(msg);
}

// Download every engine file of build v into its cache, reporting progress.
async function download({ v, bytes: total }) {
  const cache = await caches.open(PREFIX + v);
  let done = 0;
  for (const path of ENGINE) {
    if (await cache.match(scoped(path))) continue;
    // The timeout covers the whole body, not just the headers: a stalled
    // read used to hang this download (and every later update check of this
    // worker) for good. 20 s without a byte aborts it.
    const abort = new AbortController();
    let stall = setTimeout(() => abort.abort(), 20000);
    const res = await fetch(scoped(path), { cache: 'no-store', signal: abort.signal });
    if (!res.ok) { clearTimeout(stall); throw new Error(path + ' ' + res.status); }
    // Read it in chunks so the page can show how far along the update is.
    const reader = res.body.getReader();
    const chunks = [];
    let last = 0;
    for (;;) {
      const { done: end, value } = await reader.read();
      clearTimeout(stall);
      if (end) break;
      stall = setTimeout(() => abort.abort(), 20000);
      chunks.push(value);
      done += value.length;
      if (done - last > 1 << 20) { last = done; tell({ mp6Update: 'progress', v, done, total: Math.max(total, done) }); }
    }
    await cache.put(scoped(path), new Response(new Blob(chunks), { headers: res.headers }));
  }
}

// Drop engine caches other than the current build and the one just before
// it (a page that is still open may be using it).
async function prune(keep) {
  for (const k of await caches.keys())
    if (k.startsWith(PREFIX) && !keep.includes(k.slice(PREFIX.length))) await caches.delete(k);
}

let checking = null;
function checkForUpdate() {
  if (!checking) checking = (async () => {
    let remote;
    // Offline or server away: keep the cached build. Either way the page
    // that is waiting to boot hears the answer at once.
    try { remote = await remoteVersion(5000); } catch { tell({ mp6Update: 'offline' }); return; }
    const v = remote.v, old = await currentVersion();
    if (v === old) { tell({ mp6Update: 'current', v }); return; }
    tell({ mp6Update: 'downloading', v });
    const t0 = Date.now();
    try {
      await download(remote);
    } catch (e) {
      tell({ mp6Update: 'failed', v, error: String(e) });
      return;
    }
    await setCurrent(v);
    await prune([v, old]);
    tell({ mp6Update: 'ready', v });
  })().finally(() => { checking = null; });
  return checking;
}

self.addEventListener('install', (event) => {
  // The first install needs one full download; later service-worker updates
  // keep the build that is already cached.
  event.waitUntil((async () => {
    try {
      if (!(await currentVersion())) {
        const remote = await remoteVersion(15000);
        await download(remote);
        await setCurrent(remote.v);
      }
    } catch (e) {
      throw e;
    }
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const v = await currentVersion();
    // Older service workers' caches (and builds before the current one).
    for (const k of await caches.keys())
      if (k !== META && k !== PREFIX + v) await caches.delete(k);
    await self.clients.claim();
  })());
});

// The build each open page started with (by client id).
const pageVersion = new Map();

self.addEventListener('message', (event) => {
  if (event.data === 'mp6-version') {
    currentVersion().then((v) => event.source && event.source.postMessage({ mp6Version: v }));
  } else if (event.data === 'mp6-check-update') {
    event.waitUntil(checkForUpdate());
  }
});

self.addEventListener('fetch', (event) => {
  const req = event.request;
  const url = new URL(req.url);
  if (req.method !== 'GET' || url.origin !== location.origin) return;
  const path = url.pathname;
  if (path.endsWith('/version.json') || path.endsWith('/sw.js')) return;
  const navigate = req.mode === 'navigate';
  event.respondWith((async () => {
    let v = await currentVersion();
    if (navigate) {
      if (event.resultingClientId) pageVersion.set(event.resultingClientId, v);
    } else if (pageVersion.has(event.clientId)) {
      v = pageVersion.get(event.clientId);
    }
    if (v) {
      const cache = await caches.open(PREFIX + v);
      const hit = await cache.match(navigate ? scoped('index.html') : req, { ignoreSearch: true });
      if (hit) return hit;
    }
    // Not part of the cached engine (or nothing cached yet): the network.
    return fetch(req);
  })());
  if (navigate) event.waitUntil(checkForUpdate());
});
