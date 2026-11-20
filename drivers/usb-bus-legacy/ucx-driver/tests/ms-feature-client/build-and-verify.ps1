[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$WindowsTargetPlatformVersion
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot)
$buildRoot = [IO.Path]::GetFullPath((Join-Path $root 'build'))
$packageDir = Join-Path $buildRoot 'package'
$project = Join-Path $root 'UcxMsFeatureTestClient.vcxproj'
$infSource = Join-Path $root 'UcxMsFeatureTestClient.inf'

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$includeRoot = Join-Path $kitsRoot 'Include'
$libraryRoot = Join-Path $kitsRoot 'Lib'
$kmdfVersion = '1.31'
$minimumWdkVersion = [version]'10.0.19041.0'
$availableWdks = @(Get-ChildItem -LiteralPath $includeRoot -Directory -ErrorAction Stop |
    Where-Object {
        $_.Name -match '^\d+(\.\d+){2,3}$' -and
        [version]$_.Name -ge $minimumWdkVersion -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'km\ntddk.h')) -and
        (Test-Path -LiteralPath (Join-Path $includeRoot "wdf\kmdf\$kmdfVersion\wdf.h")) -and
        (Test-Path -LiteralPath (Join-Path $libraryRoot "$($_.Name)\km\x64\ntoskrnl.lib")) -and
        (Test-Path -LiteralPath (Join-Path $libraryRoot "wdf\kmdf\x64\$kmdfVersion\wdfdriverentry.lib"))
    } | Sort-Object { [version]$_.Name } -Descending)
if ($WindowsTargetPlatformVersion) {
    $wdk = $availableWdks | Where-Object Name -EQ $WindowsTargetPlatformVersion | Select-Object -First 1
} else {
    $wdk = $availableWdks | Select-Object -First 1
}
if ($null -eq $wdk) {
    $found = if ($availableWdks.Count) { $availableWdks.Name -join ', ' } else { 'none' }
    throw "No compatible x64 WDK with KMDF $kmdfVersion was found (available: $found)."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw "Visual Studio discovery tool was not found: $vswhere" }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild)) {
    throw 'MSBuild was not found. Install Visual Studio with the C++ build tools and WDK integration.'
}

Write-Output "Building test-only KMDF client with WDK $($wdk.Name), KMDF $kmdfVersion."
& $msbuild $project /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$($wdk.Name)" /v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }

$driverImage = Join-Path $buildRoot "x64\$Configuration\UcxMsFeatureTestClient.sys"
if (-not (Test-Path -LiteralPath $driverImage)) { throw "Expected driver image was not produced: $driverImage" }

if (Test-Path -LiteralPath $packageDir) {
    $resolvedBuild = [IO.Path]::GetFullPath($buildRoot).TrimEnd('\')
    $resolvedPackage = [IO.Path]::GetFullPath($packageDir).TrimEnd('\')
    if (-not $resolvedPackage.StartsWith($resolvedBuild + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace package output outside the ignored build directory: $resolvedPackage"
    }
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
Copy-Item -LiteralPath $infSource -Destination $packageDir
Copy-Item -LiteralPath $driverImage -Destination $packageDir
$packageInf = Join-Path $packageDir 'UcxMsFeatureTestClient.inf'

$toolsRoot = Join-Path $kitsRoot 'Tools'
$infverif = Get-ChildItem -LiteralPath $toolsRoot -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object {
        $candidate = Join-Path $_.FullName 'x64\InfVerif.exe'
        if (Test-Path -LiteralPath $candidate) { $candidate; break }
    } | Select-Object -First 1
if (-not $infverif) { throw 'InfVerif.exe was not found under Windows Kits\10\Tools.' }

$binRoot = Join-Path $kitsRoot 'bin'
$inf2cat = Get-ChildItem -LiteralPath $binRoot -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object {
        $candidate = Join-Path $_.FullName 'x86\Inf2Cat.exe'
        if (Test-Path -LiteralPath $candidate) { $candidate; break }
    } | Select-Object -First 1
if (-not $inf2cat) { throw 'Inf2Cat.exe was not found under Windows Kits\10\bin.' }

& $infverif /wbuild 10.0.19041 /w $packageInf
if ($LASTEXITCODE -ne 0) { throw "InfVerif failed with exit code $LASTEXITCODE" }

& $inf2cat "/driver:$packageDir" /os:10_VB_X64
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed with exit code $LASTEXITCODE" }

Write-Output "Built and statically validated test-only package: $packageDir"
Write-Output 'The generated catalog is unsigned. This script does not sign, publish to the Driver Store, install, or run the driver.'
