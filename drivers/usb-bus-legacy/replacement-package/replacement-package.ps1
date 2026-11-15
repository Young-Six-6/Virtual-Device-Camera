[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath($PSScriptRoot)
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $root '..\..\..'))
$legacyRoot = Join-Path $repoRoot 'drivers\usb-bus-legacy\driver-source'
$ucxRoot = Join-Path $repoRoot 'drivers\usb-bus-legacy\ucx-driver'
$legacySys = Join-Path $legacyRoot "build\x64\$Configuration\xusb_driver.sys"
$ucxSys = Join-Path $ucxRoot "build\x64\$Configuration\VirtualCameraUcxHcd.sys"
$packageDir = Join-Path $root "build\x64\$Configuration"
$packageParent = [System.IO.Path]::GetFullPath((Join-Path $root 'build\x64'))

if (-not [string]::Equals(
    [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($packageDir)).TrimEnd('\'),
    $packageParent.TrimEnd('\'),
    [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to stage outside the expected build directory: $packageDir"
}

& (Join-Path $legacyRoot 'build.ps1') -Configuration $Configuration -Platform x64 -Clean:$Clean
if ($LASTEXITCODE -ne 0) {
    throw "Legacy role-3 driver build failed with exit code $LASTEXITCODE"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio discovery tool was not found: $vswhere"
}
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild)) {
    throw 'MSBuild was not found. Install Visual Studio with the C++ build tools.'
}

$target = if ($Clean) { '/t:Rebuild' } else { '/t:Build' }
& $msbuild (Join-Path $ucxRoot 'ucx_hcd.vcxproj') $target /m "/p:Configuration=$Configuration" /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "UCX HCD build failed with exit code $LASTEXITCODE"
}
foreach ($requiredFile in @($legacySys, $ucxSys)) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Expected driver image was not produced: $requiredFile"
    }
}

if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
Copy-Item -LiteralPath $legacySys -Destination (Join-Path $packageDir 'xusb_driver.sys')
Copy-Item -LiteralPath $ucxSys -Destination (Join-Path $packageDir 'VirtualCameraUcxHcd.sys')
Copy-Item -LiteralPath (Join-Path $legacyRoot 'package\experimental\xusb_reconstructed_tree.inf') -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $root 'package\xusb_ucx_hcd.inf') -Destination $packageDir

$kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$inf2cat = Get-ChildItem -LiteralPath $kitRoot -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object {
        foreach ($architecture in @('x64', 'x86')) {
            $candidate = Join-Path $_.FullName "$architecture\Inf2Cat.exe"
            if (Test-Path -LiteralPath $candidate) { $candidate; break }
        }
    } |
    Select-Object -First 1
if (-not $inf2cat) {
    throw 'Inf2Cat.exe was not found. Install the Windows Driver Kit.'
}
$toolsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools'
$infverif = Get-ChildItem -LiteralPath $toolsRoot -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object {
        $candidate = Join-Path $_.FullName 'x64\InfVerif.exe'
        if (Test-Path -LiteralPath $candidate) { $candidate; break }
    } |
    Select-Object -First 1
if (-not $infverif) {
    throw 'InfVerif.exe was not found under Windows Kits\10\Tools. Install the Windows Driver Kit.'
}
$infFiles = @(
    (Join-Path $packageDir 'xusb_reconstructed_tree.inf'),
    (Join-Path $packageDir 'xusb_ucx_hcd.inf'))
& $infverif /wbuild 10.0.19041 /w @infFiles
if ($LASTEXITCODE -ne 0) {
    throw "InfVerif failed for the Windows 10 19041+ driver package (exit code $LASTEXITCODE)"
}

& $inf2cat "/driver:$packageDir" /os:10_VB_X64
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE"
}

Write-Output "Combined unsigned role-3 + UCX role-2 package staged at: $packageDir"
Write-Output 'Sign both catalog files with the release publisher certificate before using replacement-deploy.ps1.'
