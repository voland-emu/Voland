/**
 * Feature detection (see docs/DESIGN.md section 16, "Progressive
 * enhancement"). Phase 0 reports capabilities up front so boot-blocking
 * problems (no COOP/COEP, no WebGPU, no OPFS) surface as user-visible
 * errors rather than silent failures deep in a worker.
 */

export interface PlatformCapabilities {
  readonly webGPU:                 boolean;
  readonly opfs:                   boolean;
  readonly crossOriginIsolated:    boolean;
  readonly filesystemAccessApi:    boolean;
  readonly webHID:                 boolean;
  readonly webNFC:                 boolean;
  readonly webCodecs:              boolean;
  readonly screenWakeLock:         boolean;
  readonly vibration:              boolean;
}

export function detectCapabilities(): PlatformCapabilities {
  const storage = (navigator as Navigator & { storage?: { getDirectory?: unknown } }).storage;
  return {
    webGPU:              "gpu" in navigator,
    opfs:                storage !== undefined && "getDirectory" in storage,
    crossOriginIsolated: typeof crossOriginIsolated === "boolean" ? crossOriginIsolated : false,
    filesystemAccessApi: "showDirectoryPicker" in window,
    webHID:              "hid" in navigator,
    webNFC:              "NDEFReader" in window,
    webCodecs:           "VideoEncoder" in window,
    screenWakeLock:      "wakeLock" in navigator,
    vibration:           "vibrate" in navigator,
  };
}
