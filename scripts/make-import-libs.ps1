# Windows: downloads the official OBS release for an architecture and makes import libraries
# (obs.lib, w32-pthreads.lib) from its DLLs, since OBS does not ship them. Run in a Visual Studio
# developer shell (dumpbin and lib on PATH). Also leaves the extracted OBS in deps/obs-win-<arch>
# for the headless tests.
#   pwsh scripts/make-import-libs.ps1 -Arch x64|arm64 [-ObsVersion 33.0.3]
param([ValidateSet("x64", "arm64")][string]$Arch = "x64", [string]$ObsVersion = "33.0.3")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$obs = Join-Path $root "deps/obs-win-$Arch"
$lib = Join-Path $root "deps/obs-lib"
New-Item -ItemType Directory -Force $lib | Out-Null
if (-not (Test-Path "$obs/bin/64bit/obs.dll")) {
  $zip = Join-Path $root "deps/obs-$ObsVersion-$Arch.zip"
  $url = "https://github.com/obsproject/obs-studio/releases/download/$ObsVersion/OBS-Studio-$ObsVersion-Windows-$Arch.zip"
  Write-Host "downloading $url"
  Invoke-WebRequest $url -OutFile $zip
  Expand-Archive $zip -DestinationPath $obs -Force
}
$machine = if ($Arch -eq "arm64") { "ARM64" } else { "X64" }
foreach ($name in @("obs", "w32-pthreads")) {
  $dll = "$obs/bin/64bit/$name.dll"
  $exports = & dumpbin /exports $dll
  $def = @("LIBRARY $name", "EXPORTS")
  $inTable = $false
  foreach ($line in $exports) {
    if ($line -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
    if ($inTable -and $line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+(?:[0-9A-Fa-f]{8}\s+)?(\S+)') { $def += "  $($Matches[1])" }
    elseif ($inTable -and $line -match '^\s*Summary') { break }
  }
  Set-Content -Path "$lib/$name.def" -Value $def
  & lib /nologo /def:"$lib/$name.def" /machine:$machine /out:"$lib/$name.lib" | Out-Null
  Write-Host "$name.lib: $($def.Count - 2) exports"
}
