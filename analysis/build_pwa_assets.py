"""Build the local PWA manifest, vector-derived icons and version-pinned worker.

Run after dashboard.js/css and pwa.js/css are final, then embed the outputs.
No network or heater requests are performed by this generator.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
DASHBOARD = ROOT / "esphome/dashboard"
OUTPUT = DASHBOARD / "pwa"
VERSION = "1.2.0"
SW_TEMPLATE = r"""/* Rinnai PWA UI only. Commands and sensor data always use the network. */
'use strict';
const VERSION = __VERSION__;
const CACHE_PREFIX = 'rinnai-ui-';
const ASSETS = __ASSETS__;
const CACHE_NAME = CACHE_PREFIX + VERSION + '-' + __BUNDLE_HASH__;
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
"""

SW_TEST = r"""
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const code = fs.readFileSync(process.argv[2], 'utf8');
let checks = 0;
const ok = (condition, message) => { checks++; assert.ok(condition, message); };
function environment() {
  const handlers = {}, stores = new Map(), requests = [];
  const state = { fail: false, failPath: null, bad: null, active: 0, peak: 0, claims: 0 };
  const cacheStorage = {
    keys: async () => [...stores.keys()],
    delete: async name => stores.delete(name),
    open: async name => {
      if (!stores.has(name)) stores.set(name, new Map());
      const store = stores.get(name);
      return { put: async (path, response) => { store.set(path, response.clone()); },
        match: async path => store.get(path)?.clone() };
    }
  };
  function response(asset, changes = {}) {
    return { status: 200, type: 'basic', redirected: false, path: asset.path,
      headers: new Headers({ 'Content-Type': asset.types[0] }),
      ...changes, clone() { return response(asset, changes); } };
  }
  const scope = { location: { origin: 'https://heater.example' },
    addEventListener: (name, callback) => { handlers[name] = callback; },
    skipWaiting: () => { throw new Error('Must not skip the waiting lifecycle'); },
    clients: { claim: async () => { state.claims++; } } };
  const context = { self: scope, caches: cacheStorage, Request, URL, Headers, Map, console,
    fetch: async request => {
      requests.push(request); state.active++; state.peak = Math.max(state.peak, state.active);
      try {
        await Promise.resolve();
        const path = new URL(request.url).pathname;
        if (state.fail || path === state.failPath) throw new Error('Offline or integrity mismatch');
        const asset = scope.__test.ASSETS.find(item => item.path === path);
        if (!asset) throw new Error('Non-static fetch');
        if (asset.integrity) ok(request.integrity === asset.integrity, 'SRI must be passed to fetch');
        ok(request.cache === 'no-store', 'Network request must bypass the HTTP cache');
        return response(asset, state.bad || {});
      } finally { state.active--; }
    } };
  vm.runInNewContext(code + '\nself.__test = { CACHE_NAME, ASSETS };', context);
  async function event(name, fields = {}) {
    const waits = [];
    let responded = false, returned;
    handlers[name]({ ...fields,
      waitUntil: promise => { waits.push(Promise.resolve(promise)); },
      respondWith: promise => { responded = true; returned = Promise.resolve(promise); } });
    if (returned) returned = await returned;
    for (let index = 0; index < waits.length; index++) await waits[index];
    return { responded, response: returned };
  }
  return { scope, stores, state, requests, event,
    request: (path, method = 'GET') => new Request(new URL(path, scope.location.origin), { method }) };
}
(async () => {
  const env = environment(), assets = env.scope.__test.ASSETS, name = env.scope.__test.CACHE_NAME;
  ok(assets.some(asset => asset.path === '/app'), 'Dedicated app shell is cached');
  ok(assets.every(asset => asset.path === '/' || /^sha256-/.test(asset.integrity)), 'Every non-legacy static asset is version-pinned');
  await env.event('install');
  ok(env.state.peak === 1, 'Precache requests must be sequential');
  ok(env.stores.get(name).size === assets.length, 'Complete static bundle was cached');
  ok(env.state.claims === 0, 'Install must not claim an existing control screen');
  const bypass = [ ['/rinnai/api/state'], ['/rinnai/api/history'], ['/rinnai/api/periods'],
    ['/rinnai/api/diagnostics'], ['/rinnai/api/temperature', 'POST'], ['/rinnai/api/settings', 'POST'],
    ['/rinnai/api/preferences', 'POST'], ['/app', 'POST'], ['https://elsewhere.example/app'], ['/unknown'] ];
  const before = env.requests.length;
  for (const [path, method] of bypass) {
    const result = await env.event('fetch', { request: env.request(path, method) });
    ok(!result.responded, 'Non-static request must bypass the worker: ' + path);
  }
  ok(env.requests.length === before, 'Worker did not fetch, cache or replay bypassed requests');
  env.state.fail = true;
  for (const path of ['/app', '/app/', '/', '/0.js', '/0.css', '/apple-touch-icon.png']) {
    const result = await env.event('fetch', { request: env.request(path) });
    ok(result.responded && result.response.path === path, 'Static offline fallback: ' + path);
  }
  env.state.fail = false;
  const queried = await env.event('fetch', { request: env.request('/app?source=home') });
  ok(queried.response.path === '/app', 'Navigation query resolves only the canonical app shell');
  ok(!env.stores.get(name).has('/app?source=home'), 'Queries do not create arbitrary cache entries');
  for (const bad of [{ status: 500 }, { type: 'opaque' }, { redirected: true },
                    { headers: new Headers({ 'Content-Type': 'text/html' }) }]) {
    env.state.bad = bad;
    const result = await env.event('fetch', { request: env.request('/0.css') });
    ok(result.response.status === 200 && result.response.type === 'basic' && !result.response.redirected,
      'Invalid network response must not replace a usable cached asset');
    ok(result.response.headers.get('Content-Type') === 'text/css', 'HTML must never poison the CSS cache');
  }
  env.state.bad = null;
  env.stores.set('rinnai-ui-obsolete', new Map());
  env.stores.set('unrelated-app-cache', new Map());
  await env.event('activate');
  ok(!env.stores.has('rinnai-ui-obsolete'), 'Obsolete own cache is removed');
  ok(env.stores.has('unrelated-app-cache') && env.stores.has(name), 'Activation preserves foreign and current caches');
  ok(env.state.claims === 1, 'Activation may claim clients without reloading them');
  const broken = environment();
  broken.stores.set('rinnai-ui-previous', new Map([['/app', { sentinel: true }]]));
  broken.state.failPath = '/0.css';
  let rejected = false;
  try { await broken.event('install'); } catch (_) { rejected = true; }
  ok(rejected && !broken.stores.has(broken.scope.__test.CACHE_NAME), 'Failed precache cannot activate a partial new bundle');
  ok(broken.stores.has('rinnai-ui-previous'), 'Failed install preserves the older usable bundle');
  env.state.failPath = '/0.css';
  try { await env.event('install'); } catch (_) {}
  ok(env.stores.get(name).size === assets.length, 'Failed same-bundle reinstall preserves the existing complete cache');
  console.log(JSON.stringify({ passed: true, checks, static_assets: assets.length, scope: 'UI only; API and POST bypassed' }));
})().catch(error => { console.error(error); process.exit(1); });
"""


def cubic(start, control1, control2, end, samples=96):
    for index in range(samples + 1):
        t = index / samples
        u = 1 - t
        yield tuple(u ** 3 * start[axis] + 3 * u * u * t * control1[axis]
                    + 3 * u * t * t * control2[axis] + t ** 3 * end[axis]
                    for axis in (0, 1))


def source_curves(svg: ET.Element):
    """Read the existing favicon's M/c/s cubic paths without changing its source."""
    ns = {"svg": "http://www.w3.org/2000/svg"}
    for node in svg.findall(".//svg:path", ns):
        tokens = re.findall(r"[Mcs]|-?\d+(?:\.\d+)?", node.attrib["d"])
        position, last_control = (0.0, 0.0), None
        points, index = [], 0
        while index < len(tokens):
            command = tokens[index]
            index += 1
            count = {"M": 2, "c": 6, "s": 4}[command]
            values = list(map(float, tokens[index:index + count]))
            index += count
            if command == "M":
                if points:
                    yield points
                points = []
                position = tuple(values)
                points.append(position)
                last_control = None
                continue
            start = position
            if command == "c":
                control1 = (start[0] + values[0], start[1] + values[1])
                control2 = (start[0] + values[2], start[1] + values[3])
                end = (start[0] + values[4], start[1] + values[5])
            else:
                control1 = (2 * start[0] - last_control[0], 2 * start[1] - last_control[1])
                control2 = (start[0] + values[0], start[1] + values[1])
                end = (start[0] + values[2], start[1] + values[3])
            points.extend(list(cubic(start, control1, control2, end))[1:])
            position, last_control = end, control2
        yield points


