[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string[]] $OsTargets,
    [switch] $IncludeExperimentalRoles,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath($PSScriptRoot)
$buildScript = Join-Path $root 'build.ps1'
$buildOutput = Join-Path $root "build\x64\$Configuration\xusb_driver.sys"
$packageRoot = Join-Path $root "build\package\x64\$Configuration"
$packageParent = [System.IO.Path]::GetFullPath((Join-Path $root 'build\package\x64'))
if (-not [string]::Equals(
    [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($packageRoot)).TrimEnd('\'),
    $packageParent.TrimEnd('\'),
    [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to stage package outside the expected build directory: $packageRoot"
}

& $buildScript -Configuration $Configuration -Platform x64 -Clean:$Clean
if ($LASTEXITCODE -ne 0) {
    throw "Driver build failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $buildOutput)) {
    throw "Expected driver image was not produced: $buildOutput"
}

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
Copy-Item -LiteralPath $buildOutput -Destination (Join-Path $packageRoot 'xusb_driver.sys')
Get-ChildItem -LiteralPath (Join-Path $root 'package') -Filter '*.inf' -File |
    Copy-Item -Destination $packageRoot
if ($IncludeExperimentalRoles) {
    Get-ChildItem -LiteralPath (Join-Path $root 'package\experimental') -Filter '*.inf' -File |
        Copy-Item -Destination $packageRoot
}

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

$availableOsIdentifiers = (& $inf2cat /? | Out-String)
if ($OsTargets -and $OsTargets.Count -gt 0) {
    foreach ($osTarget in $OsTargets) {
        if ($osTarget -notmatch '^[A-Za-z0-9_]+$' -or
            $availableOsIdentifiers -notmatch "(?i)(?<![A-Za-z0-9_])$([regex]::Escape($osTarget))(?![A-Za-z0-9_])") {
            throw "Inf2Cat does not advertise the requested OS target '$osTarget'."
        }
    }
    $selectedOsTargets = $OsTargets
} else {
    $windows11TargetCandidates = @('10_25H2_X64', '10_GE_X64', '10_NI_X64', '10_CO_X64')
    $latestWindows11Target = $windows11TargetCandidates |
        Where-Object {
            $availableOsIdentifiers -match "(?i)(?<![A-Za-z0-9_])$([regex]::Escape($_))(?![A-Za-z0-9_])"
        } |
        Select-Object -First 1
    $selectedOsTargets = @('10_VB_X64')
    if ($latestWindows11Target) {
        $selectedOsTargets += $latestWindows11Target
    }
}
$osTargetList = $selectedOsTargets -join ','
Write-Output "Checking catalog signability for: $osTargetList"
& $inf2cat "/driver:$packageRoot" "/os:$osTargetList"
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE"
}

Write-Output "Unsigned driver package staged at: $packageRoot"
Write-Output 'Catalog files are generated but unsigned. Sign every generated catalog with the release publisher certificate before deployment.'
