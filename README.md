# Classic Xbox Store

[Español](README.es.md) · **English** · [Português](README.pt.md)

<p align="center"><img src="docs/img/icono.png" alt="Classic Xbox Store" width="200"></p>

A **native game store for the original Xbox** (2001): a `default.xbe` you launch from the dashboard of a modded console. It shows Internet Archive's Xbox game catalog with cover art, **downloads and installs games straight to the console's hard drive** and launches them from its own list. No PC involved.

It is the console version of [Classic Xbox Loader](https://github.com/xeodeo) (the PC app that downloads, extracts and uploads games over FTP), built with the same toolchain as the [OptiCraft Xbox port](https://github.com/xeodeo/OptiCraft-Heritage-XBOX-CLASSIC-Edition).

| Store | Download |
|---|---|
| ![Store](docs/img/tienda.png) | ![Download](docs/img/descarga.png) |
| **Network and proxies** | **Account (on-screen keyboard)** |
| ![Network](docs/img/red.png) | ![Account](docs/img/cuenta.png) |

## Features

- **Store:** Internet Archive catalog by letter (`#`, `A`…`Z`), with region and size. The selected game shows its **cover** on the right and its **background** behind, with a fade. Lists and images are cached in `T:\cache`, so they load instantly the second time and still show when archive.org is down.
- **Install (A):** pick the drive (`E:`, `F:` or `G:`, with the real free space) and it:
  1. downloads the **ZIP** from archive.org over **16 connections at once** (each chunk with `Range`) and inflates it on the console with zlib;
  2. skips the video partition of the redump image (the first 387 MB) and stores the game partition in chunks under 4 GB (FATX limit);
  3. as soon as the disc's file tree (XDVDFS) arrives it knows where the last file ends and **stops downloading there**: the padding at the end is never fetched;
  4. copies the files to `<drive>:\Games\<game>` (`default.xbe` last), checks each one matches the size on the original disc and deletes the temporary files.
- **Resume:** a checkpoint is saved every 128 MB (including the zlib state, like zlib's `zran` example). If the console is turned off, you quit with the button combo or the network drops, the store asks whether to resume next time it opens. Same if it stops while copying.
- **Installed (Y):** games and apps on the hard drive (any top-level folder whose subfolders have a `default.xbe`), split into *Games* and *Apps*. **A launches them.**
- **Account (Start):** log in to archive.org from the console with an on-screen keyboard (archive.org doesn't allow downloading these collections without an account). The password is never stored: only the cookies archive.org returns, in `T:\ia_session.json`. Classic Xbox Loader's `ia_session.json` copied next to `default.xbe` also works.
- **Network (X):** diagnostics (IP, DNS, connections, HTTPS) and the latest log lines. **Y opens Proxies:** search for nearby free proxies (A) and turn them on or off (Y). They are **off by default**; when on, half of the connections go through them, the list refreshes itself every 3 hours and a connection whose proxy fails continues direct.
- **Languages:** English (default), Spanish and Portuguese. Change it in Account (Start) with Y; the choice is saved.

## How it works

- **HTTPS on the Xbox:** [mbedTLS](https://github.com/Mbed-TLS/mbedtls) 3.6 (TLS 1.2 client only, no instructions the Pentium III lacks) over XNet sockets. The required root certificates are embedded in the XBE (`assets/certs/roots.pem`).
- **Speed:** the Xbox TCP stack has no window scaling (64 KB max per connection); with ~0.2 s round trip to archive.org that is ~0.3 MB/s per connection. So 1 MB chunks are spread over 16 reused (keep-alive) connections; a real console gets 3–5 MB/s.
- **DNS:** if the DNS set by the dashboard doesn't answer, it asks 1.1.1.1 and 8.8.8.8, and caches the addresses.
- **Covers:** the index `assets/data/artindex.tsv` (built with `scripts/make_art_index.py`) maps each ZIP's redump name to its XMID and Title ID and lists which art exists, so it never requests missing images.
- **Log:** `E:\TDATA\FFFF4358\cxs_log.txt` (grab it over FTP from the dashboard). If a thread crashes, it logs the exception, registers and stack with addresses from the `.map`.

## Where the games come from

From Internet Archive's Xbox game catalog, the same collections Classic Xbox Loader uses:

- `https://archive.org/details/microsoft_xbox_numberssymbols` (#)
- `https://archive.org/details/microsoft_xbox_a` … `microsoft_xbox_z` (some letters in two parts: `microsoft_xbox_c_part1` and `_part2`, same for D, M, N, O, S and T)

Each letter's list comes from `https://archive.org/metadata/<collection>/files` and each game is downloaded from `https://archive.org/download/<collection>/<game>.zip`. An archive.org account is required.

This repository does not host or distribute any game.

## Building

### Requirements

- **Windows 10/11** with **Visual Studio 2022** (*Desktop development with C++* workload, with the x86 tools) and the **Windows 10/11 SDK** (for the static UCRT).
- **Python 3** (embeds fonts, certificates and the cover index into the XBE).
- **The Xbox XDK (5849).** **It is not in this repository and must never be uploaded.** It is the same one OptiCraft uses, preserved in the Internet Archive collection <https://archive.org/details/xbox-sdks> (file `2005-03 - 5849.6, 5849.17 - RecoveryEXE, SDK.7z`).
  1. Download that file and extract it with 7-Zip: it contains `XDKSetup5849.17.exe`.
  2. Extract the installer with 7-Zip too (no need to install it; it requires Visual Studio .NET 2003 and Windows XP):

     ```text
     7z x XDKSetup5849.17.exe -o<xdk>\sdk
     ```

  3. The XDK ends up in `<xdk>\sdk\XDK\xbox` (`bin\`, `include\`, `lib\`). That is the folder to use: the XDK linker (`bin\vc71\Link.Exe`), `imagebld.exe`, `bundler.exe` and the XAPI, Direct3D 8 and XNet libraries.
- `scripts/ninja.exe` and `scripts/extract-xiso.exe` are already in the repository.

Tested with Windows 10 Pro 22H2, Visual Studio 2022 Community (MSVC 14.44), Windows SDK 10.0.26100, CMake 3.31 (the one bundled with Visual Studio), Ninja 1.13.2, XDK 5849, extract-xiso 2.7.1, [xemu](https://xemu.app) and a softmodded retail console.

### Commands

In PowerShell:

```powershell
$env:XBOX_XDK_ROOT = "C:\path\to\xdk\5849\sdk\XDK\xbox"
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --preset xbox
& $cmake --build --preset xbox
```

Output in `bin/`:

- `bin/iso/` with `default.xbe` and `default.tbn` (the thumbnail for XBMC/UnleashX-style dashboards). **That folder is what you copy to the console**, e.g. to `F:\Apps\Classic Xbox Store\`.
- `bin/ClassicXboxStore.iso`, the same as an ISO for xemu.

The `xbox-autotest` preset builds a test version for xemu (in `bin-autotest/`) that installs a ZIP served from the PC by itself (`10.0.2.2:8000`, an HTTP server with `Range` support), without depending on archive.org.

### How it is built (the same trick as OptiCraft)

The XDK compiler is Visual C++ 7.1 (2003), which doesn't understand C++17. So two toolchains are combined:

- **Compile:** VS2022's `cl.exe` (x86, `/arch:SSE`: the Pentium III has SSE but not SSE2) against the modern STL and UCRT (`cmake/xbox_toolchain.cmake`).
- **Link:** the XDK's `Link.Exe`, with the modern static CRT (`libcmt`, `libcpmt`, `libucrt`) followed by the XDK libraries. `src/runtime/XboxCrtShim.cpp` provides the Win32 functions the modern CRT needs and XAPI lacks, and `src/runtime/XboxEntry.cpp` replicates the XDK startup.
- **Afterwards:** `scripts/patch_pe_for_imagebld.ps1` prepares the PE for `imagebld`, `scripts/patch_sse2_moves.ps1` replaces the SSE2 instructions left by the CRT with equivalents the Pentium III supports, `bundler.exe` packs the icon and `imagebld.exe` creates `default.xbe`.

### Data embedded in the XBE

| File | What it is | How to regenerate |
|---|---|---|
| `assets/fonts/OpenSans-*.ttf` | UI font | — |
| `assets/certs/roots.pem` | Root certificates (Let's Encrypt/ISRG, GoDaddy, DigiCert, USERTrust, GlobalSign) | from `certifi` |
| `assets/data/artindex.tsv` | Cover index (redump name → XMID, Title ID and available art) | `python scripts/make_art_index.py` |
| `assets/icon/` | Dashboard icon, save icon and `default.tbn` | `python scripts/make_icon.py` |
| `src/ui/LangTable.inc` | English and Portuguese translations of the UI strings (written in Spanish in the code) | `python scripts/make_lang.py` (from `scripts/translations.py`) |

## Using it on the console

1. Copy the contents of `bin/iso/` (or the release zip) to a hard drive folder (over FTP from the dashboard), e.g. `F:\Apps\Classic Xbox Store\`.
2. Launch it from the dashboard.
3. Log in with **Start** (or copy Classic Xbox Loader's `ia_session.json` next to `default.xbe`).

| Button | Store | Installed | Network |
|---|---|---|---|
| A | install | launch | test network (in Proxies: search) |
| Y | go to Installed | go to Store | open Proxies (in Proxies: on / off) |
| X | Network | Network | back |
| Start | Account (Y: language) | Account | Account |
| Left / right | letter | — | — |
| Triggers | page | page | — |
| B | back to dashboard | back to dashboard | back to dashboard |

## Credits

- **Game covers, backgrounds and icons:** database and art by **MobCat** — <https://mobcat.zip/XboxIDs/> and <https://github.com/MobCat/MobCats-original-xbox-game-list>. The store downloads them from that repository; they are not included here.
- **Games:** Xbox collections on [Internet Archive](https://archive.org).
- **Proxy list:** [ProxyScrape](https://proxyscrape.com).
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0), [zlib](https://zlib.net) (zlib license), [stb_image and stb_truetype](https://github.com/nothings/stb) (public domain / MIT), [Open Sans](https://fonts.google.com/specimen/Open+Sans) (Apache 2.0), [extract-xiso](https://github.com/XboxDev/extract-xiso) and [Ninja](https://ninja-build.org).
- Build system and startup code taken from xeodeo's OptiCraft Xbox port.
- UI style inspired by [PKGi](https://github.com/bucanero/pkgi-ps3).

Xbox is a trademark of Microsoft. This project is not affiliated with Microsoft or Internet Archive.
