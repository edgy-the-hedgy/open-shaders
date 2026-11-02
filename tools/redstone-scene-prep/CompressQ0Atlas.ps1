param(
    [Parameter(Mandatory)][string]$AtlasRoot,
    [ValidateSet('XPRESS4K', 'XPRESS8K', 'XPRESS16K', 'LZX')][string]$Algorithm = 'XPRESS8K',
    [switch]$IncludeSourceCache
)

# Compress existing immutable files through NTFS. Their logical bytes, SHA-256
# identities and the native RSGM loader remain unchanged. This is DISK ONLY;
# it does not quantize vertices or shrink BLAS allocations.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($AtlasRoot)
$q0 = if ((Split-Path $root -Leaf) -eq 'q0') { $root } else { Join-Path $root 'q0' }
$geometry = Join-Path $q0 'geometry'
if (!(Test-Path -LiteralPath $geometry -PathType Container)) {
    throw "Q0 geometry directory not found: $geometry"
}
$driveRoot = [IO.Path]::GetPathRoot($q0)
$drive = New-Object IO.DriveInfo -ArgumentList $driveRoot
if ($drive.DriveFormat -ne 'NTFS') {
    throw "XPRESS requires an NTFS volume; $driveRoot is $($drive.DriveFormat)."
}
if (Get-Process -Name 'SkyrimSE', 'SkyrimVR', 'skse64_loader' -ErrorAction SilentlyContinue) {
    throw 'Close Skyrim before changing atlas compression.'
}
$compressor = Join-Path $env:SystemRoot 'System32\compact.exe'
if (!(Test-Path -LiteralPath $compressor)) { throw "Windows compact.exe is not available." }

$targets = @(@{ Directory = $geometry; Filter = '*.rsgm' })
if ($IncludeSourceCache) {
    $cache = Join-Path $q0 'source-cache'
    if (Test-Path -LiteralPath $cache -PathType Container) {
        $targets += @{ Directory = $cache; Filter = '*.nif' }
    }
}
foreach ($target in $targets) {
    $folder = $target.Directory
    $filter = $target.Filter
    Write-Output "Compressing immutable Q0 assets: $folder ($filter), algorithm $Algorithm."
    & $compressor /C /Q "/EXE:$Algorithm" "/S:$folder" $filter
    if ($LASTEXITCODE -ne 0) { throw "NTFS compression failed (exit $LASTEXITCODE): $folder" }
}
Write-Output 'Q0 atlas compression complete. SHA-256 names and decompressed RSGM content are unchanged.'
