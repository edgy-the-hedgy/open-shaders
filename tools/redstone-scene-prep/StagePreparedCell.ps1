param(
    [Parameter(Mandatory)][string]$SourceDirectory,
    [string]$OutputRoot
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
$geometryDirectory = Join-Path $destination 'geometry'
$null = New-Item -ItemType Directory -Path $geometryDirectory -Force
foreach ($mesh in ($manifest.instances | Sort-Object geometrySha256 -Unique)) {
    $hash = $mesh.geometrySha256
    if ($hash -notmatch '^[0-9a-f]{64}$' -or $mesh.geometryFile -cne "geometry/$hash.rsgm") { throw 'Invalid prepared geometry path.' }
    $sourceFile = Join-Path $source $mesh.geometryFile
    if ((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw "Prepared geometry hash mismatch: $hash" }
    Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $geometryDirectory "$hash.rsgm") -Force
}
Copy-Item -LiteralPath (Join-Path $source 'cell-layout.json') -Destination (Join-Path $destination 'cell-layout.json') -Force
Write-Output "Staged prepared cell: $destination"
Write-Output 'No game deployment or plugin build was performed.'
