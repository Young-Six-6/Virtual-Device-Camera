[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$FFmpegRoot,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) {
    throw 'Install Visual Studio 2022/2026 with Desktop development with C++ and Windows SDK.'
}
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'No Visual Studio C++ toolchain found.' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$buildArgs = @(
    (Join-Path $repoRoot 'xusb_vcam.sln'), '/m', "/t:$target",
    "/p:Configuration=$Configuration", '/p:Platform=x64', '/verbosity:minimal', '/nologo'
)
if ($FFmpegRoot) {
    $buildArgs += "/p:FFmpegRoot=$((Resolve-Path -LiteralPath $FFmpegRoot).Path)"
}
& $msbuild @buildArgs
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }
Write-Host "Built: $(Join-Path $repoRoot "build\bin\x64\$Configuration\virtual-device-camera.exe")"