def render_icons():
    source = ET.fromstring((DASHBOARD / "favicon.svg").read_text(encoding="utf-8"))
    ns = {"svg": "http://www.w3.org/2000/svg"}
    background = source.find("svg:rect", ns).attrib["fill"]
    group = source.find("svg:g", ns)
    stroke, stroke_width = group.attrib["stroke"], float(group.attrib["stroke-width"])
    viewbox = list(map(float, source.attrib["viewBox"].split()))
    assert viewbox == [0, 0, 46, 46], "Review the renderer when changing the source viewBox"
    curves = list(source_curves(source))
    assert len(curves) == 3 and all(len(curve) > 100 for curve in curves)
    specs = (('icon-192.png', 192, 1), ('icon-512.png', 512, 1),
             ('maskable-512.png', 512, .82), ('apple-touch-icon.png', 180, 1))
    icons = OUTPUT / "icons"
    icons.mkdir(parents=True, exist_ok=True)
    details = []
    for filename, size, foreground_scale in specs:
        supersample = 3
        ratio = size * supersample / 46
        image = Image.new("RGB", (size * supersample, size * supersample), background)
        draw = ImageDraw.Draw(image)
        width = max(1, round(stroke_width * ratio * foreground_scale))
        for curve in curves:
            points = [((x - 23) * foreground_scale * ratio + size * supersample / 2,
                       (y - 23) * foreground_scale * ratio + size * supersample / 2)
                      for x, y in curve]
            radius = width / 2
            left, right = [], []
            for index, (x, y) in enumerate(points):
                previous = points[max(0, index - 1)]
                following = points[min(len(points) - 1, index + 1)]
                dx, dy = following[0] - previous[0], following[1] - previous[1]
                length = (dx * dx + dy * dy) ** .5
                nx, ny = -dy / length * radius, dx / length * radius
                left.append((x + nx, y + ny))
                right.append((x - nx, y - ny))
            draw.polygon(left + right[::-1], fill=stroke)
            for x, y in (points[0], points[-1]):
                draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=stroke)
        image = image.resize((size, size), Image.Resampling.LANCZOS)
        # A small opaque palette preserves antialiased vector edges and keeps
        # four launcher icons inexpensive in the ESP32's firmware flash.
        image = image.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        destination = icons / filename
        image.save(destination, optimize=True)
        with Image.open(destination) as check:
            assert check.size == (size, size) and check.convert("RGBA").getextrema()[3] == (255, 255)
        details.append({"file": str(destination.relative_to(ROOT)), "bytes": destination.stat().st_size,
                        "width": size, "height": size, "opaque": True})
    return details


