[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$deployScript = Join-Path $repoRoot 'drivers\usb-bus-legacy\replacement-package\replacement-deploy.ps1'
$parseTokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($deployScript, [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }

# Import only the two pure preflight/wrapper functions. Never execute the
# elevated script body, and replace every native/CIM call these functions use.
foreach ($functionName in @('Invoke-PnpUtil', 'Assert-NoImageVersionCollision', 'Assert-InstalledImage')) {
    $definition = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
    }, $true)
    if (-not $definition) { throw "Missing deployment function: $functionName" }
    Invoke-Expression $definition.Extent.Text
}

$pnputil = 'Invoke-MockPnpUtil'
function Invoke-MockPnpUtil { $global:LASTEXITCODE = $script:MockNativeExit }
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
function Get-CimInstance { return $script:MockService }
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

    Copy-Item -LiteralPath $existingImage -Destination $candidateImage -Force
    Assert-NoImageVersionCollision 'mock-service' $candidateImage
    $script:MockService = $null
    Assert-NoImageVersionCollision 'new-service' $candidateImage
}
finally {
    Remove-Item -LiteralPath $existingImage, $candidateImage -Force -ErrorAction SilentlyContinue
    # Do not leak a simulated native failure to callers running the test suite.
    $global:LASTEXITCODE = 0
}
Write-Output 'PASS: deployment reboot/no-update handling, SCM runtime-state verification and image-version collision preflight (native, SCM and CIM operations mocked).'
