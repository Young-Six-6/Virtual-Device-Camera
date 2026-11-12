[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string[]]$Configurations = @('Debug','Release'),
    [string]$FFmpegRoot
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$python = Join-Path $repoRoot '.venv\Scripts\python.exe'
if (!(Test-Path -LiteralPath $python)) { throw 'Run scripts/setup.ps1 to prepare the Python environment.' }
& $python -m pip check
if ($LASTEXITCODE -ne 0) { throw 'Python dependencies are inconsistent.' }
$previousFixture = $env:VCAM_TEST_CONTROL_EXE
Push-Location $repoRoot
try {
    foreach ($configuration in $Configurations) {
        $sdkArgs = @{}
        if ($FFmpegRoot) { $sdkArgs.FFmpegRoot = $FFmpegRoot }
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $configuration @sdkArgs
        & (Join-Path $PSScriptRoot 'test-resolution.ps1') -Configuration $configuration @sdkArgs
        & (Join-Path $PSScriptRoot 'test-usb.ps1') -Configuration $configuration
        $env:VCAM_TEST_CONTROL_EXE = Join-Path $repoRoot "build\tests\usb\$configuration\usb-tests.exe"
        & $python -m unittest discover -s tests -p test_tools.py -v
        if ($LASTEXITCODE -ne 0) { throw "Python/C++ integration tests failed for $configuration." }
        Write-Host "All $configuration checks passed."
    }
}
finally {
    $env:VCAM_TEST_CONTROL_EXE = $previousFixture
    Pop-Location
}
