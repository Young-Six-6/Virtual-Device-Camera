[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Install')]
    [Parameter(Mandatory = $true, ParameterSetName = 'Update')]
    [string] $PackageDirectory,

    [Parameter(Mandatory = $true, ParameterSetName = 'Update')]
    [switch] $UpdateExisting,

    [Parameter(Mandatory = $true, ParameterSetName = 'Remove')]
    [switch] $Remove,

    [string] $DevConPath
)

$ErrorActionPreference = 'Stop'
$hardwareId = 'root\*xusb_enumbus'
$rootHubHardwareId = 'vFxsUSB\vRootHUB&Virtual&Fanxiushu'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this deployment script from an elevated PowerShell session.'
}

if (-not $DevConPath) {
    $toolsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools'
    $DevConPath = Get-ChildItem -LiteralPath $toolsRoot -Directory -ErrorAction Stop |
        Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object {
            $candidate = Join-Path $_.FullName 'x64\devcon.exe'
            if (Test-Path -LiteralPath $candidate) { $candidate }
        } |
        Select-Object -First 1
}
if (-not $DevConPath -or -not (Test-Path -LiteralPath $DevConPath)) {
    throw 'DevCon x64 was not found. Install WDK tools or pass -DevConPath.'
}

if ($PSCmdlet.ParameterSetName -eq 'Remove') {
    $matchingDevices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object {
            $_.InstanceId.StartsWith('ROOT\*XUSB_ENUMBUS', [StringComparison]::OrdinalIgnoreCase) -or
            $_.InstanceId.StartsWith('VFXSUSB\VROOTHUB&VIRTUAL&FANXIUSHU', [StringComparison]::OrdinalIgnoreCase)
        })
    foreach ($device in $matchingDevices) {
        & $DevConPath remove $device.InstanceId
        if ($LASTEXITCODE -ne 0) {
            throw "DevCon could not remove device '$($device.InstanceId)' (exit code $LASTEXITCODE)"
        }
    }
    Write-Output "Removed $($matchingDevices.Count) reconstructed USB device instance(s). Driver Store packages were left installed."
    return
}

$packageRoot = [System.IO.Path]::GetFullPath($PackageDirectory)
$busInfPath = Join-Path $packageRoot 'xusb_reconstructed_bus.inf'
$busCatalogPath = Join-Path $packageRoot 'xusb_reconstructed_bus.cat'
$rootHubInfPath = Join-Path $packageRoot 'xusb_reconstructed_usb.inf'
$rootHubCatalogPath = Join-Path $packageRoot 'xusb_reconstructed_usb.cat'
foreach ($requiredFile in @($busInfPath, $busCatalogPath, $rootHubInfPath, $rootHubCatalogPath)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "The USB-only driver package is incomplete: $requiredFile"
    }
}

function Get-InfPackageMetadata {
    param([Parameter(Mandatory = $true)][string] $InfPath)

    $text = [System.IO.File]::ReadAllText($InfPath)
    $versionMatch = [regex]::Match($text, '(?im)^\s*DriverVer\s*=\s*[^,\r\n]+,\s*(\d+\.\d+\.\d+\.\d+)\s*$')
    $serviceMatch = [regex]::Match($text, '(?im)^\s*AddService\s*=\s*([A-Za-z0-9_.-]+)\s*,')
    $imageMatch = [regex]::Match($text, '(?im)^\s*ServiceBinary\s*=\s*%\d+%\\([^\\\r\n]+\.sys)\s*$')
    if (-not $versionMatch.Success -or -not $serviceMatch.Success -or -not $imageMatch.Success) {
        throw "The INF lacks a versioned DriverVer, AddService or ServiceBinary entry: $InfPath. Rebuild it with package.ps1."
    }

    [pscustomobject]@{
        InfPath = $InfPath
        Version = $versionMatch.Groups[1].Value
        Service = $serviceMatch.Groups[1].Value
        ImageName = $imageMatch.Groups[1].Value
    }
}

