param(
    [Parameter(Mandatory)][string]$AtlasSourceRoot,
    [string]$DataDirectory = 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
    [ValidateRange(0, 2)][int[]]$QualityLevels = @(0),
    [switch]$CompressQ0
)

$ErrorActionPreference = 'Stop'
if (Get-Process -Name 'SkyrimSE', 'SkyrimVR', 'skse64_loader' -ErrorAction SilentlyContinue) {
    throw 'Close Skyrim before installing the universal asset atlas.'
}

$sourceRoot = [IO.Path]::GetFullPath($AtlasSourceRoot)
$targetRoot = Join-Path ([IO.Path]::GetFullPath($DataDirectory)) 'SKSE\Plugins\CommunityShaders\RedstoneScene\atlas'

foreach ($quality in @($QualityLevels | Sort-Object -Unique)) {
    $source = Join-Path $sourceRoot "q$quality"
    $index = Join-Path $source 'atlas-index.json'
    if (!(Test-Path -LiteralPath $index)) { throw "Missing atlas index: $index" }
    $metadata = Get-Content -LiteralPath $index -Raw | ConvertFrom-Json
    if ($metadata.schemaVersion -ne 1 -or $metadata.quality -ne $quality -or
        $metadata.kind -cne 'native-scene-universal-asset-atlas') {
        throw "Invalid atlas manifest: q$quality"
    }

    $destination = Join-Path $targetRoot "q$quality"
    $linked = 0
    $copied = 0
    $unchanged = 0
    $null = New-Item -Path $destination -ItemType Directory -Force
    foreach ($folder in @('geometry', 'shading', 'textures')) {
        $sourceFolder = Join-Path $source $folder
        if (!(Test-Path -LiteralPath $sourceFolder)) { continue }
        $targetFolder = Join-Path $destination $folder
        $null = New-Item -Path $targetFolder -ItemType Directory -Force
        $expectedExtension = switch ($folder) {
            'geometry' { 'rsgm' }
            'shading' { 'rsga' }
            'textures' { 'dds' }
        }
        foreach ($asset in (Get-ChildItem -LiteralPath $sourceFolder -File)) {
            $match = [regex]::Match($asset.Name, "^([0-9a-f]{64})\.$expectedExtension$")
            if (!$match.Success) { throw "Invalid atlas payload name: $($asset.Name)" }
            $hash = $match.Groups[1].Value
            $target = Join-Path $targetFolder $asset.Name
            $actual = (Get-FileHash -LiteralPath $asset.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -cne $hash) { throw "Atlas payload hash mismatch: $($asset.FullName)" }
            if (Test-Path -LiteralPath $target) {
                $installed = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($installed -ceq $hash) { ++$unchanged; continue }
            }
            # The atlas store is content-addressed and immutable. When build
            # output and the game are on one NTFS volume, hardlinks avoid a
            # second full copy. Deleting either link leaves the other intact.
            $temporary = $target + '.' + [guid]::NewGuid().ToString('N') + '.part'
            try {
                try {
                    $null = New-Item -ItemType HardLink -Path $temporary -Target $asset.FullName -ErrorAction Stop
                    ++$linked
                } catch {
                    Copy-Item -LiteralPath $asset.FullName -Destination $temporary -ErrorAction Stop
                    ++$copied
                }
                Move-Item -LiteralPath $temporary -Destination $target -Force
            } finally {
                if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
            }
        }
    }

    # NIF compile records are reused by later per-cell source resolution.
    $compiledRoot = Join-Path $source 'compiled'
    if (Test-Path -LiteralPath $compiledRoot) {
        foreach ($compiler in (Get-ChildItem -LiteralPath $compiledRoot -Directory)) {
            if ($compiler.Name -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid compiler cache key.' }
            $compiledDestination = Join-Path (Join-Path $destination 'compiled') $compiler.Name
            $null = New-Item -Path $compiledDestination -ItemType Directory -Force
            foreach ($entry in (Get-ChildItem -LiteralPath $compiler.FullName -File -Filter '*.json')) {
                if ($entry.BaseName -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid source hash in compiled cache.' }
                $record = Get-Content -LiteralPath $entry.FullName -Raw | ConvertFrom-Json
                if ($record.compilerKey -cne $compiler.Name -or $record.sourceSha256 -cne $entry.BaseName) {
                    throw "Compiled atlas metadata mismatch: $($entry.FullName)"
                }
                Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $compiledDestination $entry.Name) -Force
            }
        }
    }
    foreach ($manifest in @('atlas-index.json', 'atlas-textures.json')) {
        $from = Join-Path $source $manifest
        if (Test-Path -LiteralPath $from) {
            Copy-Item -LiteralPath $from -Destination (Join-Path $destination $manifest) -Force
        }
    }
    Write-Output "Installed verified atlas q$quality -> $destination; hardlinked: $linked, copied: $copied, unchanged: $unchanged"
}

if ($CompressQ0 -and (@($QualityLevels) -contains 0)) {
    & (Join-Path $PSScriptRoot 'CompressQ0Atlas.ps1') -AtlasRoot $targetRoot
}
