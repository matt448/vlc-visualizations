param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$VlcDir = "C:\Program Files\VideoLAN\VLC"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$candidatePaths = @(
    (Join-Path $projectRoot "$BuildDir\libtrackinfo_visualizer_plugin.dll"),
    (Join-Path $projectRoot "$BuildDir\$Configuration\libtrackinfo_visualizer_plugin.dll")
)

$pluginPath = $candidatePaths | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $pluginPath) {
    throw "Could not find libtrackinfo_visualizer_plugin.dll under '$BuildDir'. Build the project first."
}

$installDir = Join-Path $env:APPDATA "vlc\plugins\visualization"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null

Copy-Item -Path $pluginPath -Destination $installDir -Force
Write-Host "Installed plugin to $installDir"

$vlcExe = Join-Path $VlcDir "vlc.exe"
if (Test-Path $vlcExe) {
    & $vlcExe --reset-plugins-cache --intf dummy --dummy-quiet vlc://quit
    Write-Host "Refreshed VLC plugin cache."
} else {
    Write-Warning "Could not find VLC at '$vlcExe'. Refresh VLC's plugin cache manually."
}
