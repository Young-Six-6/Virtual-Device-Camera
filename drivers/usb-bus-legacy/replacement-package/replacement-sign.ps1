[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string] $CertificateThumbprint,
    [string] $PackagePath = (Join-Path $PSScriptRoot 'build\x64\Release'),
    [switch] $EmbedImages,
    [string] $TimestampServer
)

$ErrorActionPreference = 'Stop'
$package = [System.IO.Path]::GetFullPath($PackagePath)
$certificateThumbprint = $CertificateThumbprint.Replace(' ', '').ToUpperInvariant()
$kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$signTool = Get-ChildItem -LiteralPath $kitsBin -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object {
        $candidate = Join-Path $_.FullName 'x64\signtool.exe'
        if (Test-Path -LiteralPath $candidate) { $candidate; break }
    } |
    Select-Object -First 1
if (-not $signTool) { throw 'The x64 SignTool utility was not found in the installed Windows Kits.' }

$inf2cat = $null
if ($EmbedImages) {
    $inf2cat = Get-ChildItem -LiteralPath $kitsBin -Directory -ErrorAction Stop |
        Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object {
            foreach ($architecture in @('x64', 'x86')) {
                $candidate = Join-Path $_.FullName "$architecture\Inf2Cat.exe"
                if (Test-Path -LiteralPath $candidate) { $candidate; break }
            }
        } |
        Select-Object -First 1
    if (-not $inf2cat) { throw 'Inf2Cat was not found in the installed Windows Kits (searched x64 and x86 tool directories).' }
}

$certificate = $null
$storeArguments = @()
foreach ($store in @(
    @{ Path = 'Cert:\CurrentUser\My'; Machine = $false },
    @{ Path = 'Cert:\LocalMachine\My'; Machine = $true }
)) {
    $certificate = Get-ChildItem -LiteralPath $store.Path -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $certificateThumbprint } |
        Select-Object -First 1
    if ($certificate) {
        if ($store.Machine) { $storeArguments = @('/sm') }
        break
    }
}
if (-not $certificate) { throw "Signing certificate $certificateThumbprint was not found in CurrentUser\My or LocalMachine\My." }
if (-not $certificate.HasPrivateKey) { throw "Signing certificate $certificateThumbprint has no accessible private key." }
$codeSigningEku = '1.3.6.1.5.5.7.3.3'
if ($certificate.EnhancedKeyUsageList -and
    -not ($certificate.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq $codeSigningEku })) {
    throw "Signing certificate $certificateThumbprint does not include the Code Signing EKU."
}

$catalogs = @(
    @{ Catalog = 'xusb_reconstructed_tree.cat'; Image = 'xusb_driver.sys'; Inf = 'xusb_reconstructed_tree.inf' },
    @{ Catalog = 'xusb_ucx_hcd.cat'; Image = $null; Inf = 'xusb_ucx_hcd.inf' }
)
$ucxInfPath = Join-Path $package 'xusb_ucx_hcd.inf'
if (-not (Test-Path -LiteralPath $ucxInfPath -PathType Leaf)) {
    throw "Required package INF is missing: $ucxInfPath"
}
$ucxInfText = [System.IO.File]::ReadAllText($ucxInfPath)
$ucxImageMatch = [regex]::Match($ucxInfText, '(?im)^\s*ServiceBinary\s*=\s*%13%\\([^\\\r\n]+)\s*$')
$ucxCatalogMatch = [regex]::Match($ucxInfText, '(?im)^\s*CatalogFile\.NTamd64\s*=\s*([^\s]+)\s*$')
if (-not $ucxImageMatch.Success -or -not $ucxCatalogMatch.Success) {
    throw "Could not parse the UCX ServiceBinary/catalog from $ucxInfPath"
}
$catalogs[1].Image = $ucxImageMatch.Groups[1].Value
$catalogs[1].Catalog = $ucxCatalogMatch.Groups[1].Value
$treeInfText = [IO.File]::ReadAllText((Join-Path $package 'xusb_reconstructed_tree.inf'))
$treeImageMatch = [regex]::Match($treeInfText, '(?im)^\s*ServiceBinary\s*=\s*%13%\\([^\\\r\n]+)\s*$')
if (-not $treeImageMatch.Success) { throw 'Could not parse the tree ServiceBinary.' }
$catalogs[0].Image = $treeImageMatch.Groups[1].Value

