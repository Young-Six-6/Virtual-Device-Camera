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
$treeHardwareId = 'ROOT\*XUSB_TREE'
$controllerPrefix = 'PCI\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88\'
$oldUsbOnlyHardwareId = 'ROOT\*XUSB_ENUMBUS'
$oldRootHubHardwareId = 'VFXSUSB\VROOTHUB&VIRTUAL&FANXIUSHU'
$pnputil = Join-Path $env:WINDIR 'System32\pnputil.exe'

if ([Environment]::OSVersion.Version.Build -lt 19041) {
    throw 'This package workflow requires Windows 10 version 2004 (build 19041) or newer.'
}

function Invoke-PnpUtil([string[]] $Arguments, [switch] $AllowNoUpdate) {
    & $pnputil @Arguments
    if ($LASTEXITCODE -eq 3010) {
        throw "pnputil $($Arguments -join ' ') returned 3010 (reboot required). Deployment stopped immediately; no reboot was started. The operation may have partially changed PnP state, so inspect the target devices before retrying."
    }
    if ($LASTEXITCODE -eq 259 -and $AllowNoUpdate) {
        return
    }
    if ($LASTEXITCODE -ne 0) {
        throw "pnputil $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Get-TargetDevices {
    Import-Module PnpDevice -ErrorAction Stop
    $all = Get-PnpDevice -PresentOnly -ErrorAction Stop
    $rootDevices = $all | Where-Object {
        $_.InstanceId.StartsWith('ROOT\', [StringComparison]::OrdinalIgnoreCase)
    }
    $identified = foreach ($device in $rootDevices) {
        $hardwareIds = @((Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data)
        [pscustomobject]@{ Device = $device; HardwareIds = $hardwareIds }
    }
    [pscustomobject]@{
        Tree = @($identified | Where-Object { $_.HardwareIds -contains $treeHardwareId } | ForEach-Object { $_.Device })
        Controller = @($all | Where-Object { $_.InstanceId.StartsWith($controllerPrefix, [StringComparison]::OrdinalIgnoreCase) })
        PreviousControlProviders = @($identified | Where-Object {
            ($_.HardwareIds | Where-Object {
                $_.StartsWith($oldUsbOnlyHardwareId, [StringComparison]::OrdinalIgnoreCase) -or
                $_.StartsWith($oldRootHubHardwareId, [StringComparison]::OrdinalIgnoreCase)
            }).Count -gt 0
        } | ForEach-Object { $_.Device })
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
    $diagnostics = foreach ($device in (Get-TargetDevices).Controller) {
        $problemCode = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
        $problemStatus = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_ProblemStatus' -ErrorAction SilentlyContinue).Data
        $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        $version = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
        $problemStatusText = if ($null -ne $problemStatus) {
            '0x{0:X8}' -f [uint32]$problemStatus
        } else { 'unavailable' }
        "[$($device.Status)] $($device.InstanceId): service=$service version=$version problem=$problemCode problemStatus=$problemStatusText"
    }
    $deviceDetails = if ($diagnostics) { ' Current controller state: ' + ($diagnostics -join '; ') } else { '' }
    throw "Timed out waiting for $Description.$deviceDetails Check Device Manager and the setupapi.dev.log."
}

function Get-RootHubForController([string] $ControllerInstanceId) {
    foreach ($device in (Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object {
        $_.InstanceId -match '^USB\\ROOT_HUB(?:20|30)\\'
    })) {
        $parent = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
        if ($parent -ieq $ControllerInstanceId -and $device.Status -eq 'OK') { return $device }
    }
    return $null
}

function Assert-InstalledImage([string] $ServiceName, [string] $ExpectedImage) {
    $service = Get-CimInstance Win32_SystemDriver -Filter "Name='$ServiceName'"
    # Win32_SystemDriver.State can remain stale after a versioned PnP rebind.
    # ServiceController queries SCM directly and is authoritative for runtime state.
    $runtimeService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (-not $service -or -not $service.PathName -or
        -not $runtimeService -or [string]$runtimeService.Status -ne 'Running') {
        throw "Driver service $ServiceName is not running with a discoverable image path."
    }
    $installedPath = [Environment]::ExpandEnvironmentVariables($service.PathName.Trim().Trim('"'))
    if ($installedPath.StartsWith('\??\', [StringComparison]::OrdinalIgnoreCase)) {
        $installedPath = $installedPath.Substring(4)
    }
    $expectedHash = (Get-FileHash -LiteralPath $ExpectedImage -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath $installedPath -Algorithm SHA256).Hash
    if ($installedHash -ne $expectedHash) {
        throw "Service $ServiceName is running image $installedPath (SHA-256 $installedHash), not the requested package image (SHA-256 $expectedHash). Increase DriverVer and reinstall the package."
    }
}

function Assert-NoImageVersionCollision([string] $ServiceName, [string] $ExpectedImage) {
    # Check before staging or changing devices. Reusing a service/image name
    # for different bytes can leave a prior image resident until the next boot.
    $service = Get-CimInstance Win32_SystemDriver -Filter "Name='$ServiceName'"
    if (-not $service) { return }
    if (-not $service.PathName) {
        throw "Existing service $ServiceName has no discoverable image. Increase its INF DriverVer before deploying another build."
    }
    $installedPath = [Environment]::ExpandEnvironmentVariables($service.PathName.Trim().Trim('"'))
    if ($installedPath.StartsWith('\??\', [StringComparison]::OrdinalIgnoreCase)) {
        $installedPath = $installedPath.Substring(4)
    }
    if (-not (Test-Path -LiteralPath $installedPath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $installedPath -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $ExpectedImage -Algorithm SHA256).Hash) {
        throw "Service $ServiceName already identifies a different or unavailable image. Increase its INF DriverVer, rebuild and sign the package before retrying. No device change has been started."
    }
}

function Remove-ObsoleteUcxDriverPackages {
    # Repeated test installs leave multiple DriverStore INFs referencing the
    # same kernel service. Windows documents this as a possible Code 38 cause.
    # Delete without /uninstall or /force: PnP refuses packages still in use.
    $packages = Get-WindowsDriver -Online -All | Where-Object {
        $_.OriginalFileName -match '\\xusb_ucx_hcd\.inf$' -and
        $_.ProviderName -eq 'Virtual Device Camera Project'
    }
    $activeInfs = @(
        foreach ($device in (Get-TargetDevices).Controller) {
            (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
                -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
        }
    ) | Where-Object { $_ } | ForEach-Object { ([string]$_).ToLowerInvariant() }
    if ($packages.Count -gt 0 -and $activeInfs.Count -eq 0) {
        Write-Warning 'Could not identify the active UCX INF; preserving all matching packages.'
        return
    }
    foreach ($packageEntry in $packages) {
        $publishedInf = [string]$packageEntry.Driver
        if ($activeInfs -contains $publishedInf.ToLowerInvariant()) { continue }
        $output = & $pnputil /delete-driver $publishedInf 2>&1
        if ($LASTEXITCODE -eq 3010) {
            throw "pnputil /delete-driver $publishedInf returned 3010 (reboot required). Deployment stopped; no reboot was started. $($output -join ' ')"
        }
        if ($LASTEXITCODE -eq 0) {
            Write-Output "Removed stale UCX driver package $publishedInf (version $($packageEntry.Version))."
        } else {
            Write-Warning "Kept UCX driver package $publishedInf (version $($packageEntry.Version)); PnP reports it is still in use or cannot be removed. $($output -join ' ')"
        }
    }
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
    return
}

$ucxInfText = [System.IO.File]::ReadAllText($ucxInf)
$ucxServiceMatch = [regex]::Match($ucxInfText, '(?im)^\s*AddService\s*=\s*([^,\s]+)')
$ucxImageMatch = [regex]::Match($ucxInfText, '(?im)^\s*ServiceBinary\s*=\s*%13%\\([^\\\r\n]+)\s*$')
$ucxCatalogMatch = [regex]::Match($ucxInfText, '(?im)^\s*CatalogFile\.NTamd64\s*=\s*([^\s]+)\s*$')
if (-not $ucxServiceMatch.Success -or -not $ucxImageMatch.Success -or -not $ucxCatalogMatch.Success) {
    throw "Could not read the UCX service name/image/catalog from $ucxInf"
}
$ucxServiceName = $ucxServiceMatch.Groups[1].Value
$ucxImageName = $ucxImageMatch.Groups[1].Value
$ucxCatalogName = $ucxCatalogMatch.Groups[1].Value
$ucxImagePath = Join-Path $package $ucxImageName
$treeInfText = [IO.File]::ReadAllText($legacyInf)
$treeServiceMatch = [regex]::Match($treeInfText, '(?im)^\s*AddService\s*=\s*([^,\s]+)')
$treeImageMatch = [regex]::Match($treeInfText, '(?im)^\s*ServiceBinary\s*=\s*%13%\\([^\\\r\n]+)\s*$')
if (-not $treeServiceMatch.Success -or -not $treeImageMatch.Success) {
    throw "Could not parse the tree service/image from $legacyInf"
}
$treeServiceName = $treeServiceMatch.Groups[1].Value
$treeImagePath = Join-Path $package $treeImageMatch.Groups[1].Value

Assert-CameraAppClosed
foreach ($requiredFile in @(
    $legacyInf,
    (Join-Path $package 'xusb_reconstructed_tree.cat'),
    $ucxInf,
    (Join-Path $package $ucxCatalogName),
    $treeImagePath,
    $ucxImagePath)) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Replacement package is incomplete: $requiredFile"
    }
}
foreach ($catalog in @(
    (Join-Path $package 'xusb_reconstructed_tree.cat'),
    (Join-Path $package $ucxCatalogName))) {
    $signature = Get-AuthenticodeSignature -LiteralPath $catalog
    if ($signature.Status -ne 'Valid') {
        throw "Catalog signature is not trusted: $catalog ($($signature.Status)). Sign it with a trusted release/test certificate first."
    }
}

Assert-NoImageVersionCollision $treeServiceName $treeImagePath
Assert-NoImageVersionCollision $ucxServiceName $ucxImagePath
Remove-ObsoleteUcxDriverPackages

Invoke-PnpUtil @('/add-driver', $ucxInf)
# Stage and update an already-present tree node; otherwise a successful rerun can
# silently keep executing the older role-3 image already bound from DriverStore.
Invoke-PnpUtil @('/add-driver', $legacyInf, '/install') -AllowNoUpdate
$legacyDriverVersionText = (Get-Content -LiteralPath $legacyInf | Where-Object {
    $_ -match '^\s*DriverVer\s*='
} | Select-Object -First 1) -replace '^.*?,\s*', ''
if (-not $legacyDriverVersionText) {
    throw "DriverVer is missing from the role-3 INF: $legacyInf"
}
$legacyDriverVersion = [version]$legacyDriverVersionText
foreach ($device in (Get-TargetDevices).Tree) {
    $installedVersionText = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
        -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
    if (-not $installedVersionText -or [version]$installedVersionText -lt $legacyDriverVersion) {
        throw "Role-3 device $($device.InstanceId) is not using the requested driver version $legacyDriverVersionText."
    }
}

$devices = Get-TargetDevices
if ($devices.Tree.Count -eq 0) {
    $devcon = Find-DevCon
    & $devcon install $legacyInf 'root\*xusb_tree'
    $devconExitCode = $LASTEXITCODE
    if ($devconExitCode -eq 1) {
        throw 'DevCon created/updated the root device but reported that a reboot is required (exit code 1). Deployment stopped; no reboot was started. Inspect PnP state before retrying.'
    }
    if ($devconExitCode -ne 0) {
        throw "DevCon failed to create/install root\*xusb_tree (exit code $devconExitCode)."
    }
}

$tree = Wait-For {
    $candidate = (Get-TargetDevices).Tree | Select-Object -First 1
    if ($candidate -and $candidate.Status -eq 'OK') { $candidate } else { $null }
} 'the root\*xusb_tree enumerator to start successfully'
Invoke-PnpUtil @('/scan-devices')
$controller = Wait-For {
    (Get-TargetDevices).Controller | Select-Object -First 1
} 'the role-2 host-controller child emitted by role 3'

$ucxDriverVersionText = (Get-Content -LiteralPath $ucxInf | Where-Object {
    $_ -match '^\s*DriverVer\s*='
} | Select-Object -First 1) -replace '^.*?,\s*', ''
if (-not $ucxDriverVersionText) {
    throw "DriverVer is missing from the role-2 INF: $ucxInf"
}
$ucxDriverVersion = [version]$ucxDriverVersionText
Invoke-PnpUtil @('/add-driver', $ucxInf, '/install') -AllowNoUpdate
$boundController = Wait-For {
    $device = Get-PnpDevice -InstanceId $controller.InstanceId -PresentOnly -ErrorAction SilentlyContinue
    if (-not $device) { return $null }
    $service = (Get-PnpDeviceProperty -InstanceId $controller.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    $installedVersionText = (Get-PnpDeviceProperty -InstanceId $controller.InstanceId `
        -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
    if ($service -ieq $ucxServiceName -and $device.Status -eq 'OK' -and $installedVersionText -and
        [version]$installedVersionText -ge $ucxDriverVersion) { return $device }
    return $null
} "the role-2 child to bind to $ucxServiceName $ucxDriverVersionText or newer and start successfully"

try {
    $rootHub = Wait-For {
        Get-RootHubForController $boundController.InstanceId
    } 'the started USB root hub child beneath the new UCX HCD'
}
catch {
    $stateQuery = Join-Path $package 'query-ucx-state.ps1'
    if (Test-Path -LiteralPath $stateQuery -PathType Leaf) {
        Write-Warning 'The root hub did not appear. UCX initialization diagnostics follow:'
        try { & $stateQuery } catch { Write-Warning "UCX state query failed: $($_.Exception.Message)" }
    }
    throw
}

Assert-InstalledImage $treeServiceName $treeImagePath
Assert-InstalledImage $ucxServiceName $ucxImagePath
Remove-ObsoleteUcxDriverPackages

# Keep the old control-interface provider available until the replacement has a
# running controller, the expected images, and its own started root-hub child.
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
    throw 'A previous GUID_XUSB_VIRT_INTERFACE provider became present during installation; the application could select the wrong device.'
}

Write-Output "role-3 tree device present: $($tree.InstanceId)"
Write-Output "role-2 UCX HCD service $ucxServiceName bound: $($boundController.InstanceId)"
Write-Output "UCX root hub started: $($rootHub.InstanceId)"
Write-Output 'Installed role-3 and UCX HCD image hashes match the requested package.'
Write-Output 'This confirms PnP binding only; it does not validate USB enumeration, camera streaming, or functional replacement.'
