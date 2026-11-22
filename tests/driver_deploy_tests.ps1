[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$deployScript = Join-Path $repoRoot 'drivers\usb-bus-legacy\replacement-package\replacement-deploy.ps1'
$ucxSource = Join-Path $repoRoot 'drivers\usb-bus-legacy\ucx-driver\src\ucx_hcd.c'
$parseTokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($deployScript, [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$defaultDeployScript = Join-Path $repoRoot 'drivers\usb-bus-legacy\driver-source\deploy.ps1'
$defaultTokens = $null
$defaultErrors = $null
$defaultAst = [Management.Automation.Language.Parser]::ParseFile($defaultDeployScript, [ref]$defaultTokens, [ref]$defaultErrors)
if ($defaultErrors.Count) { throw ($defaultErrors | Out-String) }

$ucxText = [IO.File]::ReadAllText($ucxSource)
$replacementText = [IO.File]::ReadAllText($deployScript)
$removeNotify = [regex]::Match($ucxText,
    '(?s)static\s+VOID\s+VhcdNotifySlotRemoval\s*\([^)]*\)\s*\{(?<body>.*?)\n\}').Groups['body'].Value
if (-not $removeNotify -or
    $removeNotify -notmatch 'if\s*\(RemoveEvent\s*!=\s*NULL\)\s*\{\s*KeSetEvent\(RemoveEvent,\s*IO_NO_INCREMENT,\s*FALSE\);\s*ObDereferenceObject\(RemoveEvent\);') {
    throw 'Failed removal-work-item allocation must still signal and release the client remove event.'
}
$interruptTxMatch = [regex]::Match($ucxText,
    '(?s)VOID\s+VhcdEvtRootHubInterruptTx\s*\([^)]*\)\s*\{(?<body>.*?)\n\}')
$interruptTxBody = if ($interruptTxMatch.Success) { $interruptTxMatch.Groups['body'].Value } else { '' }
$interruptLengthCheck = $interruptTxBody.IndexOf('urb->UrbHeader.Length < sizeof(urb->UrbBulkOrInterruptTransfer)')
$interruptLengthRead = $interruptTxBody.IndexOf('urb->UrbBulkOrInterruptTransfer.TransferBufferLength')
if (-not $interruptTxMatch.Success -or $interruptLengthCheck -lt 0 -or
    $interruptLengthRead -lt 0 -or $interruptLengthCheck -gt $interruptLengthRead) {
    throw 'Root-hub interrupt URBs must validate their structure length before reading transfer fields.'
}
$firstPackageStage = $replacementText.IndexOf("Invoke-PnpUtil @('/add-driver', `$ucxInf)", [StringComparison]::Ordinal)
if ($firstPackageStage -lt 0) { throw 'Replacement package staging call was not found.' }
$cleanupAfterValidation = $replacementText.IndexOf('Remove-ObsoleteUcxDriverPackages', $firstPackageStage, [StringComparison]::Ordinal)
$hcdImageCheck = $replacementText.IndexOf('Assert-InstalledImage $ucxServiceName $ucxImagePath', $firstPackageStage, [StringComparison]::Ordinal)
if ($firstPackageStage -lt 0 -or $cleanupAfterValidation -lt $hcdImageCheck -or $hcdImageCheck -lt 0 -or
    $replacementText -notmatch '\$devconExitCode\s+-eq\s+1' -or
    $replacementText -match '(?im)^\s*(Restart-Computer|shutdown\.exe)\b') {
    throw 'Replacement deployment must preserve rollback packages until both images validate and must stop instead of automatically rebooting.'
}
$callerContextMatch = [regex]::Match($ucxText,
    '(?s)VOID\s+VhcdEvtIoInCallerContext\s*\([^)]*\)\s*\{(?<body>.*?)\n\}')
if (-not $callerContextMatch.Success -or
    $callerContextMatch.Groups['body'].Value -notmatch '!=\s*IOCTL_PDO_ADD_SUPERSPEED' -or
    $callerContextMatch.Groups['body'].Value -notmatch '==\s*IOCTL_PDO_ADD_SUPERSPEED' -or
    $ucxText -notmatch '#define\s+VHCD_MAX_USB30_PORTS\s+7' -or
    $ucxText -notmatch '#define\s+VHCD_USB30_FIRST_PORT_NUMBER\s+\(VHCD_MAX_PORTS\s*\+\s*1\)' -or
    $ucxText -notmatch 'C_ASSERT\(VHCD_USB30_PORT_BITMAP_BYTES\s*<\s*VHCD_USB20_PORT_BITMAP_BYTES\)' -or
    $ucxText -notmatch 'C_ASSERT\(VHCD_COMBINED_PORT_BITMAP_BYTES\s*==\s*VHCD_USB20_PORT_BITMAP_BYTES\)' -or
    $ucxText -notmatch 'DeviceInfo->DeviceSpeed\s*==\s*UsbSuperSpeed' -or
    $ucxText -notmatch 'portNumber\s*>=\s*VHCD_USB30_FIRST_PORT_NUMBER' -or
    $ucxText -notmatch 'VhcdUsb30SlotFromPort\s*\([^)]*\)' -or
    $ucxText -notmatch 'superSpeedBitmap\s*\|=\s*1u\s*<<\s*\(ssPort\s*\+\s*1\)' -or
    $ucxText -notmatch 'port->PortNumber\s*=\s*VHCD_USB30_FIRST_PORT_NUMBER\s*\+\s*i' -or
    $ucxText -notmatch 'bitmap\s*\|=\s*1u\s*<<\s*\(VHCD_USB30_FIRST_PORT_NUMBER\s*\+\s*ssPort\)') {
    throw 'SuperSpeed slot ranges, root-hub port numbering, and interrupt bitmaps must support distinct USB 2 and USB 3 ports.'
}
if ($ucxText -notmatch 'case\s+UsbdPipeTypeBulk\s*:' -or
    $ucxText -notmatch 'case\s+UsbdPipeTypeInterrupt\s*:' -or
    $ucxText -notmatch 'Companion->bmAttributes\.Bulk\.Reserved1\s*!=\s*0' -or
    $ucxText -notmatch 'Companion->wBytesPerInterval\s*!=\s*0') {
    throw 'SuperSpeed endpoint admission must validate bulk and interrupt companion descriptors as well as isochronous limits.'
}

# Import only pure preflight/wrapper functions. Never execute the elevated
# script bodies, and replace native/CIM/PnP calls made by those functions.
foreach ($functionName in @(
    'Invoke-PnpUtil', 'Assert-NoImageVersionCollision', 'Assert-InstalledImage',
    'Assert-TreePackageCandidates', 'Assert-UcxPackageCandidates',
    'Remove-ObsoleteTreeDriverPackages'
)) {
    $definition = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
    }, $true)
    if (-not $definition) { throw "Missing deployment function: $functionName" }
    Invoke-Expression $definition.Extent.Text
}
foreach ($functionName in @('Get-NormalizedServiceImagePath', 'Assert-DeviceUsesService')) {
    $definition = $defaultAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
    }, $true)
    if (-not $definition) { throw "Missing default deployment function: $functionName" }
    Invoke-Expression $definition.Extent.Text
}

