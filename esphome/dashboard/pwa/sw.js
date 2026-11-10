/* Rinnai PWA UI only. Commands and sensor data always use the network. */
'use strict';
const VERSION = "1.2.0";
const CACHE_PREFIX = 'rinnai-ui-';
const ASSETS = [{"path":"/","types":["text/html"]},{"path":"/app","types":["text/html"],"integrity":"sha256-f5wMbGywc8+7Q7K0SZe101vYnYg12HcX42Rig6WRhok="},{"path":"/app/","types":["text/html"],"integrity":"sha256-f5wMbGywc8+7Q7K0SZe101vYnYg12HcX42Rig6WRhok="},{"path":"/0.js","types":["text/javascript","application/javascript"],"integrity":"sha256-A8lPm8pOeTuvbAYOVBYQFoen/dZN1yj+loFEkd3AXRk="},{"path":"/0.css","types":["text/css"],"integrity":"sha256-labNkEr7IyknYgm7K3rSGCFg0H4aIm6OWIIqJmtDPZs="},{"path":"/pwa.js","types":["text/javascript","application/javascript"],"integrity":"sha256-A8aaleHehCj0tG/1znE2ADQzz7zeTcwRD1YSD/dmRUQ="},{"path":"/pwa.css","types":["text/css"],"integrity":"sha256-vQpe+9e9fERojlKcbmM7tDDmogNEq0Es5Hbzi4GO1ws="},{"path":"/manifest.webmanifest","types":["application/manifest+json","application/json"],"integrity":"sha256-B2nG8GD5tLdJ0ebahCCkiS0PdLdZhRxTJtAEOG28wJc="},{"path":"/favicon.svg","types":["image/svg+xml"],"integrity":"sha256-xmdxFPdcD4XjgETc3adBFy/2jpJGqlvtGM3jO1FPwKA="},{"path":"/favicon.ico","types":["image/x-icon","image/vnd.microsoft.icon"],"integrity":"sha256-bqIfvzE5Zhuw+30vRRvxDsJlRWpYQqsM9HM+lX/v00A="},{"path":"/apple-touch-icon.png","types":["image/png"],"integrity":"sha256-hAm4Fb+G8lk4z1PstmpNmdTuOx70AAv2q+WJN8TyR6o="},{"path":"/icons/apple-touch-icon.png","types":["image/png"],"integrity":"sha256-hAm4Fb+G8lk4z1PstmpNmdTuOx70AAv2q+WJN8TyR6o="},{"path":"/icons/icon-192.png","types":["image/png"],"integrity":"sha256-kzN/W7kNSxZMCwx3ZRxcM5AhghRBhFjgVYdDOVQhErA="},{"path":"/icons/icon-512.png","types":["image/png"],"integrity":"sha256-WyPVBB7W1CAHrIYp8IKYoEU+u6MMSkv7JghmUIBr6+Y="},{"path":"/icons/maskable-512.png","types":["image/png"],"integrity":"sha256-d3PLU6rp24GrQWpW8+5aDtaDNR1tNzESYUJCJQ62uCU="}];
const CACHE_NAME = CACHE_PREFIX + VERSION + '-' + "c3f2746518f3c953";
const byPath = new Map(ASSETS.map(asset => [asset.path, asset]));

function acceptable(response, asset) {
  if (!response || response.status !== 200 || response.type !== 'basic' || response.redirected) return false;
  const type = (response.headers.get('Content-Type') || '').split(';', 1)[0].trim().toLowerCase();
  return asset.types.includes(type);
}

function requestFor(asset) {
  const options = { cache: 'no-store', credentials: 'same-origin', redirect: 'error' };
  if (asset.integrity) options.integrity = asset.integrity;
  return new Request(new URL(asset.path, self.location.origin), options);
}

async function loadAsset(asset) {
  // Fetch integrity pins static bytes to this worker's firmware version.
  // A new firmware cannot replace only part of an older offline UI cache.
  const response = await fetch(requestFor(asset));
  if (!acceptable(response, asset)) throw new Error('Unexpected static asset: ' + asset.path);
  return response;
}

self.addEventListener('install', event => {
  event.waitUntil((async () => {
    const existed = (await caches.keys()).includes(CACHE_NAME);
    const cache = await caches.open(CACHE_NAME);
    try {
      // Sequential requests respect the ESP32's small HTTP connection budget.
      for (const asset of ASSETS) await cache.put(asset.path, await loadAsset(asset));
    } catch (error) {
      if (!existed) await caches.delete(CACHE_NAME);
      throw error;
    }
    // Keep the normal waiting lifecycle. Never reload a live control screen.
  })());
});

self.addEventListener('activate', event => {
  event.waitUntil((async () => {
    for (const name of await caches.keys()) {
      if (name.startsWith(CACHE_PREFIX) && name !== CACHE_NAME) await caches.delete(name);
    }
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', event => {
  const request = event.request;
  // Do not intercept POST, heater APIs, cross-origin traffic, or unknown URLs.
  // There is deliberately no command queue, background sync, or API fallback.
  if (request.method !== 'GET') return;
  const url = new URL(request.url);
  if (url.origin !== self.location.origin || url.pathname.startsWith('/rinnai/api/')) return;
  const asset = byPath.get(url.pathname);
  if (!asset) return;
  // Queries are allowed for a root navigation and versioned static icon links.
  // The cache key remains the allowlisted path; arbitrary requests are never stored.
  event.respondWith((async () => {
    const cache = await caches.open(CACHE_NAME);
    try {
      const response = await loadAsset(asset);
      // Cache writes must not turn a successful network response into an error.
      event.waitUntil(cache.put(asset.path, response.clone()).catch(() => {}));
      return response;
    } catch (error) {
      const cached = await cache.match(asset.path);
      if (acceptable(cached, asset)) return cached;
      throw error;
    }
  })());
});
