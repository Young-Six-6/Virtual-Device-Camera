[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [ValidateSet('x64')]
    [string] $Platform = 'x64',
    [string] $WindowsTargetPlatformVersion,
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

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$includeRoot = Join-Path $kitsRoot 'Include'
$libraryRoot = Join-Path $kitsRoot 'Lib'
$minimumWdkVersion = [version]'10.0.19041.0'
$availableWdkVersions = @(Get-ChildItem -LiteralPath $includeRoot -Directory -ErrorAction Stop |
    Where-Object {
        $_.Name -match '^\d+(\.\d+){2,3}$' -and
        [version]$_.Name -ge $minimumWdkVersion -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'km\ntddk.h')) -and
        (Test-Path -LiteralPath (Join-Path $libraryRoot "$($_.Name)\km\x64\ntoskrnl.lib"))
    } |
    Sort-Object { [version]$_.Name } -Descending)
if ($WindowsTargetPlatformVersion) {
    $wdkVersion = $availableWdkVersions | Where-Object Name -EQ $WindowsTargetPlatformVersion | Select-Object -First 1
    if ($null -eq $wdkVersion) {
        $available = if ($availableWdkVersions.Count) { ($availableWdkVersions.Name -join ', ') } else { 'none' }
        throw "Requested WDK $WindowsTargetPlatformVersion is unavailable or lacks x64 kernel headers/libraries. Compatible installed versions: $available."
    }
} else {
    $wdkVersion = $availableWdkVersions | Select-Object -First 1
}
if ($null -eq $wdkVersion) {
    throw 'A WDK 10.0.19041.0 or newer with x64 kernel headers and libraries was not found.'
}
Write-Output "Using WDK $($wdkVersion.Name)."

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

& $msbuild $project /m "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/p:WindowsTargetPlatformVersion=$($wdkVersion.Name)" /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE"
}

$driver = Join-Path $PSScriptRoot "build\$Platform\$Configuration\xusb_driver.sys"
if (-not (Test-Path -LiteralPath $driver)) {
    throw "Build completed without producing the expected driver: $driver"
}
Write-Output "Built unsigned driver: $driver"
