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
    & $DevConPath remove $hardwareId
    if ($LASTEXITCODE -ne 0) {
        throw "DevCon remove failed with exit code $LASTEXITCODE"
    }
    return
}

$packageRoot = [System.IO.Path]::GetFullPath($PackageDirectory)
$busInfPath = Join-Path $packageRoot 'xusb_reconstructed_bus.inf'
$busCatalogPath = Join-Path $packageRoot 'xusb_reconstructed_bus.cat'
$rootHubInfPath = Join-Path $packageRoot 'xusb_reconstructed_usb.inf'
$rootHubCatalogPath = Join-Path $packageRoot 'xusb_reconstructed_usb.cat'
$driverPath = Join-Path $packageRoot 'xusb_driver.sys'
foreach ($requiredFile in @($busInfPath, $busCatalogPath, $rootHubInfPath, $rootHubCatalogPath, $driverPath)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "The USB-only driver package is incomplete: $requiredFile"
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

$rootHubDevices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'VFxsUSB\vRootHUB&Virtual&Fanxiushu*' })
if ($rootHubDevices.Count -gt 0) {
    & $DevConPath update $rootHubInfPath $rootHubHardwareId
    if ($LASTEXITCODE -ne 0) {
        throw "DevCon could not update the existing Root Hub provider (exit code $LASTEXITCODE)"
    }
}

Write-Output 'The USB-only driver is installed, and any present Root Hub provider was updated. Restart the camera application before opening the virtual camera.'
