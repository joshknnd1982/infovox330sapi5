<#
.SYNOPSIS
    Assembles the install layout in output\ from the two build trees.

.DESCRIPTION
    The engine and its ~250 MB of voice data are linked in by directory junction rather than
    copied, unless -Copy is given. Junctions need no elevation and keep a rebuild instant;
    the installer packages the real files.

    output\
      Infovox330SAPI5.dll      32-bit SAPI 5 engine
      Infovox330Server.exe     32-bit helper for the 64-bit engine
      Infovox330Config.exe     the configuration utility
      infovox_host.dll         registry-virtualising shim
      ivx_render.exe           sample renderer
      ivx_sapitest.exe         32-bit test harness
      ivx_speak.exe            speaks through the registered SAPI 5 stack
      x64\Infovox330SAPI5.dll  64-bit SAPI 5 engine
      x64\ivx_sapitest.exe     64-bit test harness
      Ivx330\                  the engine
      Voices Ivx330\           voice data
#>
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Output,
    [switch]$Copy
)

$ErrorActionPreference = 'Stop'
if (-not $Output) { $Output = Join-Path $Root 'output' }

$x86 = Join-Path $Root 'build_x86\bin\Release'
$x64 = Join-Path $Root 'build_x64\bin\Release'
$data = Join-Path $Root 'bin'

foreach ($required in @($x86, $x64, $data)) {
    if (-not (Test-Path $required)) { throw "missing $required - build first" }
}

# The helper lingers for a minute after the last client disconnects, and while it is alive
# its executable cannot be replaced. Stopping it is safe: the next 64-bit client starts a
# fresh one on demand.
Get-Process -Name 'Infovox330Server', 'Infovox330Config' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

New-Item -ItemType Directory -Force -Path $Output | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Output 'x64') | Out-Null

Copy-Item (Join-Path $x86 'Infovox330SAPI5.dll')  $Output -Force
Copy-Item (Join-Path $x86 'Infovox330Server.exe') $Output -Force
Copy-Item (Join-Path $x86 'Infovox330Config.exe') $Output -Force
Copy-Item (Join-Path $x86 'ivx_render.exe')       $Output -Force
Copy-Item (Join-Path $x86 'ivx_sapitest.exe')     $Output -Force
Copy-Item (Join-Path $x86 'ivx_speak.exe')        $Output -Force
Copy-Item (Join-Path $data 'infovox_host.dll')    $Output -Force
Copy-Item (Join-Path $x64 'Infovox330SAPI5.dll')  (Join-Path $Output 'x64') -Force
Copy-Item (Join-Path $x64 'ivx_sapitest.exe')     (Join-Path $Output 'x64') -Force
Copy-Item (Join-Path $x64 'ivx_speak.exe')        (Join-Path $Output 'x64') -Force

foreach ($folder in @('Ivx330', 'Voices Ivx330')) {
    $target = Join-Path $Output $folder
    $source = Join-Path $data $folder
    if (Test-Path $target) {
        $item = Get-Item $target -Force
        if ($item.LinkType) {
            # Remove-Item asks whether to delete the junction's contents; this does not.
            [System.IO.Directory]::Delete($target, $false)
        } else {
            Remove-Item $target -Recurse -Force
        }
    }
    if ($Copy) {
        Copy-Item $source $target -Recurse -Force
    } else {
        cmd /c mklink /J "$target" "$source" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "could not create a junction for $folder" }
    }
}

Write-Host "staged to $Output"
Get-ChildItem $Output | Select-Object Mode, Name, Length
