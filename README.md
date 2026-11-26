# KaiVesper

**Native Switch 2 hbmenu application (`kaivesper.nro`) for patched kaiHEN** — by **sank-kab** *(Beta)*

> [!CAUTION]
> ### Anti-Piracy & Legal Disclaimer
> **KaiVesper does NOT support, facilitate, or condone software piracy.**
> - This project contains **zero** Nintendo proprietary code, keys, firmware, signed binaries, or copyrighted game assets.
> - KaiVesper **cannot** decrypt encrypted `.nsp`, `.xci`, `.nca`, or DRM-protected packages and includes **no** DRM-bypass, key-derivation, or signature-patching functionality.
> - This software is intended strictly for **homebrew development, modding, and loading unencrypted files dumped legally from software you personally own** on your own console.
> - "Nintendo" and "Nintendo Switch" are registered trademarks of Nintendo Co., Ltd. This project is an independent open-source effort and is not affiliated with or endorsed by Nintendo.

> [!WARNING]
> ### Console Safety, Save Data & No-Warranty Disclaimer
> **USE ENTIRELY AT YOUR OWN RISK.**
> - KaiVesper operates in RAM on a live homebrew environment (`kaiHEN`) and interacts with Switch 2 filesystem redirection and save-data mounting (`save:/`).
> - While KaiVesper does **not** write to NAND system partitions, **beta homebrew software can crash, freeze, corrupt save data, trigger unexpected system errors, or potentially render your console unstable or unusable ("brick")** if misused or combined with incompatible firmware/homebrew.
> - Always maintain independent backups of your important save data and microSD card. Never use your primary Nintendo Account online while running homebrew modifications.
> - This software is provided **"AS IS"**, without warranty of any kind, express or implied, as detailed in the GPL-2.0 license. The author (`sank-kab`) assumes **zero liability** for any console damage, bricking, ban, or data loss.

---

## Overview

**KaiVesper** is a standalone native Switch 2 homebrew application (`1280×720` anti-aliased GUI with D-Pad, Analog Stick, and Touchscreen support) that manages unencrypted **Switch 2 (`SW2`)** and **Switch 1 (`SW1`)** `ExeFS` + `RomFS` profiles directly from the console — with **zero-reboot Hot-Swapping**, an automatic **Console Host scanner**, built-in **on-console `romfs.bin` packing**, and **per-profile Same-Account Save Isolation**.

