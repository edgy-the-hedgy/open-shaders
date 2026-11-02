param(
    [string]$DataDirectory = 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
    [string]$PluginsFile = (Join-Path $env:LOCALAPPDATA 'Skyrim Special Edition\Plugins.txt'),
    [string]$AtlasOutputRoot,
    [ValidateRange(0, 2)][int[]]$QualityLevels = @(0)
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$artifactRoot = Join-Path $projectRoot 'build\scene-prep'
if (!$AtlasOutputRoot) { $AtlasOutputRoot = Join-Path $artifactRoot 'atlas' }
$projectFile = Join-Path $PSScriptRoot 'RedstoneScenePrep.csproj'
$artifacts = Join-Path $artifactRoot 'atlas-artifacts'
$previousCliHome = $env:DOTNET_CLI_HOME
$previousDxrData = $env:REDSTONE_DXR_HEIGHT_DATA
try {
    $env:DOTNET_CLI_HOME = Join-Path $artifactRoot 'cli'
    $env:REDSTONE_DXR_HEIGHT_DATA = $DataDirectory
    & dotnet restore $projectFile --locked-mode --packages (Join-Path $artifactRoot 'nuget') --artifacts-path $artifacts --verbosity minimal
    if ($LASTEXITCODE -ne 0) { throw 'Atlas compiler dependency restore failed.' }
    & dotnet build $projectFile --no-restore --artifacts-path $artifacts -c Release --verbosity minimal
    if ($LASTEXITCODE -ne 0) { throw 'Atlas compiler build failed.' }
    $compiler = Join-Path $artifacts 'bin\RedstoneScenePrep\release\RedstoneScenePrep.dll'
    $levels = @($QualityLevels | Sort-Object -Unique)
    if (($levels.Count -eq 2 -or $levels.Count -eq 3) -and
        $levels[0] -eq 0 -and $levels[1] -eq 1 -and
        ($levels.Count -eq 2 -or $levels[2] -eq 2)) {
        Write-Output "Using one-pass universal atlas build for q$($levels -join ', q')..."
        & dotnet $compiler --atlas-batch $DataDirectory $AtlasOutputRoot $PluginsFile ($levels -join ',')
        if ($LASTEXITCODE -ne 0) { throw 'One-pass atlas compilation failed; check the logged per-asset failures.' }
    } else {
        foreach ($level in $levels) {
            Write-Output "Preparing universal asset atlas quality q$level..."
            & dotnet $compiler --atlas $DataDirectory $AtlasOutputRoot $level $PluginsFile
            if ($LASTEXITCODE -ne 0) { throw "Atlas compilation for quality q$level failed." }
        }
    }
    Write-Output "Offline atlas generated at $AtlasOutputRoot. Run StagePreparedAtlas.ps1 after closing Skyrim."
} finally {
    $env:DOTNET_CLI_HOME = $previousCliHome
    $env:REDSTONE_DXR_HEIGHT_DATA = $previousDxrData
}
