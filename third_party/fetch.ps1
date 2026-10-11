<#
.SYNOPSIS
  Download the third-party headers needed to build the add-on.

  - ReShade add-on API headers (crosire/reshade, BSD-3-Clause) at $ReShadeTag
  - Dear ImGui headers (ocornut/imgui, MIT) at the commit ReShade uses as
    submodule for that tag (required by reshade_overlay.hpp)
#>
param(
    [string] $ReShadeTag = "v6.8.0",
    [string] $ImGuiCommit = "3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Get-File($url, $dest) {
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $dest
    Write-Host "  $dest"
}

$rs = "https://raw.githubusercontent.com/crosire/reshade/$ReShadeTag"
foreach ($f in "reshade.hpp", "reshade_api.hpp", "reshade_api_device.hpp", "reshade_api_format.hpp",
               "reshade_api_pipeline.hpp", "reshade_api_resource.hpp", "reshade_events.hpp", "reshade_overlay.hpp") {
    Get-File "$rs/include/$f" "$root\reshade\include\$f"
}
Get-File "$rs/LICENSE.md" "$root\reshade\LICENSE.md"

$im = "https://raw.githubusercontent.com/ocornut/imgui/$ImGuiCommit"
foreach ($f in "imgui.h", "imconfig.h", "LICENSE.txt") {
    Get-File "$im/$f" "$root\imgui\$f"
}

"ReShade $ReShadeTag`nImGui $ImGuiCommit" | Set-Content -Encoding ascii "$root\VERSIONS.txt"
