# EDAA - temporal anti-aliasing for Elite Dangerous

A ReShade add-on that adds TAA (temporal anti-aliasing) to Elite Dangerous
Odyssey (DX11): sub-pixel jitter, motion vectors from the game's camera and
depth, and a TAA resolve inserted into the game's frame before the cockpit
HUD, bloom and tonemapping.

* How it works, settings, diagnostics: [docs/addon.md](docs/addon.md)
* The game's rendering, reverse-engineered: [docs/reverse-engineering.md](docs/reverse-engineering.md)
* Finding camera matrices in any DX11 game: [docs/finding-camera-matrices.md](docs/finding-camera-matrices.md)
* Motion vectors design: [docs/motion-vectors.md](docs/motion-vectors.md)
* Analysis scripts (RenderDoc): [tools/README.md](tools/README.md)

## Install

Game folder (Steam): `C:\Program Files (x86)\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64`

1. Download **ReShade 6.8.0 or newer, "with full add-on support"** from
   https://reshade.me (the normal build does not load add-ons).
2. Run the installer, select `EliteDangerous64.exe` in the game folder and
   the API **DirectX 10/11/12**. It installs `dxgi.dll` and `ReShade.ini`.
   Skip the effect packages (none are needed). EDHM / 3DMigoto (`d3d11.dll`)
   can stay installed.
3. Copy `EDAA.addon64` next to `dxgi.dll`. It comes from a package made
   with `tools\package.ps1` (`dist\EDAA-<date>.zip`, with `INSTALL.txt`) or
   from `build\Release` after building (see [docs/addon.md](docs/addon.md)).
4. In the game's graphics options, set anti-aliasing to off (the TAA
   replaces it).
5. Start the game. TAA is on by default; press `Home` for the ReShade
   overlay and open the "EDAA" tab to see its status or change settings
   (saved in `ReShade.ini`). `Ctrl+Shift+T` toggles it.

## Uninstall

Close the game and delete from the game folder: `EDAA.addon64`, and to
remove ReShade too: `dxgi.dll`, `ReShade.ini`, `ReShade.log` and the
`reshade-shaders` folder if present (or run the ReShade installer and choose
"Uninstall").

## Hotkeys

| Keys | Action |
|---|---|
| `Ctrl+Shift+T` | TAA on/off |

Developer builds (`-DEDAA_DEVELOPER=ON`, see [docs/addon.md](docs/addon.md))
add `Ctrl+Shift+J` (jitter alone), `Ctrl+Shift+M` (debug views) and
`Ctrl+Shift+C` (auto capture to `Documents\EDAA-captures`).
