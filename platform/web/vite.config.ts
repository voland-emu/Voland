import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import solid from "vite-plugin-solid";

/** Mirrors tsconfig.json's "paths" - Rollup/Vite don't read tsconfig
 * path mapping on their own, so the alias has to be declared twice. */
const alias = {
  "@":         fileURLToPath(new URL("./src", import.meta.url)),
  "@bindings": fileURLToPath(new URL("./bindings", import.meta.url)),
  "@workers":  fileURLToPath(new URL("./workers", import.meta.url)),
};

/**
 * Cross-origin isolation is mandatory - the shared memory64 WebAssembly.
 * Memory (§4) is gated behind COOP + COEP. See docs/DESIGN.md section 16
 * ("Required HTTP headers"). Vite's preview and dev servers both need
 * these headers or the app fails at boot with `!crossOriginIsolated`.
 *
 * COOP must be the strict `same-origin`, not `same-origin-allow-popups`:
 * verified directly against shipping Chromium that the popups variant
 * never yields `crossOriginIsolated`, no matter what COEP is set to. An
 * earlier revision used the popups variant for OAuth opener retention
 * (§15) - unnecessary, since that flow already goes through
 * BroadcastChannel rather than `window.opener` (§16). Do not swap this
 * back; `npm run e2e` asserts `crossOriginIsolated === true` specifically
 * to catch that regression.
 */
const crossOriginIsolationHeaders = {
  "Cross-Origin-Opener-Policy":   "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Content-Security-Policy":     "frame-ancestors 'none'",
};

export default defineConfig({
  plugins: [solid()],
  resolve: {
    alias,
  },
  server: {
    headers: crossOriginIsolationHeaders,
    port:    5173,
  },
  preview: {
    headers: crossOriginIsolationHeaders,
    port:    5174,
  },
  worker: {
    format: "es",
  },
  build: {
    target: "es2022",
    sourcemap: true,
    rollupOptions: {
      input: {
        main: fileURLToPath(new URL("./index.html", import.meta.url)),
        // A Service Worker's own URL must be stable, so it gets its own
        // unhashed entry rather than going through the normal asset chunks.
        sw: fileURLToPath(new URL("./sw.ts", import.meta.url)),
      },
      output: {
        entryFileNames: (chunk) => (chunk.name === "sw" ? "sw.js" : "assets/[name]-[hash].js"),
      },
    },
  },
});
