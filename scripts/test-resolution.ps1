[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$FFmpegRoot
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (!$FFmpegRoot) { $FFmpegRoot = Join-Path $repoRoot '.deps\ffmpeg' }
$FFmpegRoot = (Resolve-Path -LiteralPath $FFmpegRoot).Path
$ffmpeg = Join-Path $FFmpegRoot 'bin\ffmpeg.exe'
if (!(Test-Path -LiteralPath $ffmpeg)) { throw 'The FFmpeg SDK must include bin/ffmpeg.exe to create test fixtures.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'Visual Studio C++ toolchain not found.' }
if (!$env:VSCMD_VER -or $env:VSCMD_ARG_TGT_ARCH -notmatch '^(amd64|x64)$') {
    & (Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation -NoLogo
}

$output = Join-Path $repoRoot "build\tests\resolution\$Configuration"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$testExe = Join-Path $output 'resolution-tests.exe'
$compilerArgs = @('/nologo', '/std:c++17', '/EHsc', '/W4', '/utf-8', '/external:W0',
    ("/external:I$FFmpegRoot\include"), ("/Fo$output\"), ("/Fe$testExe"),
    (Join-Path $repoRoot 'tests\frame_source_resolution_tests.cpp'),
    (Join-Path $repoRoot 'src\media\frame_source.cpp'),
    (Join-Path $repoRoot 'src\media\obs_capture.cpp'),
    (Join-Path $repoRoot 'src\media\pipe_reader.cpp'))
if ($Configuration -eq 'Debug') { $compilerArgs += @('/MTd', '/Od', '/Zi', ("/Fd$output\resolution-tests.pdb")) }
else { $compilerArgs += @('/MT', '/O2') }
$compilerArgs += @('/link', ("/LIBPATH:$FFmpegRoot\lib"),
    'avformat.lib', 'avcodec.lib', 'swscale.lib', 'avutil.lib', 'gdi32.lib', 'advapi32.lib',
    'bcrypt.lib', 'crypt32.lib', 'winhttp.lib')
& cl.exe @compilerArgs
if ($LASTEXITCODE -ne 0) { throw "Resolution test build failed: $LASTEXITCODE" }

$obsTestExe = Join-Path $output 'obs-capture-protocol-tests.exe'
$obsObjectOutput = Join-Path $output 'obs-protocol'
New-Item -ItemType Directory -Path $obsObjectOutput -Force | Out-Null
$obsCompilerArgs = @('/nologo', '/std:c++17', '/EHsc', '/W4', '/utf-8', '/external:W0',
    ("/external:I$FFmpegRoot\include"), ("/Fo$obsObjectOutput\"), ("/Fe$obsTestExe"),
    (Join-Path $repoRoot 'tests\obs_capture_protocol_tests.cpp'),
    (Join-Path $repoRoot 'src\media\obs_capture.cpp'))
if ($Configuration -eq 'Debug') { $obsCompilerArgs += @('/MTd', '/Od', '/Zi', ("/Fd$output\obs-capture-protocol-tests.pdb")) }
else { $obsCompilerArgs += @('/MT', '/O2') }
$obsCompilerArgs += @('/link', ("/LIBPATH:$FFmpegRoot\lib"),
    'avcodec.lib', 'swscale.lib', 'avutil.lib', 'bcrypt.lib', 'crypt32.lib', 'winhttp.lib', 'ws2_32.lib')
& cl.exe @obsCompilerArgs
if ($LASTEXITCODE -ne 0) { throw "OBS capture protocol test build failed: $LASTEXITCODE" }

$fixtures = @()
$segments = @()
$sizes = @('640x480', '1280x720', '1920x1080')
$colors = @('0x00ff00', 'red', 'blue')
for ($i = 0; $i -lt $sizes.Count; ++$i) {
    $fixture = Join-Path $output "$($sizes[$i]).mp4"
    & $ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=0x00ff00:s=$($sizes[$i]):r=30" -frames:v 3 -c:v mpeg4 -q:v 2 $fixture
    if ($LASTEXITCODE -ne 0) { throw 'Unable to generate an MP4 fixture.' }
    $fixtures += $fixture
    $segment = Join-Path $output "$($sizes[$i]).h264"
    & $ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=$($colors[$i]):s=$($sizes[$i]):r=30" -frames:v 3 -c:v libx264 -preset ultrafast -tune zerolatency -x264-params keyint=1 -f h264 $segment
    if ($LASTEXITCODE -ne 0) { throw 'Unable to generate a variable-resolution fixture segment.' }
    $segments += $segment
}
$dynamicFixture = Join-Path $output 'dynamic.h264'
$stream = [System.IO.File]::Create($dynamicFixture)
try {
    foreach ($segment in $segments) {
        $bytes = [System.IO.File]::ReadAllBytes($segment)
        $stream.Write($bytes, 0, $bytes.Length)
    }
}
finally { $stream.Dispose() }
$mjpegFixture = Join-Path $output 'encoded-720p-mjpeg.jpg'

$originalPath = $env:PATH
try {
    $env:PATH = (Join-Path $FFmpegRoot 'bin') + ';' + $env:PATH
    & $testExe @fixtures $dynamicFixture $mjpegFixture
    if ($LASTEXITCODE -ne 0) { throw "Resolution regression tests failed: $LASTEXITCODE" }
    & $obsTestExe $mjpegFixture
    if ($LASTEXITCODE -ne 0) { throw "OBS capture protocol tests failed: $LASTEXITCODE" }
    & $ffmpeg -hide_banner -loglevel error -y -i $mjpegFixture -frames:v 1 -f null -
    if ($LASTEXITCODE -ne 0) { throw 'FFmpeg could not decode the generated 720p MJPEG frame.' }
}
finally { $env:PATH = $originalPath }
