#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall', 'Cleanup')]
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

function Assert-TreePackageCandidates([version] $RequestedVersion, [string] $RequestedService,
    [string] $ExpectedImage) {
    $expectedHash = (Get-FileHash -LiteralPath $ExpectedImage -Algorithm SHA256).Hash
    $treePackages = @(Get-WindowsDriver -Online -All | Where-Object {
        [IO.Path]::GetFileName([string]$_.OriginalFileName) -ieq 'xusb_reconstructed_tree.inf'
    })
    foreach ($packageEntry in $treePackages) {
        $packageVersion = [version]$packageEntry.Version
        if ($packageVersion -gt $RequestedVersion) {
            throw "A higher-version role-3 package is already staged: $($packageEntry.Driver) $packageVersion. Remove it or build a newer replacement package before changing any device."
        }
        if ($packageVersion -ne $RequestedVersion) { continue }

        $exportDirectory = Join-Path ([IO.Path]::GetTempPath()) `
            ('vcam-tree-preflight-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $exportDirectory -Force | Out-Null
        try {
            $storedInf = $null
            $storeRoot = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository')).TrimEnd('\') + '\'
            $originalInfPath = [string]$packageEntry.OriginalFileName
            if (Test-Path -LiteralPath $originalInfPath -PathType Leaf) {
                $originalInfFullPath = [IO.Path]::GetFullPath($originalInfPath)
                if ($originalInfFullPath.StartsWith($storeRoot, [StringComparison]::OrdinalIgnoreCase)) {
                    $storedInf = $originalInfFullPath
                }
            }
            if (-not $storedInf) {
                $publishedInf = [string]$packageEntry.Driver
                if ($publishedInf -notmatch '^oem\d+\.inf$') {
                    throw "Cannot safely inspect same-version staged role-3 package '$publishedInf'; no device change was made."
                }
                $exportOutput = & $pnputil /export-driver $publishedInf $exportDirectory 2>&1
                if ($LASTEXITCODE -ne 0) {
                    throw "Could not export same-version role-3 package $publishedInf for hash preflight (exit code $LASTEXITCODE). No device change was made. $($exportOutput -join ' ')"
                }
                $exportedInf = Get-ChildItem -LiteralPath $exportDirectory -Filter 'xusb_reconstructed_tree.inf' `
                    -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
                if (-not $exportedInf) {
                    throw "Exported package $publishedInf does not contain xusb_reconstructed_tree.inf; no device change was made."
                }
                $storedInf = $exportedInf.FullName
            }

            $storedText = [IO.File]::ReadAllText($storedInf)
            if ($storedText -notmatch '(?im)\broot\\\*xusb_tree\b') { continue }
            $storedVersion = [regex]::Match($storedText,
                '(?im)^\s*DriverVer\s*=\s*[^,\r\n]+,\s*(\d+\.\d+\.\d+\.\d+)\s*$')
            $storedService = [regex]::Match($storedText, '(?im)^\s*AddService\s*=\s*([A-Za-z0-9_.-]+)\s*,')
            $storedImage = [regex]::Match($storedText,
                '(?im)^\s*ServiceBinary\s*=\s*%\d+%\\([^\\\r\n]+\.sys)\s*$')
            if (-not $storedVersion.Success -or -not $storedService.Success -or -not $storedImage.Success -or
                [version]$storedVersion.Groups[1].Value -ne $RequestedVersion) {
                throw "Same-version staged role-3 package $($packageEntry.Driver) has incomplete or inconsistent INF metadata; no device change was made."
            }
            if ($storedService.Groups[1].Value -ine $RequestedService) {
                throw "A same-version role-3 package is staged with service '$($storedService.Groups[1].Value)', not '$RequestedService'; no device change was made."
            }
            $storedImagePath = Join-Path (Split-Path -Parent $storedInf) $storedImage.Groups[1].Value
            if (-not (Test-Path -LiteralPath $storedImagePath -PathType Leaf) -or
                (Get-FileHash -LiteralPath $storedImagePath -Algorithm SHA256).Hash -ne $expectedHash) {
                throw "A same-version role-3 package contains different or unavailable SYS bytes; no device change was made. Increase DriverVer before retrying."
            }
        }
        finally {
            Remove-Item -LiteralPath $exportDirectory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Assert-TreeUsesRequestedPackage([object[]] $Devices, [string] $ExpectedService,
    [version] $ExpectedVersion, [string] $ExpectedImage) {
    foreach ($device in $Devices) {
        $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        $installedVersionText = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
        if ($device.Status -ne 'OK' -or $service -ine $ExpectedService -or
            -not $installedVersionText -or [version]$installedVersionText -ne $ExpectedVersion) {
            throw "Role-3 device $($device.InstanceId) did not bind to the requested service $ExpectedService version $ExpectedVersion. Deployment stopped before updating the HCD."
        }
    }
    Assert-InstalledImage $ExpectedService $ExpectedImage
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
        if ($publishedInf -notmatch '^oem\d+\.inf$') {
            Write-Warning "Could not validate published UCX INF '$publishedInf'; preserving it."
            continue
        }
        if (-not (Export-StaleDriverPackage $publishedInf 'xusb_ucx_hcd.inf')) { continue }
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

function Export-StaleDriverPackage([string] $PublishedInf, [string] $ExpectedInfName) {
    $backupRoot = Join-Path $PSScriptRoot 'build\stale-package-backup'
    $backupDirectory = Join-Path $backupRoot `
        (([IO.Path]::GetFileNameWithoutExtension($PublishedInf)) + '-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
    $output = & $pnputil /export-driver $PublishedInf $backupDirectory 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Could not back up $PublishedInf; it was preserved. $($output -join ' ')"
        return $false
    }
    $exportedInf = Get-ChildItem -LiteralPath $backupDirectory -Filter $ExpectedInfName `
        -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $exportedInf) {
        Write-Warning "The export of $PublishedInf did not contain $ExpectedInfName; the package was preserved."
        return $false
    }
    Write-Verbose "Backed up $PublishedInf to $backupDirectory before removal."
    return $true
}

function Remove-ObsoleteTreeDriverPackages {
    # Keep the active role-3 package and any newer/equal candidate. For older
    # packages, preflight present PnP bindings and let PnPUtil make the final
    # in-use decision. Never use /uninstall or /force here.
    $treeDevices = @(Get-TargetDevices).Tree
    if ($treeDevices.Count -eq 0) {
        Write-Warning 'Could not identify a present role-3 tree device; preserving all reconstructed tree packages.'
        return
    }

    $activeBindings = foreach ($device in $treeDevices) {
        $inf = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
        $versionText = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
        if ($inf -and $versionText) {
            [pscustomobject]@{ Inf = [string]$inf; Version = [version]$versionText }
        }
    }
    $activeInfs = @($activeBindings | ForEach-Object { $_.Inf } | Select-Object -Unique)
    $activeVersions = @($activeBindings | ForEach-Object { $_.Version } | Select-Object -Unique)
    if ($activeBindings.Count -ne $treeDevices.Count -or $activeInfs.Count -ne 1 -or
        $activeVersions.Count -ne 1 -or @($treeDevices | Where-Object { $_.Status -ne 'OK' }).Count -gt 0) {
        Write-Warning 'The active role-3 package/version or device state is ambiguous; preserving all reconstructed tree packages.'
        return
    }
    $activeInf = [string]$activeInfs[0]
    $activeVersion = [version]$activeVersions[0]

    $packages = @(Get-WindowsDriver -Online -All | Where-Object {
        [IO.Path]::GetFileName([string]$_.OriginalFileName) -ieq 'xusb_reconstructed_tree.inf' -and
        $_.ProviderName -eq 'Virtual Device Camera reconstructed driver'
    })
    if ($packages.Count -eq 0) { return }

    # PnPUtil also refuses a package that is still in use. This explicit
    # present-device check avoids asking it to remove known bindings.
    $boundInfs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    try {
        foreach ($device in (Get-PnpDevice -PresentOnly -ErrorAction Stop)) {
            $inf = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
                -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
            if ($inf) { [void]$boundInfs.Add([string]$inf) }
        }
    }
    catch {
        Write-Warning "Could not complete the present-device INF preflight; preserving all inactive tree packages. $($_.Exception.Message)"
        return
    }

    foreach ($packageEntry in $packages) {
        $publishedInf = [string]$packageEntry.Driver
        if ($publishedInf -ieq $activeInf) { continue }
        if ($publishedInf -notmatch '^oem\d+\.inf$') {
            Write-Warning "Could not validate published INF '$publishedInf'; preserving it."
            continue
        }
        try { $packageVersion = [version]$packageEntry.Version }
        catch {
            Write-Warning "Could not parse version for $publishedInf; preserving it."
            continue
        }
        if ($packageVersion -ge $activeVersion) { continue }
        if ($boundInfs.Contains($publishedInf)) {
            Write-Output "Kept bound role-3 package $publishedInf (version $packageVersion)."
            continue
        }

        if (-not (Export-StaleDriverPackage $publishedInf 'xusb_reconstructed_tree.inf')) { continue }

        $output = & $pnputil /delete-driver $publishedInf 2>&1
        if ($LASTEXITCODE -eq 3010) {
            throw "pnputil /delete-driver $publishedInf returned 3010 (reboot required). Cleanup stopped; no reboot was started. $($output -join ' ')"
        }
        if ($LASTEXITCODE -eq 0) {
            Write-Output "Removed stale role-3 driver package $publishedInf (version $packageVersion; active version $activeVersion)."
        } else {
            Write-Warning "Kept role-3 driver package $publishedInf (version $packageVersion); PnP reports it is still in use or cannot be removed. $($output -join ' ')"
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

if ($Action -eq 'Cleanup') {
    Assert-CameraAppClosed
    $devices = Get-TargetDevices
    if ($devices.Tree.Count -eq 0 -or $devices.Controller.Count -eq 0 -or
        @($devices.Tree | Where-Object { $_.Status -ne 'OK' }).Count -gt 0 -or
        @($devices.Controller | Where-Object { $_.Status -ne 'OK' }).Count -gt 0) {
        throw 'Cleanup requires the reconstructed tree and UCX controller to be present and in OK state. No package was removed.'
    }
    Remove-ObsoleteTreeDriverPackages
    Remove-ObsoleteUcxDriverPackages
    Write-Output 'Cleanup finished. Active and newer packages were preserved; PnP refused packages still in use. No reboot was started.'
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

$legacyDriverVersionText = (Get-Content -LiteralPath $legacyInf | Where-Object {
    $_ -match '^\s*DriverVer\s*='
} | Select-Object -First 1) -replace '^.*?,\s*', ''
if (-not $legacyDriverVersionText) {
    throw "DriverVer is missing from the role-3 INF: $legacyInf"
}
$legacyDriverVersion = [version]$legacyDriverVersionText
Assert-TreePackageCandidates -RequestedVersion $legacyDriverVersion `
    -RequestedService $treeServiceName -ExpectedImage $treeImagePath

Invoke-PnpUtil @('/add-driver', $ucxInf)
# Stage and update an already-present tree node; otherwise a successful rerun can
# silently keep executing the older role-3 image already bound from DriverStore.
Invoke-PnpUtil @('/add-driver', $legacyInf, '/install') -AllowNoUpdate
foreach ($device in (Get-TargetDevices).Tree) {
    $installedVersionText = (Get-PnpDeviceProperty -InstanceId $device.InstanceId `
        -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
    if (-not $installedVersionText -or [version]$installedVersionText -ne $legacyDriverVersion) {
        throw "Role-3 device $($device.InstanceId) is not using the exact requested driver version $legacyDriverVersionText. Deployment stopped before updating the HCD."
    }
}
$existingTreeDevices = @(Get-TargetDevices).Tree
if ($existingTreeDevices.Count -gt 0) {
    Assert-TreeUsesRequestedPackage -Devices $existingTreeDevices -ExpectedService $treeServiceName `
        -ExpectedVersion $legacyDriverVersion -ExpectedImage $treeImagePath
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
Assert-TreeUsesRequestedPackage -Devices @($tree) -ExpectedService $treeServiceName `
    -ExpectedVersion $legacyDriverVersion -ExpectedImage $treeImagePath
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
Remove-ObsoleteTreeDriverPackages
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
