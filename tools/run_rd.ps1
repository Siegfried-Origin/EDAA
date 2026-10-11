<#
.SYNOPSIS
  Run a RenderDoc replay script (tools/rd_*.py) headless against a capture.

.EXAMPLE
  .\tools\run_rd.ps1 -Script rd_dump_frame.py -Capture "...\Coriolis.rdc" -Out out\coriolis
#>
param(
    [Parameter(Mandatory)] [string] $Script,
    [Parameter(Mandatory)] [string] $Capture,
    [Parameter(Mandatory)] [string] $Out,
    [string] $ScriptArgs = "",
    [string] $RenderDoc = "C:\Program Files\RenderDoc\qrenderdoc.exe"
)

$tools = $PSScriptRoot
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$env:EDAA_CAPTURE = (Resolve-Path $Capture).Path
$env:EDAA_OUT = $Out
$env:EDAA_ARGS = $ScriptArgs
$env:EDAA_LOG = Join-Path $Out "log.txt"
$env:PYTHONPATH = $tools

# qrenderdoc's embedded Python does not honour PYTHONPATH reliably, so a small
# bootstrap adds the tools folder to sys.path before running the real script.
$boot = Join-Path $Out "_boot.py"
@"
import sys, runpy
sys.path.insert(0, r'$tools')
runpy.run_path(r'$(Join-Path $tools $Script)', run_name='__main__')
"@ | Set-Content -Encoding ascii $boot

# -Wait would also wait for helper processes that outlive qrenderdoc.
$p = Start-Process -FilePath $RenderDoc -ArgumentList "--python", "`"$boot`"" -PassThru
$p.WaitForExit()
if (Test-Path $env:EDAA_LOG) { Get-Content $env:EDAA_LOG }
exit $p.ExitCode
