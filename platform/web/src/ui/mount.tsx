/**
 * Dynamically imported from main.ts once both workers report "ready".
 * Splitting the Solid runtime into its own chunk keeps the boot path - the
 * part that runs inside a black screen - small and fast.
 */
import { render } from "solid-js/web";
import App from "./App";
import "./shell.css";

export interface MountOptions {
  readonly adapterLabel: string;
  readonly cpuBackend:   string;
  readonly guestRamMiB:  number;
}

export function mountShell(options: MountOptions): void {
  const root = document.getElementById("app-root");
  if (!root) throw new Error("mountShell: #app-root missing from index.html");

  render(() => <App adapterLabel={options.adapterLabel} cpuBackend={options.cpuBackend} guestRamMiB={options.guestRamMiB} />, root);

  const boot = document.getElementById("boot");
  if (boot) {
    boot.classList.add("fade-out");
    window.setTimeout(() => boot.remove(), 260);
  }
}
