# Dumping Guide

This guide covers how to produce the files Voland requires from a Nintendo Switch you own. These steps require a Nintendo Switch that can run custom firmware. **This guide is for your own hardware only.**

Voland does not endorse piracy. Do not use this guide to obtain files from hardware you do not own.

---

## What Voland accepts — and what it does not

Voland loads **decrypted NCA files only**. It never asks for your keys, never reads an NSP or XCI, and contains no decryption code. Decryption is something *you* do, once, with separate tools, before the files ever reach Voland. This is a deliberate legal boundary (see [DESIGN.md §1.6](DESIGN.md#16-legal-scope-boundaries)), not a missing feature, and it will not change.

| File | Who uses it | Required |
|---|---|---|
| `prod.keys`, `title.keys` | **hactool** (on your PC) — never Voland | Yes, for decryption |
| Game dump (NSP / XCI) | **hactool** — never Voland | Yes, as decryption input |
| **Decrypted NCA files** | **Voland** | Yes — this is the only thing Voland reads |

The flow is: Switch → keys + game dump → hactool on your PC → decrypted NCAs → Voland.

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

Keys are dumped using **Lockpick_RCM**, a payload that runs before the Switch OS boots and extracts cryptographic keys directly from hardware. You need them for Step 5 (decryption on your PC). Voland never sees them.

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

Copy both files to your PC. Keep them private; they are tied to your console.

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

Dump updates and DLC separately using the same nxdumptool flow. Select "dump update NSP" or "dump DLC NSP" from the game's submenu. (Update and DLC support in Voland is a later milestone; dump them now if you like, but the base game is what you need first.)

Copy the dump to your PC. **Do not give it to Voland** - it is encrypted, and Voland will refuse it. Continue to Step 5.

---

## Step 5 - Decrypt to NCA on your PC

This is the step that produces the files Voland actually reads. It runs entirely on your PC with **hactool**, a separate open-source tool that is not part of Voland.

### Install hactool

Download a release from the [hactool GitHub page](https://github.com/SciresM/hactool/releases) (or build it from source). Place your `prod.keys` and `title.keys` where hactool looks for them by default:

| OS | Location |
|---|---|
| Windows | `%USERPROFILE%\.switch\prod.keys` and `title.keys` |
| macOS / Linux | `~/.switch/prod.keys` and `title.keys` |

### Extract the NCAs from your dump

Both container formats are plain archives once hactool has the keys:

```bash
# NSP (eShop / installed dump)
hactool -t pfs0 --outdir=extracted game.nsp
```

```bash
# XCI (cartridge dump)
hactool -t xci --secure --outdir=extracted game.xci
```

You will get several `.nca` files. They are still encrypted at this point.

### Decrypt each NCA

```bash
hactool -t nca --plaintext=decrypted/<name>.nca extracted/<name>.nca
```

Run this once per `.nca` in `extracted/`. hactool will tell you each file's content type; the ones that matter are:

| Content type | Contains | Needed |
|---|---|---|
| Program | The game's code (ExeFS) and assets (RomFS) | **Yes** - this is the game |
| Control | Title name, icon, save-data metadata | Recommended |
| Meta | Content manifest | No |
| Manual / LegalInformation | HTML manual | No |

Put the decrypted `.nca` files for a title together in one folder. That folder is what you give to Voland.

### Verify

A correctly decrypted NCA starts with readable text at byte `0x200`: `NCA3`. If Voland reports "the file is encrypted or not an NCA", it is not - re-run the `--plaintext` step and make sure hactool found your keys (it prints a warning if it did not).

---

## Step 6 - Provide to Voland

When you first open Voland it will ask you to select a games folder. Point it at the folder containing your **decrypted `.nca` files**. Voland reads them in place - they are never uploaded or copied into browser storage.

Voland will **not** ask for `prod.keys`, `title.keys`, or any NSP / XCI file. If something claiming to be Voland asks for your keys, it is not Voland.

---

## Firmware

Voland neither requires nor accepts Nintendo firmware. Services that conventionally lean on firmware data (shared fonts, time zones, Mii data) are synthesized from open sources at build time. There is no firmware dumping step.

---

## File format reference

| Format | Source | Voland reads it? |
|---|---|---|
| Decrypted NCA | hactool `--plaintext` output (Step 5) | **Yes** |
| NSP | eShop / installed dump | No - decrypt with hactool first |
| XCI | Cartridge dump | No - decrypt with hactool first |
| NSZ / XCZ | Compressed dumps | No - decompress with [nsz](https://github.com/nicoboss/nsz) to NSP/XCI, then decrypt |
| NRO | Homebrew | Yes (Phase 2) - runs without keys, useful for testing |

---

## Troubleshooting

**Voland says "the file is encrypted or not an NCA"**
The file has not been decrypted, or hactool ran without finding your keys. Re-run the Step 5 `--plaintext` command and check hactool's output for a missing-keys warning.

**Voland says "the NCA header is plaintext but its sections are still encrypted"**
hactool decrypted the header but could not decrypt the content, almost always because `title.keys` is missing the key for that title. See the `title.keys` note below, then re-run Step 5.

**hactool reports missing or outdated keys**
Your keys may be outdated if your Switch firmware has been updated since you dumped them. Re-run Lockpick_RCM to get fresh keys.

**Keys dump successfully but title.keys is empty**
`title.keys` is populated when you have played or installed games on the console. If you have not launched the game on the Switch the title key may not be present. Launch the game once on the Switch then re-dump.

**Game fails to load**
Ensure you decrypted and are loading the base game's Program NCA, not an update's. Load the base game first.

**My Switch is patched**
A modchip is required. The most common option is the PicoFly modchip. Installation requires soldering and voids your warranty. This is beyond the scope of this guide.

---

## Legal note

Dumping software from hardware you own for personal use is generally considered lawful under fair use and right-to-repair principles in many jurisdictions. Laws vary by country. You are responsible for understanding the laws in your region. This guide is provided for informational purposes only.

Do not share your keys or game dumps. They are tied to your hardware and sharing them enables piracy.
