[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [ValidateSet('x64')]
    [string] $Platform = 'x64',
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'xusb_driver.vcxproj'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer discovery tool was not found: $vswhere"
}

$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild)) {
    throw 'MSBuild was not found. Install Visual Studio with the C++ build tools.'
}

$wdkInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include\10.0.28000.0\km\ntddk.h'
$wdkLib = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Lib\10.0.28000.0\km\x64\ntoskrnl.lib'
if (-not (Test-Path -LiteralPath $wdkInclude) -or -not (Test-Path -LiteralPath $wdkLib)) {
    throw 'WDK 10.0.28000.0 headers or x64 kernel libraries are missing. Install the matching WDK.'
}

if ($Clean) {
    $sourceRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
    $buildDirectory = [System.IO.Path]::GetFullPath((Join-Path $sourceRoot 'build'))
    $buildParent = [System.IO.Path]::GetDirectoryName($buildDirectory)
    if (-not [string]::Equals(
        $buildParent.TrimEnd('\'),
        $sourceRoot.TrimEnd('\'),
        [System.StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
            [System.IO.Path]::GetFileName($buildDirectory),
            'build',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean an unexpected path: $buildDirectory"
    }
    if (Test-Path -LiteralPath $buildDirectory) {
        Remove-Item -LiteralPath $buildDirectory -Recurse -Force
    }
}

& $msbuild $project /m "/p:Configuration=$Configuration" "/p:Platform=$Platform" /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE"
}

$driver = Join-Path $PSScriptRoot "build\$Platform\$Configuration\xusb_driver.sys"
if (-not (Test-Path -LiteralPath $driver)) {
    throw "Build completed without producing the expected driver: $driver"
}
Write-Output "Built unsigned driver: $driver"
