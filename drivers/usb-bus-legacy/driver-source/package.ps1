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
Get-ChildItem -LiteralPath (Join-Path $root 'package') -Filter '*.inf' -File |
    Copy-Item -Destination $packageRoot
if ($IncludeExperimentalRoles) {
    Get-ChildItem -LiteralPath (Join-Path $root 'package\experimental') -Filter '*.inf' -File |
        Copy-Item -Destination $packageRoot
}

# Use UTC time slots (18 per second) for the INF version, plus unique service and
# image names. Windows can then load a new image while an earlier build is mapped.
$buildTime = [DateTime]::UtcNow
$yearOffset = $buildTime.Year - 2000
$dayAndHour = ($buildTime.DayOfYear * 24) + $buildTime.Hour
$timeSlot = (($buildTime.Minute * 60 + $buildTime.Second) * 18) +
    [int][Math]::Floor($buildTime.Millisecond * 18 / 1000)
if ($yearOffset -lt 0 -or $yearOffset -gt 65535 -or
    $dayAndHour -gt 65535 -or $timeSlot -gt 65535) {
    throw "The current time cannot be represented in a Windows driver version: $buildTime"
}
$driverVersion = "1.$yearOffset.$dayAndHour.$timeSlot"
$versionSuffix = $driverVersion.Replace('.', '_')
$versionedImageName = "xusb_driver_$versionSuffix.sys"
$versionedImagePath = Join-Path $packageRoot $versionedImageName
Copy-Item -LiteralPath $buildOutput -Destination $versionedImagePath

$driverDate = $buildTime.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
$driverVersionPattern = '(?im)^(DriverVer\s*=\s*)[^,\r\n]+,[^\r\n]*$'
$servicePattern = '(?im)^(\s*AddService\s*=\s*)([A-Za-z0-9_.-]+)(\s*,)'
$imageReferencePattern = '(?i)xusb_driver\.sys'
$infFiles = @(Get-ChildItem -LiteralPath $packageRoot -Filter '*.inf' -File)
if ($infFiles.Count -eq 0) {
    throw "No INF files were staged in the package directory: $packageRoot"
}
foreach ($infFile in $infFiles) {
    $infText = [System.IO.File]::ReadAllText($infFile.FullName)
    if (-not [regex]::IsMatch($infText, $driverVersionPattern)) {
        throw "INF does not contain a valid DriverVer line: $($infFile.FullName)"
    }
    $infText = [regex]::Replace($infText, $driverVersionPattern, {
        param($match)
        return $match.Groups[1].Value + $driverDate + ',' + $driverVersion
    })

    $serviceMatches = [regex]::Matches($infText, $servicePattern)
    foreach ($match in $serviceMatches) {
        $serviceName = $match.Groups[2].Value
        $versionedServiceName = "${serviceName}_$versionSuffix"
        if ($versionedServiceName.Length -gt 256) {
            throw "Versioned service name exceeds the Windows limit: $versionedServiceName"
        }
        $infText = $infText.Replace($match.Value,
            $match.Groups[1].Value + $versionedServiceName + $match.Groups[3].Value)
    }
    if ($serviceMatches.Count -eq 0) {
        throw "INF has no AddService directive to version: $($infFile.FullName)"
    }

    if (-not [regex]::IsMatch($infText, $imageReferencePattern)) {
        throw "INF does not reference the expected driver image: $($infFile.FullName)"
    }
    $infText = [regex]::Replace($infText, $imageReferencePattern, $versionedImageName)
    [System.IO.File]::WriteAllText($infFile.FullName, $infText, [System.Text.Encoding]::ASCII)
}

Write-Output "Package version: $driverVersion"
Write-Output "Versioned driver image: $versionedImageName"

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
$packageInfFiles = @(Get-ChildItem -LiteralPath $packageRoot -Filter '*.inf' -File |
    ForEach-Object FullName)
& $infverif /wbuild 10.0.19041 /w @packageInfFiles
if ($LASTEXITCODE -ne 0) {
    throw "InfVerif failed for the Windows 10 19041+ driver package (exit code $LASTEXITCODE)"
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