$pnputil = 'Invoke-MockPnpUtil'
$script:MockPnpCalls = @()
$script:MockBackups = @()
function Export-StaleDriverPackage {
    param([string] $PublishedInf, [string] $ExpectedInfName)
    $script:MockBackups += $PublishedInf
    return $true
}
function Invoke-MockPnpUtil {
    [CmdletBinding()]
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $Arguments)
    $script:MockPnpCalls += ,@($Arguments)
    if ($Arguments.Count -ge 3 -and $Arguments[0] -eq '/export-driver') {
        $destination = $Arguments[2]
        $script:MockExportDirectory = $destination
        if ($script:MockUcxPublishedInf -and $Arguments[1] -eq $script:MockUcxPublishedInf) {
            Copy-Item -LiteralPath $script:MockUcxExportInf -Destination (Join-Path $destination 'xusb_ucx_hcd.inf')
            Copy-Item -LiteralPath $script:MockUcxExportImage -Destination (Join-Path $destination ([IO.Path]::GetFileName($script:MockUcxExportImage)))
        } else {
            Copy-Item -LiteralPath $script:MockExportInf -Destination (Join-Path $destination 'xusb_reconstructed_tree.inf')
            Copy-Item -LiteralPath $script:MockExportImage -Destination (Join-Path $destination ([IO.Path]::GetFileName($script:MockExportImage)))
        }
        $global:LASTEXITCODE = 0
        return
    }
    $global:LASTEXITCODE = $script:MockNativeExit
}
function Get-WindowsDriver {
    [CmdletBinding()]
    param([switch] $Online, [switch] $All)
    return @($script:MockDriverPackages) + @($script:MockUcxDriverPackages)
}
foreach ($case in @(
    @{ Exit = 0; Allow = $false; Throws = $false },
    @{ Exit = 259; Allow = $true; Throws = $false },
    @{ Exit = 259; Allow = $false; Throws = $true },
    @{ Exit = 3010; Allow = $true; Throws = $true },
    @{ Exit = 5; Allow = $false; Throws = $true }
)) {
    $script:MockNativeExit = $case.Exit
    $didThrow = $false
    try { Invoke-PnpUtil @('/add-driver', 'mock.inf') -AllowNoUpdate:$case.Allow }
    catch {
        $didThrow = $true
        if ($case.Exit -eq 3010 -and $_.Exception.Message -notmatch 'stopped immediately') { throw }
    }
    if ($didThrow -ne $case.Throws) { throw "Unexpected handling of PnPUtil exit code $($case.Exit)." }
}

