[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'Visual Studio C++ toolchain not found.' }
if (!$env:VSCMD_VER -or $env:VSCMD_ARG_TGT_ARCH -notmatch '^(amd64|x64)$') {
    & (Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation -NoLogo
}
$output = Join-Path $repoRoot "build\tests\usb\$Configuration"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$arguments = @('/nologo','/std:c++17','/EHsc','/W3','/wd4200','/utf-8',("/Fo$output\"),("/Fe$output\usb-tests.exe"),
    (Join-Path $repoRoot 'tests\usb_protocol_tests.cpp'),
    (Join-Path $repoRoot 'src\camera\uvc_device.cpp'),
    (Join-Path $repoRoot 'src\camera\camera_session.cpp'),
    (Join-Path $repoRoot 'src\app\control_server.cpp'))
if ($Configuration -eq 'Debug') { $arguments += @('/MTd','/Od','/Zi',("/Fd$output\usb-tests.pdb")) }
else { $arguments += @('/MT','/O2') }
$arguments += @('/link', 'advapi32.lib')
& cl.exe @arguments
if ($LASTEXITCODE -ne 0) { throw 'USB test compilation failed.' }
& (Join-Path $output 'usb-tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'USB tests failed.' }
