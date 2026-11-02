param(
    [Parameter(Mandatory)][string]$AtlasSourceRoot,
    [string]$DataDirectory = 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
    [ValidateRange(0, 2)][int[]]$QualityLevels = @(0, 1, 2)
)

$ErrorActionPreference = 'Stop'
if (Get-Process -Name 'SkyrimSE', 'SkyrimVR', 'skse64_loader' -ErrorAction SilentlyContinue) {
    throw 'Close Skyrim before installing the universal asset atlas.'
}
$sourceRoot = [IO.Path]::GetFullPath($AtlasSourceRoot)
$targetRoot = Join-Path ([IO.Path]::GetFullPath($DataDirectory)) 'SKSE\Plugins\CommunityShaders\RedstoneScene\atlas'
foreach ($quality in ($QualityLevels | Select-Object -Unique)) {
    $source = Join-Path $sourceRoot "q$quality"
    $index = Join-Path $source 'atlas-index.json'
    if (!(Test-Path -LiteralPath $index)) { throw "Atlas manifest missing: $index" }
    $metadata = Get-Content -LiteralPath $index -Raw | ConvertFrom-Json
    if ($metadata.schemaVersion -ne 1 -or $metadata.quality -ne $quality -or
        $metadata.kind -ne 'native-scene-universal-asset-atlas') {
        throw "Invalid atlas index: q$quality"
    }
    $destination = Join-Path $targetRoot "q$quality"
    foreach ($folder in @('geometry', 'shading', 'textures')) {
        $sourceFolder = Join-Path $source $folder
        if (!(Test-Path -LiteralPath $sourceFolder)) { continue }
        $targetFolder = Join-Path $destination $folder
        $null = New-Item -ItemType Directory -Path $targetFolder -Force
        foreach ($asset in (Get-ChildItem -LiteralPath $sourceFolder -File)) {
            if ($asset.Name -cnotmatch '^([0-9a-f]{64})\.(rsgm|rsga|dds)$') { throw "Unexpected atlas asset: $($asset.Name)" }
            $expected = $Matches[1]
            if ((Get-FileHash -LiteralPath $asset.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected) {
                throw "Atlas source digest mismatch: $($asset.FullName)"
            }
            $target = Join-Path $targetFolder $asset.Name
            if ((Test-Path -LiteralPath $target) -and
                (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $expected) {
                continue
            }
            Copy-Item -LiteralPath $asset.FullName -Destination $target -Force
        }
    }
    # Compile metadata is required for cell preparation to reuse atlas work
    # without re-parsing NIFs. Each entry is cross-checked against its names.
    $compiledRoot = Join-Path $source 'compiled'
    if (Test-Path -LiteralPath $compiledRoot) {
        foreach ($compiler in (Get-ChildItem -LiteralPath $compiledRoot -Directory)) {
            if ($compiler.Name -cnotmatch '^[0-9a-f]{64} in @('atlas-textures.json', 'atlas-index.json')) {
        $from = Join-Path $source $manifest
        if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination (Join-Path $destination $manifest) -Force }
    }
    Write-Output "Installed atlas q$quality into $destination"
}
) { throw 'Invalid atlas compiler key' }
            foreach ($entry in (Get-ChildItem -LiteralPath $compiler.FullName -File -Filter '*.json')) {
                if ($entry.BaseName -cnotmatch '^[0-9a-f]{64} in @('atlas-textures.json', 'atlas-index.json')) {
        $from = Join-Path $source $manifest
        if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination (Join-Path $destination $manifest) -Force }
    }
    Write-Output "Installed atlas q$quality into $destination"
}
) { throw 'Invalid atlas source key' }
                $record = Get-Content -LiteralPath $entry.FullName -Raw | ConvertFrom-Json
                if ($record.compilerKey -cne $compiler.Name -or $record.sourceSha256 -cne $entry.BaseName) {
                    throw "Invalid atlas compile record: $($entry.FullName)"
                }
                $compiledDestination = Join-Path (Join-Path $destination 'compiled') $compiler.Name
                $null = New-Item -ItemType Directory -Path $compiledDestination -Force
                Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $compiledDestination $entry.Name) -Force
            }
        }
    }
    $null = New-Item -ItemType Directory -Path $destination -Force
    foreach ($manifest in @('atlas-textures.json', 'atlas-index.json')) {
        $from = Join-Path $source $manifest
        if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination (Join-Path $destination $manifest) -Force }
    }
    Write-Output "Installed atlas q$quality into $destination"
}
