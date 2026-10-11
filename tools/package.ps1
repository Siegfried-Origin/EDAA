<#
.SYNOPSIS
  Build EDAA in Release and pack it into dist\EDAA-<date>\ and dist\EDAA-<date>.zip.

  The package contains EDAA.addon64 and INSTALL.txt (install / uninstall
  steps). Nothing is copied into the game folder: installation stays manual.

.EXAMPLE
  .\tools\package.ps1
#>
param(
    [string] $Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

cmake -S . -B build -G $Generator -A x64 | Out-Null
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$name = "EDAA-" + (Get-Date -Format "yyyyMMdd-HHmm")
$dir = Join-Path $root "dist\$name"
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item build\Release\EDAA.addon64 $dir

$install = @'
EDAA - temporal anti-aliasing for Elite Dangerous (ReShade add-on)
=================================================================

Requirements
  - Elite Dangerous Odyssey (DX11)
  - ReShade 6.8.0 or newer, "with full add-on support" (https://reshade.me).
    The normal ReShade build does not load add-ons.

Install
  1. Close the game.
  2. Run the ReShade installer, select EliteDangerous64.exe in the game
     folder, for Steam:
       C:\Program Files (x86)\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64
     choose the API "DirectX 10/11/12", skip the effect packages.
     ReShade installs dxgi.dll and ReShade.ini next to the exe.
     EDHM / 3DMigoto (d3d11.dll) can stay installed.
  3. Copy EDAA.addon64 from this folder next to dxgi.dll in the game folder.
  4. In the game's graphics options, set anti-aliasing to off.
  5. Start the game, press Home for the ReShade overlay, open the "EDAA" tab
     and check the first line shows the build date/time of this package.
     TAA is on by default; Ctrl+Shift+T toggles it. Settings are saved in
     ReShade.ini ([EDAA] section); "Reset to defaults" restores them.

Update
  Close the game and replace EDAA.addon64 in the game folder.

Uninstall
  Close the game and delete EDAA.addon64 from the game folder.
  To remove ReShade too: delete dxgi.dll, ReShade.ini, ReShade.log and the
  reshade-shaders folder if present, or run the ReShade installer and
  choose "Uninstall".

Hotkeys
  Ctrl+Shift+T  TAA on/off

Notes
  - The cockpit HUD/holograms are drawn after the TAA: they stay sharp but
    are not anti-aliased.
  - Tuning values are under "Advanced settings" in the "EDAA" overlay tab;
    see docs/addon.md in the source.
'@
$install -replace "`r?`n", "`r`n" | Set-Content -Encoding ascii (Join-Path $dir "INSTALL.txt")

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $dir "*") -DestinationPath $zip
Write-Host "Package: $dir"
Write-Host "Zip:     $zip"