function Get-NormalizedServiceImagePath {
    param([Parameter(Mandatory = $true)][string] $PathName)

    $path = [Environment]::ExpandEnvironmentVariables($PathName.Trim().Trim('"'))
    if ($path.StartsWith('\??\', [StringComparison]::OrdinalIgnoreCase)) {
        $path = $path.Substring(4)
    }
    if ($path.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $path = Join-Path $env:SystemRoot $path.Substring('\SystemRoot\'.Length)
    }
    return $path
}

$busPackage = Get-InfPackageMetadata -InfPath $busInfPath
$rootHubPackage = Get-InfPackageMetadata -InfPath $rootHubInfPath
if ($busPackage.Version -ne $rootHubPackage.Version) {
    throw "The bus and Root Hub packages have different DriverVer values ($($busPackage.Version), $($rootHubPackage.Version)). Rebuild them together."
}
$versionSuffix = $busPackage.Version.Replace('.', '_')
$expectedImageName = "xusb_driver_$versionSuffix.sys"
if ($busPackage.ImageName -ne $expectedImageName -or $rootHubPackage.ImageName -ne $expectedImageName -or
    $busPackage.Service -notmatch "_$([regex]::Escape($versionSuffix))$" -or
    $rootHubPackage.Service -notmatch "_$([regex]::Escape($versionSuffix))$") {
    throw "The package does not use service and image names isolated to DriverVer $($busPackage.Version). Rebuild it with package.ps1."
}
$driverPath = Join-Path $packageRoot $expectedImageName
if (-not (Test-Path -LiteralPath $driverPath -PathType Leaf)) {
    throw "The versioned driver image referenced by the INFs is missing: $driverPath"
}

$packageHash = (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash
foreach ($serviceName in @($busPackage.Service, $rootHubPackage.Service)) {
    $installedService = Get-CimInstance -ClassName Win32_SystemDriver -Filter "Name='$serviceName'" -ErrorAction SilentlyContinue
    if ($installedService) {
        $serviceImagePath = Get-NormalizedServiceImagePath -PathName ([string]$installedService.PathName)
        if (-not [string]::Equals(
            [System.IO.Path]::GetFileName($serviceImagePath),
            $expectedImageName,
            [StringComparison]::OrdinalIgnoreCase)) {
            throw "Installed service '$serviceName' points to an unexpected image: $($installedService.PathName)"
        }
        if (-not (Test-Path -LiteralPath $serviceImagePath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $serviceImagePath -Algorithm SHA256).Hash -ne $packageHash) {
            throw "Installed service '$serviceName' has a stale or different image. Increase the package version; deployment will not overwrite a possibly loaded driver."
        }
    }
}

Write-Output "Deploying driver version $($busPackage.Version) ($expectedImageName)."

if ($PSCmdlet.ParameterSetName -eq 'Install') {
    $existingBusDevices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId.StartsWith('ROOT\*XUSB_ENUMBUS', [StringComparison]::OrdinalIgnoreCase) })
    if ($existingBusDevices.Count -gt 0) {
        throw "A reconstructed USB-only bus is already present. Use -UpdateExisting to update it without creating a duplicate device."
    }
}

foreach ($catalogPath in @($busCatalogPath, $rootHubCatalogPath)) {
    $signature = Get-AuthenticodeSignature -LiteralPath $catalogPath
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "The package catalog is not trusted on this computer ($($signature.Status)): $catalogPath. Sign it through the appropriate Microsoft/test-signing workflow first."
    }
}

foreach ($infPath in @($busInfPath, $rootHubInfPath)) {
    & "$env:SystemRoot\System32\pnputil.exe" /add-driver $infPath
    if ($LASTEXITCODE -ne 0) {
        throw "PnPUtil could not stage the signed driver package (exit code $LASTEXITCODE): $infPath"
    }
}

if ($PSCmdlet.ParameterSetName -eq 'Update') {
    & $DevConPath update $busInfPath $hardwareId
} else {
    & $DevConPath install $busInfPath $hardwareId
}
if ($LASTEXITCODE -ne 0) {
    throw "DevCon could not bind the reconstructed driver (exit code $LASTEXITCODE)"
}

function Assert-DeviceUsesService {
    param(
        [Parameter(Mandatory = $true)][string] $InstancePrefix,
        [Parameter(Mandatory = $true)][string] $ExpectedService,
        [Parameter(Mandatory = $true)][string] $ExpectedImageName,
        [Parameter(Mandatory = $true)][string] $ExpectedImageHash
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId.StartsWith($InstancePrefix, [StringComparison]::OrdinalIgnoreCase) })
        if ($devices.Count -gt 0) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($devices.Count -eq 0) {
        throw "No present device was found after binding '$ExpectedService' (prefix '$InstancePrefix'). No automatic restart was attempted."
    }

    foreach ($device in $devices) {
        if ($device.Status -ne 'OK') {
            throw "Device '$($device.InstanceId)' is present but not started (status '$($device.Status)')."
        }
        $serviceProperty = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop
        if ([string]$serviceProperty.Data -ne $ExpectedService) {
            throw "Device '$($device.InstanceId)' is bound to '$($serviceProperty.Data)', not the requested versioned service '$ExpectedService'. Deployment stopped; inspect PnP state before retrying."
        }
        $installedService = Get-CimInstance -ClassName Win32_SystemDriver `
            -Filter "Name='$ExpectedService'" -ErrorAction SilentlyContinue
        if (-not $installedService -or -not $installedService.PathName) {
            throw "The bound service '$ExpectedService' has no discoverable image path."
        }
        $serviceImagePath = Get-NormalizedServiceImagePath -PathName ([string]$installedService.PathName)
        if ([System.IO.Path]::GetFileName($serviceImagePath) -ine $ExpectedImageName -or
            -not (Test-Path -LiteralPath $serviceImagePath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $serviceImagePath -Algorithm SHA256).Hash -ne $ExpectedImageHash) {
            throw "Device '$($device.InstanceId)' is bound to '$ExpectedService', but its service image does not match the requested package."
        }
    }
}

Assert-DeviceUsesService -InstancePrefix 'ROOT\*XUSB_ENUMBUS' -ExpectedService $busPackage.Service `
    -ExpectedImageName $expectedImageName -ExpectedImageHash $packageHash

$rootHubDevices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'VFxsUSB\vRootHUB&Virtual&Fanxiushu*' })
if ($rootHubDevices.Count -gt 0) {
    & $DevConPath update $rootHubInfPath $rootHubHardwareId
    if ($LASTEXITCODE -ne 0) {
        throw "DevCon could not update the existing Root Hub provider (exit code $LASTEXITCODE)"
    }
    Assert-DeviceUsesService -InstancePrefix 'VFXSUSB\VROOTHUB&VIRTUAL&FANXIUSHU' `
        -ExpectedService $rootHubPackage.Service -ExpectedImageName $expectedImageName -ExpectedImageHash $packageHash
}

Write-Output "The USB-only driver version $($busPackage.Version) is installed, and any present Root Hub provider was updated. Restart the camera application before opening the virtual camera. No Windows restart was requested."
