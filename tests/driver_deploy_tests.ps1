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
    'Assert-TreePackageCandidates'
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
function Invoke-MockPnpUtil {
    [CmdletBinding()]
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $Arguments)
    if ($Arguments.Count -ge 3 -and $Arguments[0] -eq '/export-driver') {
        $destination = $Arguments[2]
        $script:MockExportDirectory = $destination
        Copy-Item -LiteralPath $script:MockExportInf -Destination (Join-Path $destination 'xusb_reconstructed_tree.inf')
        Copy-Item -LiteralPath $script:MockExportImage -Destination (Join-Path $destination ([IO.Path]::GetFileName($script:MockExportImage)))
        $global:LASTEXITCODE = 0
        return
    }
    $global:LASTEXITCODE = $script:MockNativeExit
}
function Get-WindowsDriver {
    [CmdletBinding()]
    param([switch] $Online, [switch] $All)
    return $script:MockDriverPackages
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

    Copy-Item -LiteralPath $existingImage -Destination $candidateImage -Force
    Assert-NoImageVersionCollision 'mock-service' $candidateImage
    $script:MockService = [pscustomobject]@{ PathName = $candidateImage; State = 'Running' }
    $script:MockDevice = [pscustomobject]@{ InstanceId = 'ROOT\TEST'; Status = 'OK' }
    $script:MockDeviceService = 'mock-service'
    function Get-PnpDevice {
        [CmdletBinding()]
        param([switch] $PresentOnly, [string] $InstanceId)
        return $script:MockDevice
    }
    function Get-PnpDeviceProperty {
        [CmdletBinding()]
        param([string] $InstanceId, [string] $KeyName)
        return [pscustomobject]@{ Data = $script:MockDeviceService }
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

    $script:MockService = $null
    Assert-NoImageVersionCollision 'new-service' $candidateImage
}
finally {
    Remove-Item -LiteralPath $existingImage, $candidateImage -Force -ErrorAction SilentlyContinue
    if ($storePackageDirectory) { Remove-Item -LiteralPath $storePackageDirectory -Recurse -Force -ErrorAction SilentlyContinue }
    # Do not leak a simulated native failure to callers running the test suite.
    $global:LASTEXITCODE = 0
}
Write-Output 'PASS: deployment reboot/no-update handling, role-3 rank preflight, bound-service/image hash verification and image-version collision checks (native, SCM, CIM and PnP operations mocked).'