def manifest():
    return {
        "id": "/app", "name": "Rinnai · Água no seu ponto", "short_name": "Aquecedor",
        "description": "Temperatura, uso e consumo do seu aquecedor na rede local.",
        "lang": "pt-BR", "dir": "ltr", "start_url": "/app", "scope": "/",
        "display": "standalone", "background_color": "#f5f5f0", "theme_color": "#192a38",
        "prefer_related_applications": False,
        "icons": [
            {"src": "/icons/icon-192.png", "sizes": "192x192", "type": "image/png", "purpose": "any"},
            {"src": "/icons/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any"},
            {"src": "/icons/maskable-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable"},
        ],
    }


def integrity(path: Path):
    data = path.read_bytes()
    if path.name in ('dashboard.js', 'dashboard.css'):
        # ESPHome reads these as UTF-8 text with universal newline translation
        # before gzip embedding, so CRLF source files are served as LF bytes.
        data = path.read_text(encoding='utf-8').encode('utf-8')
    # The existing embedded favicon SVG strips its file's trailing newline.
    if path.name == 'favicon.svg':
        data = data.rstrip(b'\r\n')
    digest = hashlib.sha256(data).digest()
    return "sha256-" + base64.b64encode(digest).decode("ascii")


def worker_assets():
    types = {
        "/": ["text/html"],
        "/app": ["text/html"],
        "/app/": ["text/html"],
        "/0.js": ["text/javascript", "application/javascript"],
        "/0.css": ["text/css"],
        "/pwa.js": ["text/javascript", "application/javascript"],
        "/pwa.css": ["text/css"],
        "/manifest.webmanifest": ["application/manifest+json", "application/json"],
        "/favicon.svg": ["image/svg+xml"],
        "/favicon.ico": ["image/x-icon", "image/vnd.microsoft.icon"],
        "/apple-touch-icon.png": ["image/png"],
    }
    files = {"/0.js": DASHBOARD / "dashboard.js", "/0.css": DASHBOARD / "dashboard.css",
             "/pwa.js": DASHBOARD / "pwa.js", "/pwa.css": DASHBOARD / "pwa.css",
             "/manifest.webmanifest": OUTPUT / "manifest.webmanifest",
             "/favicon.svg": DASHBOARD / "favicon.svg", "/favicon.ico": DASHBOARD / "favicon.ico",
             "/app": OUTPUT / "app.html", "/app/": OUTPUT / "app.html",
             "/apple-touch-icon.png": OUTPUT / "icons/apple-touch-icon.png"}
    for path in sorted((OUTPUT / "icons").glob("*.png")):
        route = "/icons/" + path.name
        types[route] = ["image/png"]
        files[route] = path
    result = []
    for route, content_types in types.items():
        item = {"path": route, "types": content_types}
        if route in files:
            if not files[route].is_file():
                raise SystemExit(f"Build the dashboard UI first; missing {files[route]}")
            item["integrity"] = integrity(files[route])
        result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--icons-only", action="store_true", help="Build icons/manifest before the UI files are final")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    icons = render_icons()
    destination = OUTPUT / "manifest.webmanifest"
    destination.write_text(json.dumps(manifest(), ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    result = {"version": VERSION, "icons": icons, "manifest_bytes": destination.stat().st_size}
    if not args.icons_only:
        assets = worker_assets()
        encoded_assets = json.dumps(assets, separators=(",", ":"))
        bundle_hash = hashlib.sha256(encoded_assets.encode('utf-8')).hexdigest()[:16]
        worker = SW_TEMPLATE.replace("__VERSION__", json.dumps(VERSION)).replace("__ASSETS__", encoded_assets).replace("__BUNDLE_HASH__", json.dumps(bundle_hash))
        worker_path = OUTPUT / "sw.js"
        worker_path.write_text(worker, encoding="utf-8")
        subprocess.run(["node", "--check", str(worker_path)], check=True)
        checked = subprocess.run(["node", "-", str(worker_path)], input=SW_TEST, text=True,
                                 capture_output=True, check=True)
        result.update(worker_bytes=worker_path.stat().st_size, pinned_assets=len(assets) - 1,
                      service_worker_tests=json.loads(checked.stdout))
    result["total_asset_bytes"] = sum(path.stat().st_size for path in OUTPUT.rglob("*") if path.is_file())
    result["passed"] = True
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
