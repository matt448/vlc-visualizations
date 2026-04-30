param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$VlcDir = "C:\Program Files\VideoLAN\VLC"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectRoot $BuildDir
$configurationPath = Join-Path $buildPath $Configuration

$pluginPaths = @()
if (Test-Path $buildPath) {
    $pluginPaths += Get-ChildItem -Path $buildPath -Filter "lib*_plugin.dll" -File
}
if (Test-Path $configurationPath) {
    $pluginPaths += Get-ChildItem -Path $configurationPath -Filter "lib*_plugin.dll" -File
}

$pluginPaths = $pluginPaths | Sort-Object FullName -Unique
if (-not $pluginPaths) {
    throw "Could not find VLC plugin DLLs under '$BuildDir'. Build the project first."
}

$installDir = Join-Path $env:APPDATA "vlc\plugins\visualization"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null

foreach ($pluginPath in $pluginPaths) {
    Copy-Item -Path $pluginPath.FullName -Destination $installDir -Force
    Write-Host "Installed $($pluginPath.Name) to $installDir"
}

$vlcExe = Join-Path $VlcDir "vlc.exe"
$vlcCacheGen = Join-Path $VlcDir "vlc-cache-gen.exe"
if (Test-Path $vlcCacheGen) {
    & $vlcCacheGen (Join-Path $env:APPDATA "vlc\plugins")
    Write-Host "Refreshed VLC user plugin cache."
} elseif (Test-Path $vlcExe) {
    & $vlcExe --reset-plugins-cache --intf dummy --dummy-quiet vlc://quit
    Write-Host "Refreshed VLC application plugin cache."
} else {
    Write-Warning "Could not find VLC tools under '$VlcDir'. Refresh VLC's plugin cache manually."
}