$output = Join-Path $repoRoot 'build\tests\driver-deploy'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$fixtureId = [guid]::NewGuid().ToString('N')
$existingImage = Join-Path $output "$fixtureId-existing.sys"
$candidateImage = Join-Path $output "$fixtureId-candidate.sys"
$script:MockService = [pscustomobject]@{ PathName = $existingImage; State = 'Running' }
function Get-CimInstance {
    [CmdletBinding()]
    param([string] $ClassName, [string] $Filter)
    return $script:MockService
}
function Get-Service { return $script:MockRuntimeService }
try {
    [IO.File]::WriteAllText($existingImage, 'old bytes')
    [IO.File]::WriteAllText($candidateImage, 'new bytes')
    # Reproduce the test machine's stale CIM state: WMI says Stopped while SCM
    # correctly reports the versioned kernel driver as Running.
    $script:MockService.State = 'Stopped'
    $script:MockRuntimeService = [pscustomobject]@{ Status = 'Running' }
    Assert-InstalledImage 'mock-service' $existingImage

    $script:MockRuntimeService.Status = 'Stopped'
    $didThrow = $false
    try { Assert-InstalledImage 'mock-service' $existingImage }
    catch { $didThrow = $true }
    if (-not $didThrow) { throw 'A driver service stopped in SCM passed the running-image check.' }

    $script:MockService.State = 'Running'
    $didThrow = $false
    try { Assert-NoImageVersionCollision 'mock-service' $candidateImage }
    catch {
        $didThrow = $true
        if ($_.Exception.Message -notmatch 'No device change') { throw }
    }
    if (-not $didThrow) { throw 'A same-version image collision was accepted.' }

    $storePackageDirectory = Join-Path $output "$fixtureId-store"
    New-Item -ItemType Directory -Path $storePackageDirectory -Force | Out-Null
    $script:MockExportImage = Join-Path $storePackageDirectory ([IO.Path]::GetFileName($candidateImage))
    $script:MockExportInf = Join-Path $storePackageDirectory 'xusb_reconstructed_tree.inf'
    Copy-Item -LiteralPath $candidateImage -Destination $script:MockExportImage -Force
    $treeService = 'xusb_tree_1_0_0_13'
    $treeImageName = [IO.Path]::GetFileName($candidateImage)
    $treeInfText = @"
[Version]
DriverVer = 11/16/2026,1.0.0.13
[Models.NTamd64.10.0...19041]
%UsbTreeDesc% = UsbTree_Install, root\*xusb_tree
[UsbTree_Install.NT.Services]
AddService = $treeService,0x00000002,UsbTree_Service
[UsbTree_Service]
ServiceBinary = %13%\$treeImageName
"@
    [IO.File]::WriteAllText($script:MockExportInf, $treeInfText)

    $script:MockDriverPackages = @(
        [pscustomobject]@{ OriginalFileName = 'C:\old\xusb_reconstructed_tree.inf'; ProviderName = 'Virtual Device Camera reconstructed driver'; Version = [version]'1.0.0.12'; Driver = 'oem12.inf' },
        [pscustomobject]@{ OriginalFileName = 'C:\same\xusb_reconstructed_tree.inf'; ProviderName = 'Virtual Device Camera reconstructed driver'; Version = [version]'1.0.0.13'; Driver = 'oem13.inf' }
    )
    Assert-TreePackageCandidates -RequestedVersion ([version]'1.0.0.13') `
        -RequestedService $treeService -ExpectedImage $candidateImage
    if (Test-Path -LiteralPath $script:MockExportDirectory) { throw 'The exported role-3 preflight directory was not removed.' }
    [IO.File]::WriteAllText($script:MockExportImage, 'stale same-version bytes')
    $didThrow = $false
    try {
        Assert-TreePackageCandidates -RequestedVersion ([version]'1.0.0.13') `
            -RequestedService $treeService -ExpectedImage $candidateImage
    }
    catch {
        $didThrow = $true
        if ($_.Exception.Message -notmatch 'same-version role-3 package contains different') { throw }
    }
    if (-not $didThrow) { throw 'A same-version DriverStore image collision passed the preflight.' }
    Assert-TreePackageCandidates -RequestedVersion ([version]'1.0.0.14') `
        -RequestedService $treeService -ExpectedImage $candidateImage
    Assert-TreePackageCandidates -RequestedVersion ([version]'1.0.0.16') `
        -RequestedService $treeService -ExpectedImage $candidateImage
    Copy-Item -LiteralPath $candidateImage -Destination $script:MockExportImage -Force
    $script:MockDriverPackages += [pscustomobject]@{
        OriginalFileName = 'xusb_reconstructed_tree.inf'
        ProviderName = 'Virtual Device Camera reconstructed driver'
        Version = [version]'1.26.7740.1519'
        Driver = 'oem14.inf'
    }
    $didThrow = $false
    try {
        Assert-TreePackageCandidates -RequestedVersion ([version]'1.0.0.13') `
            -RequestedService $treeService -ExpectedImage $candidateImage
    }
    catch {
        $didThrow = $true
        if ($_.Exception.Message -notmatch 'before changing any device') { throw }
    }
    if (-not $didThrow) { throw 'A higher-ranked role-3 package passed the preflight.' }

    $script:MockUcxPublishedInf = 'oem40.inf'
    $script:MockUcxExportImage = Join-Path $storePackageDirectory 'VirtualCameraUcxHcd_2_0_0_40.sys'
    $script:MockUcxExportInf = Join-Path $storePackageDirectory 'xusb_ucx_hcd.inf'
    Copy-Item -LiteralPath $candidateImage -Destination $script:MockUcxExportImage -Force
    $ucxService = 'ucx_hcd_2_0_0_40'
    $ucxInfText = @"
[Version]
DriverVer = 11/20/2026,2.0.0.40
[Models.NTamd64.10.0...19041]
%UcxDesc% = Ucx_Install, PCI\VEN_37FA&DEV_FE09
[Ucx_Install.NT.Services]
AddService = $ucxService,0x00000002,Ucx_Service
[Ucx_Service]
ServiceBinary = %13%\VirtualCameraUcxHcd_2_0_0_40.sys
"@
    [IO.File]::WriteAllText($script:MockUcxExportInf, $ucxInfText)
    $script:MockUcxDriverPackages = @(
        [pscustomobject]@{ OriginalFileName = 'C:\store\xusb_ucx_hcd.inf'; ProviderName = 'Virtual Device Camera Project'; Version = [version]'2.0.0.40'; Driver = $script:MockUcxPublishedInf }
    )
    Assert-UcxPackageCandidates -RequestedVersion ([version]'2.0.0.40') `
        -RequestedService $ucxService -ExpectedImage $candidateImage
    if (Test-Path -LiteralPath $script:MockExportDirectory) { throw 'The exported UCX preflight directory was not removed.' }

    [IO.File]::WriteAllText($script:MockUcxExportImage, 'stale same-version bytes')
    $didThrow = $false
    try {
        Assert-UcxPackageCandidates -RequestedVersion ([version]'2.0.0.40') `
            -RequestedService $ucxService -ExpectedImage $candidateImage
    }
    catch {
        $didThrow = $true
        if ($_.Exception.Message -notmatch 'same-version UCX package contains different') { throw }
    }
    if (-not $didThrow) { throw 'A same-version staged HCD image collision passed the preflight.' }

    Copy-Item -LiteralPath $candidateImage -Destination $script:MockUcxExportImage -Force
    $script:MockUcxDriverPackages += [pscustomobject]@{
        OriginalFileName = 'xusb_ucx_hcd.inf'
        ProviderName = 'Virtual Device Camera Project'
        Version = [version]'2.0.0.42'
        Driver = 'oem42.inf'
    }
    $didThrow = $false
    try {
        Assert-UcxPackageCandidates -RequestedVersion ([version]'2.0.0.40') `
            -RequestedService $ucxService -ExpectedImage $candidateImage
    }
    catch {
        $didThrow = $true
        if ($_.Exception.Message -notmatch 'before changing any device') { throw }
    }
    if (-not $didThrow) { throw 'A higher-ranked UCX HCD package passed the preflight.' }

    Copy-Item -LiteralPath $existingImage -Destination $candidateImage -Force
    Assert-NoImageVersionCollision 'mock-service' $candidateImage
    $script:MockService = [pscustomobject]@{ PathName = $candidateImage; State = 'Running' }
    $script:MockDevice = [pscustomobject]@{ InstanceId = 'ROOT\TEST'; Status = 'OK' }
    $script:MockDeviceService = 'mock-service'
    function Get-PnpDevice {
        [CmdletBinding()]
        param([switch] $PresentOnly, [string] $InstanceId)
        $devices = if ($script:MockPresentDevices) { @($script:MockPresentDevices) } else { @($script:MockDevice) }
        if ($InstanceId) { return @($devices | Where-Object { $_.InstanceId -eq $InstanceId }) }
        return $devices
    }
    function Get-PnpDeviceProperty {
        [CmdletBinding()]
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' { return [pscustomobject]@{ Data = $script:MockDeviceService } }
            'DEVPKEY_Device_DriverInfPath' { return [pscustomobject]@{ Data = $script:MockDeviceInfMap[$InstanceId] } }
            'DEVPKEY_Device_DriverVersion' { return [pscustomobject]@{ Data = '1.0.0.18' } }
            default { return [pscustomobject]@{ Data = $null } }
        }
    }
    $script:MockRuntimeService.Status = 'Running'
    Assert-DeviceUsesService -InstancePrefix 'ROOT\TEST' -ExpectedService 'mock-service' `
        -ExpectedImageName ([IO.Path]::GetFileName($candidateImage)) `
        -ExpectedImageHash (Get-FileHash -LiteralPath $candidateImage -Algorithm SHA256).Hash

    $script:MockDeviceService = 'wrong-service'
    $didThrow = $false
    try {
        Assert-DeviceUsesService -InstancePrefix 'ROOT\TEST' -ExpectedService 'mock-service' `
            -ExpectedImageName ([IO.Path]::GetFileName($candidateImage)) `
            -ExpectedImageHash (Get-FileHash -LiteralPath $candidateImage -Algorithm SHA256).Hash
    }
    catch { $didThrow = $true }
    if (-not $didThrow) { throw 'A device bound to the wrong service passed the default deploy check.' }

    $script:MockDeviceService = 'mock-service'
    $script:MockService.PathName = $existingImage
    [IO.File]::WriteAllText($candidateImage, 'different package bytes')
    $didThrow = $false
    try {
        Assert-DeviceUsesService -InstancePrefix 'ROOT\TEST' -ExpectedService 'mock-service' `
            -ExpectedImageName ([IO.Path]::GetFileName($candidateImage)) `
            -ExpectedImageHash (Get-FileHash -LiteralPath $candidateImage -Algorithm SHA256).Hash
    }
    catch {
        $didThrow = $true
        if ($_.Exception.Message -notmatch 'does not match the requested package') { throw }
    }
    if (-not $didThrow) { throw 'A bound service using different image bytes passed the default deploy check.' }

    $script:MockTreeDevice = [pscustomobject]@{ InstanceId = 'ROOT\TREE'; Status = 'OK' }
    $script:MockPresentDevices = @(
        $script:MockTreeDevice,
        [pscustomobject]@{ InstanceId = 'ROOT\OTHER'; Status = 'OK' }
    )
    $script:MockDeviceInfMap = @{
        'ROOT\TREE' = 'oem18.inf'
        'ROOT\OTHER' = 'oem16.inf'
    }
    function Get-TargetDevices {
        return [pscustomobject]@{
            Tree = @($script:MockTreeDevice)
            Controller = @()
            PreviousControlProviders = @()
        }
    }
    $script:MockDriverPackages = @(
        [pscustomobject]@{ OriginalFileName = 'C:\store\xusb_reconstructed_tree.inf'; ProviderName = 'Virtual Device Camera reconstructed driver'; Version = [version]'1.0.0.18'; Driver = 'oem18.inf' },
        [pscustomobject]@{ OriginalFileName = 'C:\store\xusb_reconstructed_tree.inf'; ProviderName = 'Virtual Device Camera reconstructed driver'; Version = [version]'1.0.0.17'; Driver = 'oem17.inf' },
        [pscustomobject]@{ OriginalFileName = 'C:\store\xusb_reconstructed_tree.inf'; ProviderName = 'Virtual Device Camera reconstructed driver'; Version = [version]'1.0.0.16'; Driver = 'oem16.inf' },
        [pscustomobject]@{ OriginalFileName = 'C:\store\xusb_reconstructed_tree.inf'; ProviderName = 'Virtual Device Camera reconstructed driver'; Version = [version]'1.0.0.19'; Driver = 'oem19.inf' },
        [pscustomobject]@{ OriginalFileName = 'C:\store\xusb_reconstructed_tree.inf'; ProviderName = 'Original Vendor'; Version = [version]'1.0.0.1'; Driver = 'oem1.inf' }
    )
    $script:MockPnpCalls = @()
    $script:MockBackups = @()
    $script:MockNativeExit = 0
    Remove-ObsoleteTreeDriverPackages
    $deletedPackages = @($script:MockPnpCalls | Where-Object { $_.Count -ge 2 -and $_[0] -eq '/delete-driver' })
    if ($deletedPackages.Count -ne 1 -or $deletedPackages[0][1] -ne 'oem17.inf' -or
        $deletedPackages[0] -contains '/force' -or $deletedPackages[0] -contains '/uninstall' -or
        $script:MockBackups.Count -ne 1 -or $script:MockBackups[0] -ne 'oem17.inf') {
        throw 'Tree package cleanup must back up and delete only older, unbound project packages without force/uninstall.'
    }

    $script:MockService = $null
    Assert-NoImageVersionCollision 'new-service' $candidateImage
}
finally {
    Remove-Item -LiteralPath $existingImage, $candidateImage -Force -ErrorAction SilentlyContinue
    if ($storePackageDirectory) { Remove-Item -LiteralPath $storePackageDirectory -Recurse -Force -ErrorAction SilentlyContinue }
    # Do not leak a simulated native failure to callers running the test suite.
    $global:LASTEXITCODE = 0
}
Write-Output 'PASS: deployment reboot/no-update handling, role-3 rank and stale-package cleanup, bound-service/image hash verification and image-version collision checks (native, SCM, CIM and PnP operations mocked).'
