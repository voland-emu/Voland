# Dumping Guide

This guide covers how to obtain the files Voland requires from a Nintendo Switch you own. These steps require a Nintendo Switch that can run custom firmware. **This guide is for your own hardware only.**

Voland does not endorse piracy. Do not use this guide to obtain files from hardware you do not own.

---

## What you need

| File | Purpose | Required |
|---|---|---|
| `prod.keys` | Decrypts game files and firmware | Yes |
| `title.keys` | Per-game decryption keys | Yes |
| Game files (NSP / XCI) | Your game dumps | Yes |
| Firmware | Switch OS files | Optional - most games work without it |

---

## Step 1 - Check if your Switch is hackable

Not all Switch consoles can run custom firmware. Your ability to proceed depends on your hardware revision and serial number.

Check your serial number at [ismyswitchpatched.com](https://ismyswitchpatched.com/) before continuing.

| Result | Meaning |
|---|---|
| Definitely patched | Cannot run CFW without a modchip |
| Potentially unpatched | May work - follow the guide at your own risk |
| Definitely unpatched | Can run CFW via software exploit |

Switch Lite and Switch OLED require a modchip regardless of serial number unless a software exploit has been discovered for your firmware version.

---

## Step 2 - Set up custom firmware

Follow the [NH Switch Guide](https://switch.hacks.guide/) - the most maintained and accurate CFW setup guide available.

You will need:
- A microSD card (32GB minimum, 128GB recommended)
- A USB-C cable
- A PC

Do not use other guides - they are frequently outdated and some contain malware.

---

## Step 3 - Dump your keys

Keys are dumped using **Lockpick_RCM**, a payload that runs before the Switch OS boots and extracts cryptographic keys directly from hardware.

### Download

Get the latest release from the [Lockpick_RCM GitHub releases page](https://github.com/s1204IT/Lockpick_RCM/releases). Download `Lockpick_RCM.bin`.

### Run

1. Place `Lockpick_RCM.bin` on your microSD card
2. Boot into RCM mode (hold Vol+ while pressing Power with jig inserted)
3. Inject the payload using your preferred injector (TegraRcmGUI on Windows, fusee-launcher on Linux/macOS)
4. Follow the on-screen prompts - select "Dump from SysNAND"
5. Wait for completion

### Locate the output

Keys are saved to your microSD card at:

```
/switch/prod.keys
/switch/title.keys
```

Copy both files to your PC.

### Provide to Voland

When you first open Voland it will ask you to select your `prod.keys` file. Select it from wherever you saved it on your PC. Voland reads it in place - it is never uploaded or copied anywhere.

---

## Step 4 - Dump your games

### Option A - Cartridge dumps (XCI)

Use **nxdumptool** to dump physical cartridges to XCI files.

1. Download [nxdumptool](https://github.com/DarkMatterCore/nxdumptool/releases/latest) and place it in `/switch/` on your microSD
2. Insert the cartridge you want to dump
3. Launch nxdumptool from the Homebrew Menu
4. Select "Dump gamecard content" → "gamecard image (XCI)"
5. Select "Keep certificate" - No, "Trim output image" - No
6. Wait for the dump to complete (time varies by game size)
7. Find the XCI file in `/dump/XCI/` on your microSD

### Option B - Installed game dumps (NSP)

Use **nxdumptool** to dump games installed from the eShop.

1. Launch nxdumptool from the Homebrew Menu
2. Select "Dump installed SD card / eMMC content"
3. Select the game you want to dump
4. Select "Nintendo Submission Package (NSP)" → "dump base application NSP"
5. Wait for the dump to complete
6. Find the NSP file in `/dump/NSP/` on your microSD

### Updates and DLC

Dump updates and DLC separately using the same nxdumptool flow. Select "dump update NSP" or "dump DLC NSP" from the game's submenu.

### Provide to Voland

When you first open Voland it will ask you to select a games folder. Point it at the folder containing your XCI and NSP files. Voland reads them in place - they are never uploaded or copied into browser storage.

---

## Step 5 - Dump firmware (optional)

Most games run without real firmware - Voland's HLE layer implements the Switch OS services directly. Firmware is only needed for games that depend on specific system behavior that HLE does not yet cover.

If you need firmware, use **Tegra Explorer** or **NXDumpFuse** to dump system firmware from your Switch's NAND. Instructions are on the [NH Switch Guide](https://switch.hacks.guide/).

---

## File format reference

| Format | Source | Notes |
|---|---|---|
| XCI | Cartridge dump | Contains the full game including update if present on cart |
| NSP | eShop / installed dump | Base game, update, and DLC are separate files |
| NRO | Homebrew | Runs without keys - useful for testing |

Voland does not support NSZ or XCZ (compressed formats). Use [nsz](https://github.com/nicoboss/nsz) to decompress them to NSP/XCI before use.

---

## Troubleshooting

**prod.keys is rejected**
Your keys may be outdated if your Switch firmware has been updated since you dumped them. Re-run Lockpick_RCM to get fresh keys.

**Game fails to load**
Ensure you are loading the base game NSP not the update NSP. Load the base game first, then manage updates through Voland's title update manager.

**Keys dump successfully but title.keys is empty**
title.keys is populated when you have played or installed games on the console. If you have not launched the game on the Switch the title key may not be present. Launch the game once on the Switch then re-dump.

**My Switch is patched**
A modchip is required. The most common option is the PicoFly modchip. Installation requires soldering and voids your warranty. This is beyond the scope of this guide.

---

## Legal note

Dumping software from hardware you own for personal use is generally considered lawful under fair use and right-to-repair principles in many jurisdictions. Laws vary by country. You are responsible for understanding the laws in your region. This guide is provided for informational purposes only.

Do not share your keys or game dumps. They are tied to your hardware and sharing them enables piracy.
