param(
    [Parameter(Mandatory)][string]$SourceDirectory,
    [string]$OutputRoot,
    [ValidateRange(0, 2)][int]$Quality = 2,
    [switch]$QualityDirectory,
    [switch]$UseAtlas
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$OutputRoot) { $OutputRoot = Join-Path $projectRoot 'build\scene-prep\runtime' }
$source = [IO.Path]::GetFullPath($SourceDirectory)
$manifest = Get-Content -LiteralPath (Join-Path $source 'cell-layout.json') -Raw | ConvertFrom-Json
if ($manifest.rendererSchemaVersion -ne 1) { throw 'Regenerate the cell with the current preparation tool first.' }
$parts = $manifest.cell.Split(':')
if ($parts.Count -ne 2 -or $parts[0] -notmatch '^[0-9A-Fa-f]{6}$' -or $parts[1] -match '[\\/:]' -or $parts[1] -notmatch '\.es[mp]$') {
    throw 'Invalid prepared cell identity.'
}
$destination = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('SKSE\Plugins\CommunityShaders\RedstoneScene\cells\' + $parts[1] + '\' + $parts[0])
if ($QualityDirectory) { $destination = Join-Path $destination "q$Quality" }
$atlas = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ("SKSE\\Plugins\\CommunityShaders\\RedstoneScene\\atlas\\q$Quality")
$geometryDirectory = Join-Path $destination 'geometry'
$null = New-Item -ItemType Directory -Path $geometryDirectory -Force
foreach ($mesh in ($manifest.instances | Sort-Object geometrySha256 -Unique)) {
    $hash = $mesh.geometrySha256
    if ($hash -notmatch '^[0-9a-f]{64}$' -or $mesh.geometryFile -cne "geometry/$hash.rsgm") { throw 'Invalid prepared geometry path.' }
    $sourceFile = Join-Path $source $mesh.geometryFile
    if ((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw "Prepared geometry hash mismatch: $hash" }
    $atlasGeometry = Join-Path (Join-Path $atlas 'geometry') "$hash.rsgm"
    if ($UseAtlas -and (Test-Path -LiteralPath $atlasGeometry)) {
        if ((Get-FileHash -LiteralPath $atlasGeometry -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw "Atlas geometry invalid: $hash" }
    } else { Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $geometryDirectory "$hash.rsgm") -Force }
}
foreach ($attributes in ($manifest.instances | Where-Object { $_.shadingStatus -eq 'compiled' } | Sort-Object shadingSha256 -Unique)) {
    $hash = $attributes.shadingSha256
    if ($hash -notmatch '^[0-9a-f]{64}$' -or $attributes.shadingFile -cne "shading/$hash.rsga") { throw 'Invalid prepared shading path.' }
    $sourceFile = Join-Path $source $attributes.shadingFile
    if ((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw "Prepared shading hash mismatch: $hash" }
    $shadingDirectory = Join-Path $destination 'shading'
    $null = New-Item -ItemType Directory -Path $shadingDirectory -Force
    $atlasShading = Join-Path (Join-Path $atlas 'shading') "$hash.rsga"
    if ($UseAtlas -and (Test-Path -LiteralPath $atlasShading)) {
        if ((Get-FileHash -LiteralPath $atlasShading -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw "Atlas shading invalid: $hash" }
    } else { Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $shadingDirectory "$hash.rsga") -Force }
}
$textureManifest = Join-Path $source 'cell-textures.json'
if (Test-Path -LiteralPath $textureManifest) {
    $textures = Get-Content -LiteralPath $textureManifest -Raw | ConvertFrom-Json
    if ($textures.schemaVersion -ne 1 -or $textures.cell -cne $manifest.cell -or $textures.layoutDigest -cne $manifest.layoutDigest) { throw 'Texture source manifest does not match prepared layout.' }
    $textureDirectory = Join-Path $destination 'textures'
    $null = New-Item -ItemType Directory -Path $textureDirectory -Force
    foreach ($texture in ($textures.textures | Where-Object status -eq 'cached' | Sort-Object sha256 -Unique)) {
        $hash = $texture.sha256
        if ($hash -notmatch '^[0-9a-f]{64}$' -or $texture.file -cne "textures/$hash.dds") { throw 'Invalid prepared texture path.' }
        $sourceFile = Join-Path $source $texture.file
        if ((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw "Prepared texture hash mismatch: $hash" }
        $atlasTexture = Join-Path (Join-Path $atlas 'textures') "$hash.dds"
        if ($UseAtlas -and (Test-Path -LiteralPath $atlasTexture)) {
            if ((Get-FileHash -LiteralPath $atlasTexture -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw "Atlas DDS invalid: $hash" }
        } else { Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $textureDirectory "$hash.dds") -Force }
    }
    Copy-Item -LiteralPath $textureManifest -Destination (Join-Path $destination 'cell-textures.json') -Force
}
Copy-Item -LiteralPath (Join-Path $source 'cell-layout.json') -Destination (Join-Path $destination 'cell-layout.json') -Force
Write-Output "Staged prepared cell: $destination"
Write-Output 'No game deployment or plugin build was performed.'
