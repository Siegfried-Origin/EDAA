# EDAA - Temporal Anti-Aliasing for Elite Dangerous

A ReShade add-on that adds TAA (Temporal Anti-Aliasing) to Elite Dangerous
Odyssey (DX11): sub-pixel jitter, motion vectors from the game's camera and
depth, and a TAA resolve inserted into the game's frame before the cockpit
HUD, bloom and tonemapping.

* How it works, settings, diagnostics: [docs/addon.md](docs/addon.md)
* The game's rendering, reverse-engineered: [docs/reverse-engineering.md](docs/reverse-engineering.md)
* Finding camera matrices in any DX11 game: [docs/finding-camera-matrices.md](docs/finding-camera-matrices.md)
* Motion vectors design: [docs/motion-vectors.md](docs/motion-vectors.md)
* Analysis scripts (RenderDoc): [tools/README.md](tools/README.md)

## Install

### With the installer (recommended)

Download `EDAA-Setup-<version>.exe` from the
[releases](https://github.com/Siegfried-Origin/EDAA/releases) and run it.
It:

* finds the game (Steam libraries, Epic, Frontier launcher), or lets you
  browse to the folder that contains `EliteDangerous64.exe`;
* checks for ReShade with full add-on support (`dxgi.dll` in the game
  folder, version 6.8.0 or newer, not the normal build that ignores
  add-ons). If it is missing, it offers to download the official
  `ReShade_Setup_6.8.0_Addon.exe` from reshade.me and run it silently for
  the game (API DirectX 10/11/12), or to open reshade.me;
* copies `EDAA.addon64` next to `dxgi.dll`.

Then, in the game's graphics options, set anti-aliasing to off (the TAA
replaces it). TAA is on by default; press `Home` for the ReShade overlay and
open the "EDAA" tab to see its status or change settings (saved in
`ReShade.ini`). `Ctrl+Shift+T` toggles it.

EDHM / 3DMigoto (`d3d11.dll`) can stay installed. ReShade warns that its
add-on build is meant for single-player games; use it at your own
discretion.

### By hand

Game folder (Steam): `C:\Program Files (x86)\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64`

1. Download **ReShade 6.8.0 or newer, "with full add-on support"** from
   https://reshade.me (the normal build does not load add-ons).
2. Run it, select `EliteDangerous64.exe` in the game folder and the API
   **DirectX 10/11/12**. It installs `dxgi.dll` and `ReShade.ini`. Skip the
   effect packages (none are needed).
3. Copy `EDAA.addon64` next to `dxgi.dll` (from the release zip, a package
   made with `tools\package.ps1`, or `build\Release` after building, see
   [docs/addon.md](docs/addon.md)).
4. Set the game's anti-aliasing to off and start the game.

## Uninstall

Installed with the installer: Windows Settings > Apps > "EDAA" (removes
`EDAA.addon64`; ReShade stays). By hand: close the game and delete
`EDAA.addon64` from the game folder.

To remove ReShade too: run the ReShade setup and choose "Uninstall", or
delete `dxgi.dll`, `ReShade.ini`, `ReShade.log` and the `reshade-shaders`
folder if present.

## Releases

Pushing a tag (`git tag v1.0.0 && git push origin v1.0.0`) runs
[.github/workflows/release_on_tag.yml](.github/workflows/release_on_tag.yml):
it builds the add-on, builds the installer from
[installer/EDAA.iss](installer/EDAA.iss) with Inno Setup and attaches
`EDAA-Setup-<version>.exe` and `EDAA-<version>.zip` to the release of that
tag (marked pre-release). Local installer build:
`ISCC /DAppVersion=1.0.0 installer\EDAA.iss` (output in `installer\Output`).

## Hotkeys

| Keys | Action |
|---|---|
| `Ctrl+Shift+T` | TAA on/off |

Developer builds (`-DEDAA_DEVELOPER=ON`, see [docs/addon.md](docs/addon.md))
add `Ctrl+Shift+J` (jitter alone), `Ctrl+Shift+M` (debug views) and
`Ctrl+Shift+C` (auto capture to `Documents\EDAA-captures`).
