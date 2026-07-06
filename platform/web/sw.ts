/**
 * Service Worker (docs/DESIGN.md section 16). Two jobs:
 *
 * 1. Re-inject COOP/COEP/CSP on every served response. Real deployments
 *    should set these at the server/hosting-config level; the SW path is
 *    the fallback for self-hosters who can't - and requires one reload on
 *    first visit before it can control the page (handled in main.ts).
 * 2. Cache strategy: app shell cache-first (navigations always resolve to
 *    the cached shell, so client-side routing survives a hard refresh at
 *    e.g. /game/:titleId), `.wasm` aggressively cache-first, `/api/`
 *    network-only, everything else network-first.
 *
 * Phase 0 has no build-time precache manifest (that needs a bundler
 * plugin, e.g. vite-plugin-pwa, which is not wired up yet) - caches are
 * filled lazily as requests are served rather than precached at install.
 */

const sw: ServiceWorkerGlobalScope = globalThis as unknown as ServiceWorkerGlobalScope;

const SHELL_CACHE = "voland-shell-v1";
const WASM_CACHE = "voland-wasm-v1";
const SHELL_URL = "/index.html";

/* COOP must be strict "same-origin" - "same-origin-allow-popups" never
 * yields crossOriginIsolated in shipping Chromium regardless of COEP (see
 * vite.config.ts for the full explanation and docs/DESIGN.md §16). */
const CROSS_ORIGIN_ISOLATION_HEADERS: ReadonlyArray<readonly [string, string]> = [
  ["Cross-Origin-Opener-Policy", "same-origin"],
  ["Cross-Origin-Embedder-Policy", "require-corp"],
  ["Content-Security-Policy", "frame-ancestors 'none'"],
];

function addCrossOriginIsolationHeaders(response: Response): Response {
  const headers = new Headers(response.headers);
  for (const [key, value] of CROSS_ORIGIN_ISOLATION_HEADERS) {
    headers.set(key, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

async function cacheFirst(request: Request, cacheName: string): Promise<Response> {
  const cache = await sw.caches.open(cacheName);
  const cached = await cache.match(request);
  if (cached) return addCrossOriginIsolationHeaders(cached);

  const response = await fetch(request);
  if (response.ok) await cache.put(request, response.clone());
  return addCrossOriginIsolationHeaders(response);
}

async function networkFirst(request: Request): Promise<Response> {
  const cache = await sw.caches.open(SHELL_CACHE);
  try {
    const response = await fetch(request);
    if (response.ok) await cache.put(request, response.clone());
    return addCrossOriginIsolationHeaders(response);
  } catch (e) {
    const cached = await cache.match(request);
    if (cached) return addCrossOriginIsolationHeaders(cached);
    throw e;
  }
}

sw.addEventListener("install", () => {
  sw.skipWaiting();
});

sw.addEventListener("activate", (event: ExtendableEvent) => {
  event.waitUntil(sw.clients.claim());
});

sw.addEventListener("fetch", (event: FetchEvent) => {
  const url = new URL(event.request.url);
  if (url.origin !== location.origin) return; // let cross-origin requests through untouched

  if (url.pathname.startsWith("/api/")) {
    event.respondWith(fetch(event.request).then(addCrossOriginIsolationHeaders));
    return;
  }

  if (url.pathname.endsWith(".wasm")) {
    event.respondWith(cacheFirst(event.request, WASM_CACHE));
    return;
  }

  if (event.request.mode === "navigate") {
    // Navigation API + client-side routing (§16): every navigate request
    // resolves to the cached shell so a hard refresh at any route works.
    event.respondWith(cacheFirst(new Request(SHELL_URL), SHELL_CACHE));
    return;
  }

  event.respondWith(networkFirst(event.request));
});
