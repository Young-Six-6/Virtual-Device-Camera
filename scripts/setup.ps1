[CmdletBinding()]
param(
    [string]$FFmpegSource,
    [string]$Python = 'python',
    [switch]$SkipPython
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$ffmpegRoot = Join-Path $repoRoot '.deps\ffmpeg'

function Assert-FFmpegSdk([string]$Path) {
    foreach ($relative in @(
        'include\libavformat\avformat.h', 'include\libavcodec\avcodec.h',
        'include\libswscale\swscale.h', 'include\libavutil\imgutils.h',
        'lib\avformat.lib', 'lib\avcodec.lib', 'lib\swscale.lib', 'lib\avutil.lib',
        'lib\swresample.lib'
    )) {
        if (!(Test-Path -LiteralPath (Join-Path $Path $relative))) {
            throw "Missing $relative in $Path. Use a complete win64 shared FFmpeg SDK."
        }
    }
    foreach ($library in @('avformat', 'avcodec', 'swscale', 'avutil', 'swresample')) {
        if (!(Get-ChildItem -LiteralPath (Join-Path $Path 'bin') -Filter "$library-*.dll" -ErrorAction SilentlyContinue)) {
            throw "Missing $library runtime DLL in $Path\bin."
        }
    }
}

if ($FFmpegSource) {
    $source = (Resolve-Path -LiteralPath $FFmpegSource).Path
    Assert-FFmpegSdk $source
    if ($source.TrimEnd('\') -ne $ffmpegRoot.TrimEnd('\')) {
        New-Item -ItemType Directory -Path $ffmpegRoot -Force | Out-Null
        foreach ($directory in @('include', 'lib', 'bin')) {
            Copy-Item -LiteralPath (Join-Path $source $directory) -Destination $ffmpegRoot -Recurse -Force
        }
        $license = Join-Path $source 'LICENSE.txt'
        if (Test-Path -LiteralPath $license) { Copy-Item -LiteralPath $license -Destination $ffmpegRoot -Force }
    }
}
Assert-FFmpegSdk $ffmpegRoot
Write-Host "FFmpeg SDK: $ffmpegRoot"

if (!$SkipPython) {
    & $Python -c "import sys, struct; assert sys.version_info[:2] == (3, 11) and struct.calcsize('P') == 8, 'Use Python 3.11 x64'"
    if ($LASTEXITCODE -ne 0) { throw 'Python 3.11 x64 is required.' }
    $venvPython = Join-Path $repoRoot '.venv\Scripts\python.exe'
    if (!(Test-Path -LiteralPath $venvPython)) {
        & $Python -m venv (Join-Path $repoRoot '.venv')
        if ($LASTEXITCODE -ne 0) { throw 'Failed to create Python virtual environment.' }
    }
    & $venvPython -m pip install -r (Join-Path $repoRoot 'tools\python\requirements.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Python dependency installation failed.' }
    & $venvPython -m pip check
    if ($LASTEXITCODE -ne 0) { throw 'Python dependency check failed.' }
    & $venvPython -c "import cv2, numpy, obswebsocket, win32pipe, win32file, pywintypes; from obswebsocket.exceptions import ConnectionFailure; print('Python dependency imports OK')"
    if ($LASTEXITCODE -ne 0) { throw 'Python dependency import failed.' }
}

& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Debug
Write-Host 'Setup complete. USB bus driver installation is a separate step.'