Pairs seamlessly with **[KaiVesper-Prep](https://github.com/Sank-kab/KaiVesper-Prep)** (native Windows preparation tool).

---

## Features

- **4-Tab Native Console UI (1280×720, Touch + Controller):**
  - **Games:**
    - Browse prepared Switch 2 (`SW2`) and Switch 1 (`SW1`) profiles.
    - One-press cold install into `FS` on first load, followed by **instant zero-reboot Hot-Swapping** between games within the same `kaiHEN` session.
    - Supports packed `exefs.nsp` + `romfs.bin`, loose unencrypted `exefs/` (automatically preserving the Host's signed `main.npdm` and stubbing unused Host `subsdk*` modules), loose `romfs/` (auto-packed once into `romfs.bin` directly on-console with live progress!), and `exefs_patches/*.ips` / `.ips32`.
  - **Hosts:**
    - Automatically enumerates installed Switch 1 (`01...`) and Switch 2 (`04...`) applications on your console (including FW 20/21+ compressed NACP titles and large Switch 2 control data).
    - Displays **`Auto Logo (Best)`** vs **`Manual Logo`** compatibility next to each title so you can pick a Host that cleanly dismisses the Nintendo/Switch boot overlay.
    - Pressing **`[A]`** selects your active `SW1` or `SW2` Host, persists it to `sdmc:/vesper/hosts.cfg`, and automatically creates the required folders on your microSD.
  - **Saves & System:**
    - **Same-Account Save Isolation:** automatically backs up and swaps `save:/` per profile on the same Switch 2 account, with atomic rename verification and a permanent, never-overwritten `_stock_initial_backup` on `sdmc:/vesper/saves/`.
    - Switch between console user accounts (`[ZL]` / `[ZR]`) and run a safe **read-only FS & Save diagnostic check (`[B]`)**.
  - **Guide:** Built-in quick-start guide directly inside the app.

---

## Requirements

1. **Nintendo Switch 2** running **FW 23.0.1** with **patched `kaiHEN`** (with `ro:dmnt` FS helper operations enabled — see [Building Patched `kaiHEN`](#building-patched-kaihen-ro-dmnt-fs-helpers) below) and **hbmenu**. *(KaiVesper verifies FS ABI prologues read-only before patching and aborts safely on unsupported firmware.)*
2. **exFAT-formatted microSD card** (strongly recommended if any `romfs.bin` exceeds 4 GB — FAT32 has a 4 GB single-file limit).
3. At least one legally installed **Switch 2 (`04...`)** and/or **Switch 1 (`01...`)** title on your console to act as a Host container *(prefer titles marked **`Auto Logo (Best)`** in the Hosts tab)*.

---

## Installation & Quick Start

1. Download **`kaivesper.nro`** from the [Releases](../../releases) page and copy it to:
   ```text
   SD:/switch/kaivesper.nro
   ```
2. Launch **hbmenu** under patched **kaiHEN** and open **KaiVesper**.
3. Open the **Hosts** tab (`[R]`), highlight an installed Switch 2 (`SW2`) and/or Switch 1 (`SW1`) title marked **`Auto Logo (Best)`**, and press **`[A]`** to set it as your active Host.
4. Prepare and copy your unencrypted game profiles onto your microSD card (using **[KaiVesper-Prep](https://github.com/Sank-kab/KaiVesper-Prep)** on Windows, or manually):
   ```text
   SD:/ns2jb-modloader/sw2/<game_name>/
     ├── exefs/           (unencrypted NSO0 executables; main.npdm is ignored safely)
     ├── romfs.bin        (or loose romfs/ folder — auto-packed once on console)
     └── exefs_patches/   (optional .ips / .ips32 patches)

   SD:/ns2jb-modloader/sw1/<game_name>/
     ├── exefs/
     ├── romfs.bin        (or loose romfs/ folder)
     └── exefs_patches/
   ```
   *(Per-host paths `SD:/ns2jb-modloader/contents/<HOST_TITLE_ID>/games/<game_name>/` and `exefs.nsp` are also supported.)*
5. In the **Games** tab, press **`[Y]`** to refresh, highlight a game, and press **`[A]`**.
6. Press **`[+]`** to exit KaiVesper and launch your selected Host title from the Home Menu!
7. **To switch games without rebooting:** close the running Host game completely (`X → Close`), reopen **KaiVesper**, highlight the next game, and press **`[A]`** (Hot-Swap!).

---

## Controls

| Button | Action |
| :--- | :--- |
| **`L` / `R`** (or Touch sidebar) | Switch tabs (**Games**, **Hosts**, **Saves & System**, **Guide**) |
| **D-Pad / Left Stick / Touch** | Navigate game or host rows |
| **`A`** | Load / Hot-Swap game *(Games)* · Set active SW1/SW2 Host *(Hosts)* · Toggle Auto Save-Swap *(Saves)* |
| **`X`** | Return to Stock Mode without reboot *(Games)* · Filter `All / SW1 / SW2` *(Hosts)* |
| **`ZR`** | Switch between SW1 / SW2 Host containers *(Games)* · Switch user account *(Saves)* |
| **`ZL`** | Switch user account |
| **`B`** | Safe read-only FS & Save diagnostic check |
| **`Y`** | Refresh microSD catalog & console title list |
| **`-`** | Toggle Auto Save-Swap ON / OFF |
| **`+`** | Exit back to hbmenu |

---

## Troubleshooting

- **Nintendo / Switch logos remain overlaid while the game plays:**
  Your chosen Host title uses `logo_handling = Manual`. Open the **Hosts** tab in KaiVesper and choose a Host marked **`Auto Logo (Best)`**.
- **Switch 1 game freezes on the boot logo:**
  Make sure you selected a **64-bit Switch 1 Host** (`01...`) for 64-bit Switch 1 games (or a 32-bit Host for 32-bit titles), and use **KaiVesper-Prep** so any required `MountTemporaryStorage` `.ips32` patch is generated automatically.
- **`Write failed: >4GB romfs.bin requires an exFAT microSD!`:**
  FAT32 cannot store files larger than 4 GiB. Use an exFAT-formatted microSD card for large games.
- **Stock `kaiHEN` detected:**
  On-console FS installation and zero-reboot Hot-Swapping require a patched `kaiHEN` build with the `ro:dmnt` FS helper primitives.
- **Diagnostic Logs:**
  KaiVesper writes readable diagnostic logs to the microSD root (`vesper-install.log`, `vesper-discover.log`, `vesper-save.log`, `vesper-hosts.log`).

---

## Building Patched `kaiHEN` (`ro:dmnt` FS Helpers)

On-console FS installation and zero-reboot Hot-Swapping require `ouncebrew/kaihen` compiled with the 3 public FS helper patches from **`nicholasyoannou/switch2-modbridge`** (exposing `HEN_GET_FS_PROCESS` and the `ro:dmnt` FS query/read/write/patch/protect/map ops).

### Option A — 1-Click on Windows (Recommended)

Open **[KaiVesper-Prep](https://github.com/Sank-kab/KaiVesper-Prep)** → **`kaiHEN (Patch & Launch)`** tab:
1. Click **`Patch & Build kaiHEN`** — automatically downloads `ouncebrew/kaihen` (`20261117.1`) and the 3 `ro:dmnt` patches from `switch2-modbridge`, compiles them via Docker, and creates a ready-to-launch patched `kaiHEN` folder.
2. Click **`Launch kaiHEN`** — automatically downloads `ns2jb` (`hen.cjs`) and launches your patched `kaiHEN` through WSL2 (needs WSL2 + Node.js; mirrored networking recommended) with a live SmileBASIC `V=START(...)` command banner!

### Option B — Manual CLI Build (Linux / macOS / WSL)

```bash
# 1. Clone kaiHEN (20261117.1) and switch2-modbridge
git clone https://github.com/ouncebrew/kaihen.git
git clone https://github.com/nicholasyoannou/switch2-modbridge.git

# 2. Apply the 3 kaiHEN patches from switch2-modbridge
cd kaihen
git checkout 20261117.1
git apply ../switch2-modbridge/kaihen/patches/*.patch

# 3. Build using the ounce-devkit Docker image (follow ouncebrew/kaihen build steps),
#    replace ro/main, hen/main, and bootstrap.bin in your kaiHEN-20261117.1 release folder,
#    and launch normally (e.g. via ns2jb hen.cjs)
```

Once running on Switch 2, open **KaiVesper → Saves & System** and confirm:
- **`kaiHEN: Patched kaiHEN Active (Ready)`**
- Press **`[B]`** to run a safe read-only FS & Save check.

*(Note: Running KaiVesper on stock unpatched `kaiHEN` still works safely in SD-staging mode, but will not perform on-console FS install or zero-reboot Hot-Swap.)*

---

## Building from Source

Requires Docker with the `ounce-devkit:latest` image (with `libounce` and `libkaihen`):

```bash
./build.sh
```

Output binary: `kaivesper.nro` (with NACP metadata `KaiVesper` / author `sank-kab` and embedded `icon.jpg`).

To regenerate the anti-aliased Segoe UI bitmap font header (`source/font_aa.h`) and `icon.jpg` on Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_assets.ps1
```

---

## Credits & Third-Party Software

- Built with **devkitPro / `libounce` / `libnx`** and **`ouncebrew/kaihen` (`libkaihen`)**.

## License

Licensed under the **GNU General Public License v2.0 only (`GPL-2.0-only`)**. See [LICENSE](LICENSE) for full terms.
