[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^USB\\ROOT_HUB30\\')]
    [string]$RootHubInstanceId,

    [switch]$RestartRootHub,

    [ValidateRange(2, 60)]
    [int]$CaptureSeconds = 8
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$stamp = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $PID
$outputDir = Join-Path $repoRoot "build\diagnostics\ucx-roothub-$stamp"
$hubSession = "vcam_hub_$stamp"
$ucxSession = "vcam_ucx_$stamp"
$hubEtl = Join-Path $outputDir 'usbhub3.etl'
$ucxEtl = Join-Path $outputDir 'ucx.etl'
$transcript = Join-Path $outputDir 'capture.log'
$hubStarted = $false
$ucxStarted = $false
$runError = $null
$rootHub = $null
$parent = $null

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
Start-Transcript -LiteralPath $transcript -Force | Out-Null
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this diagnostic from an elevated PowerShell session.'
    }

    $rootHub = Get-PnpDevice -PresentOnly -InstanceId $RootHubInstanceId -ErrorAction Stop
    $parent = (Get-PnpDeviceProperty -InstanceId $RootHubInstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data
    if ($parent -notlike 'PCI\VEN_37FA&DEV_FE09*') {
        throw "The selected root hub is not a child of this project's virtual HCD: $parent"
    }

    Write-Host "Root hub: $($rootHub.InstanceId) ($($rootHub.Status))"
    Write-Host "Virtual HCD parent: $parent"
    Write-Host "Output: $outputDir"
    Write-Host 'Capturing USBHUB3 and UCX ETW events; UCX includes IRP/dispatch keywords.'

    & logman.exe create trace $hubSession -o $hubEtl -p Microsoft-Windows-USB-USBHUB3 0x20613 5 -ets
    if ($LASTEXITCODE -ne 0) { throw "Could not start USBHUB3 trace (exit $LASTEXITCODE)." }
    $hubStarted = $true

    & logman.exe create trace $ucxSession -o $ucxEtl -p Microsoft-Windows-USB-UCX 0xA0617 5 -ets
    if ($LASTEXITCODE -ne 0) { throw "Could not start UCX trace (exit $LASTEXITCODE)." }
    $ucxStarted = $true

    if ($RestartRootHub) {
        if ($PSCmdlet.ShouldProcess($RootHubInstanceId, 'Restart only this virtual root-hub PnP device')) {
            & pnputil.exe /restart-device $RootHubInstanceId
            if ($LASTEXITCODE -ne 0) { throw "PnPUtil could not restart the selected root hub (exit $LASTEXITCODE)." }
        }
        else {
            Write-Host 'PnP restart skipped by ShouldProcess; this run will capture passively.'
        }
    }
    else {
        Write-Host 'Passive capture only. Pass -RestartRootHub to capture one root-hub start sequence.'
    }

    Start-Sleep -Seconds $CaptureSeconds
}
catch {
    $runError = $_
    Write-Host "CaptureError=$($_.Exception.Message)"
}
finally {
    if ($ucxStarted) { & logman.exe stop $ucxSession -ets }
    if ($hubStarted) { & logman.exe stop $hubSession -ets }
    Stop-Transcript | Out-Null
}

if (Test-Path -LiteralPath $hubEtl) {
    & tracerpt.exe $hubEtl -o (Join-Path $outputDir 'usbhub3.xml') -of XML -y -lr
}
if (Test-Path -LiteralPath $ucxEtl) {
    & tracerpt.exe $ucxEtl -o (Join-Path $outputDir 'ucx.xml') -of XML -y -lr
}

if ($null -eq $runError) {
    Get-PnpDevice -PresentOnly -InstanceId $RootHubInstanceId -ErrorAction SilentlyContinue |
        Select-Object Status, Class, FriendlyName, InstanceId | Format-List
    try {
        & (Join-Path $PSScriptRoot 'query-ucx-state.ps1')
    }
    catch {
        Write-Warning "Could not query HCD state: $($_.Exception.Message)"
    }
}

Write-Host "Trace artifacts: $outputDir"
if ($null -ne $runError) { throw $runError }