if ($EmbedImages) {
    foreach ($item in $catalogs) {
        $imagePath = Join-Path $package $item.Image
        $infPath = Join-Path $package $item.Inf
        foreach ($path in @($imagePath, $infPath)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required package file is missing: $path" }
        }

        $imageSignature = Get-AuthenticodeSignature -LiteralPath $imagePath
        if ($imageSignature.Status -eq 'Valid') {
            if ($imageSignature.SignerCertificate.Thumbprint -ne $certificateThumbprint) {
                throw "$($item.Image) is already embedded-signed by another certificate. Rebuild the package before signing it again."
            }
            Write-Output "Already embedded-signed by the requested certificate: $($item.Image)"
        } else {
            if ($imageSignature.Status -ne 'NotSigned') {
                throw "Image signature state is not clean for $imagePath ($($imageSignature.Status)). Rebuild the package before signing it."
            }
            $signArguments = @('sign', '/fd', 'SHA256', '/sha1', $certificateThumbprint) + $storeArguments
            if ($TimestampServer) { $signArguments += @('/tr', $TimestampServer, '/td', 'SHA256') }
            $signArguments += $imagePath
            & $signTool @signArguments
            if ($LASTEXITCODE -ne 0) { throw "SignTool failed to embed-sign $($item.Image) (exit code $LASTEXITCODE)." }
        }
        & $signTool verify /pa /v $imagePath
        if ($LASTEXITCODE -ne 0) { throw "Embedded signature verification failed for $($item.Image)." }
    }

    & $inf2cat "/driver:$package" /os:10_VB_X64
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed after embedded image signing (exit code $LASTEXITCODE)." }
}

foreach ($item in $catalogs) {
    $catalogPath = Join-Path $package $item.Catalog
    $imagePath = Join-Path $package $item.Image
    foreach ($path in @($catalogPath, $imagePath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required package file is missing: $path" }
    }

    $existingSignature = Get-AuthenticodeSignature -LiteralPath $catalogPath
    if ($existingSignature.Status -eq 'Valid') {
        if ($existingSignature.SignerCertificate.Thumbprint -eq $certificateThumbprint) {
            Write-Output "Already signed by the requested certificate: $($item.Catalog)"
        } else {
            throw "$($item.Catalog) is already signed by another certificate. Rebuild the package before signing it again."
        }
    } else {
        if ($existingSignature.Status -ne 'NotSigned') {
            throw "Catalog signature state is not clean for $catalogPath ($($existingSignature.Status)). Rebuild the package before signing it."
        }
        $signArguments = @('sign', '/fd', 'SHA256', '/sha1', $certificateThumbprint) + $storeArguments
        if ($TimestampServer) { $signArguments += @('/tr', $TimestampServer, '/td', 'SHA256') }
        $signArguments += $catalogPath
        & $signTool @signArguments
        if ($LASTEXITCODE -ne 0) { throw "SignTool failed to sign $($item.Catalog) (exit code $LASTEXITCODE)." }
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $catalogPath
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $certificateThumbprint) {
        throw "Catalog signature did not validate with the requested certificate: $catalogPath ($($signature.Status))."
    }
    & $signTool verify /pa /v /c $catalogPath $imagePath
    if ($LASTEXITCODE -ne 0) { throw "The catalog does not verify the matching image: $($item.Image)." }
}

Write-Warning 'This verifies Authenticode catalog signatures and image hashes only. It does not prove that the active Windows kernel Code Integrity or boot policy will accept a test certificate.'
