#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall')]
    [string] $Action = 'Install',
    [string] $PackagePath = (Join-Path $PSScriptRoot 'build\x64\Release'),
    [ValidateRange(5, 300)]
    [int] $TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$package = [System.IO.Path]::GetFullPath($PackagePath)
$legacyInf = Join-Path $package 'xusb_reconstructed_tree.inf'
$ucxInf = Join-Path $package 'xusb_ucx_hcd.inf'
$treePrefix = 'ROOT\*XUSB_TREE\'
$controllerPrefix = 'PCI\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88\'
$oldUsbOnlyPrefix = 'ROOT\*XUSB_ENUMBUS\'
$oldRootHubPrefix = 'VFXSUSB\VROOTHUB&VIRTUAL&FANXIUSHU\'
$pnputil = Join-Path $env:WINDIR 'System32\pnputil.exe'
$script:RebootRequired = $false

if ([Environment]::OSVersion.Version.Build -lt 19041) {
    throw 'This package workflow requires Windows 10 version 2004 (build 19041) or newer.'
}

function Invoke-PnpUtil([string[]] $Arguments) {
    & $pnputil @Arguments
    if ($LASTEXITCODE -eq 3010) {
        $script:RebootRequired = $true
        return
    }
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Get-TargetDevices {
    Import-Module PnpDevice -ErrorAction Stop
    $all = Get-PnpDevice -PresentOnly -ErrorAction Stop
    [pscustomobject]@{
        Tree = @($all | Where-Object { $_.InstanceId.StartsWith($treePrefix, [StringComparison]::OrdinalIgnoreCase) })
        Controller = @($all | Where-Object { $_.InstanceId.StartsWith($controllerPrefix, [StringComparison]::OrdinalIgnoreCase) })
        PreviousControlProviders = @($all | Where-Object {
            $_.InstanceId.StartsWith($oldUsbOnlyPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            $_.InstanceId.StartsWith($oldRootHubPrefix, [StringComparison]::OrdinalIgnoreCase)
        })
    }
}

function Assert-CameraAppClosed {
    $running = Get-Process -Name 'virtual-device-camera' -ErrorAction SilentlyContinue
    if ($running) {
        throw 'Close virtual-device-camera before changing the virtual USB device tree. The application selects the first openable GUID_XUSB_VIRT_INTERFACE.'
    }
}

function Find-DevCon {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools'
    if (-not (Test-Path -LiteralPath $kitsRoot)) {
        throw 'DevCon was not found. Install the Windows Driver Kit tools to create the root-enumerated role-3 device.'
    }
    $found = Get-ChildItem -LiteralPath $kitsRoot -Filter devcon.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\devcon\.exe$' } |
        Sort-Object { [version]($_.Directory.Parent.Name) } -Descending |
        Select-Object -First 1
    if (-not $found) {
        throw 'The x64 DevCon utility was not found under Windows Kits\10\Tools.'
    }
    return $found.FullName
}

function Wait-For([scriptblock] $Predicate, [string] $Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $value = & $Predicate
        if ($value) { return $value }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Description. Check Device Manager and the setupapi.dev.log."
}

if ($Action -eq 'Uninstall') {
    Assert-CameraAppClosed
    $devices = Get-TargetDevices
    foreach ($device in $devices.Controller) {
        Invoke-PnpUtil @('/remove-device', $device.InstanceId)
    }
    foreach ($device in $devices.Tree) {
        Invoke-PnpUtil @('/remove-device', $device.InstanceId, '/subtree')
    }
    foreach ($device in $devices.PreviousControlProviders) {
        Invoke-PnpUtil @('/remove-device', $device.InstanceId, '/subtree')
    }
    Write-Output 'Removed matching role-2, role-3 and previous GUID_XUSB_VIRT_INTERFACE provider device nodes. Driver packages remain staged in the Windows Driver Store.'
    Write-Warning 'Rollback requires re-adding the original root enumerator device with its preserved legacy INF; the old package remains in DriverStore but its removed root device is not recreated automatically.'
    if ($script:RebootRequired) { Write-Warning 'Windows reported that a reboot is required to finish device removal.' }
    return
}

Assert-CameraAppClosed
foreach ($requiredFile in @(
    $legacyInf,
    (Join-Path $package 'xusb_reconstructed_tree.cat'),
    $ucxInf,
    (Join-Path $package 'xusb_ucx_hcd.cat'),
    (Join-Path $package 'xusb_driver.sys'),
    (Join-Path $package 'VirtualCameraUcxHcd.sys'))) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Replacement package is incomplete: $requiredFile"
    }
}
foreach ($catalog in @(
    (Join-Path $package 'xusb_reconstructed_tree.cat'),
    (Join-Path $package 'xusb_ucx_hcd.cat'))) {
    $signature = Get-AuthenticodeSignature -LiteralPath $catalog
    if ($signature.Status -ne 'Valid') {
        throw "Catalog signature is not trusted: $catalog ($($signature.Status)). Sign it with a trusted release/test certificate first."
    }
}

Invoke-PnpUtil @('/add-driver', $ucxInf)
Invoke-PnpUtil @('/add-driver', $legacyInf)

$devices = Get-TargetDevices
foreach ($device in $devices.PreviousControlProviders) {
    Invoke-PnpUtil @('/remove-device', $device.InstanceId, '/subtree')
}
if ($devices.PreviousControlProviders.Count -gt 0) {
    Wait-For {
        $remaining = (Get-TargetDevices).PreviousControlProviders
        if ($remaining.Count -eq 0) { $true } else { $null }
    } 'old USB-only/Root Hub GUID_XUSB_VIRT_INTERFACE providers to be removed'
}
if ((Get-TargetDevices).PreviousControlProviders.Count -gt 0) {
    throw 'A previous GUID_XUSB_VIRT_INTERFACE provider is still present; refusing to continue because the unchanged application may open it instead of the UCX HCD.'
}

$devices = Get-TargetDevices
if ($devices.Tree.Count -eq 0) {
    $devcon = Find-DevCon
    & $devcon install $legacyInf 'root\*xusb_tree'
    if ($LASTEXITCODE -ne 0) {
        throw "DevCon failed to create/install root\*xusb_tree (exit code $LASTEXITCODE)."
    }
}

$tree = Wait-For {
    (Get-TargetDevices).Tree | Select-Object -First 1
} 'the root\*xusb_tree enumerator'
Invoke-PnpUtil @('/scan-devices')
$controller = Wait-For {
    (Get-TargetDevices).Controller | Select-Object -First 1
} 'the role-2 host-controller child emitted by role 3'

Invoke-PnpUtil @('/add-driver', $ucxInf, '/install')
$boundController = Wait-For {
    $device = Get-PnpDevice -InstanceId $controller.InstanceId -PresentOnly -ErrorAction SilentlyContinue
    if (-not $device) { return $null }
    $service = (Get-PnpDeviceProperty -InstanceId $controller.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    if ($service -ieq 'ucx_hcd') { return $device }
    return $null
} 'the role-2 child to bind to the ucx_hcd service'

if ((Get-TargetDevices).PreviousControlProviders.Count -gt 0) {
    throw 'A previous GUID_XUSB_VIRT_INTERFACE provider became present during installation; the application could select the wrong device.'
}

Write-Output "role-3 tree device present: $($tree.InstanceId)"
Write-Output "role-2 UCX HCD service bound: $($boundController.InstanceId)"
Write-Output 'This confirms PnP binding only; it does not validate USB enumeration, camera streaming, or functional replacement.'
if ($script:RebootRequired) { Write-Warning 'Windows reported that a reboot is required to finish driver installation.' }
