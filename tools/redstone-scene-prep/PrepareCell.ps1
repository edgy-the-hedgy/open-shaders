param(
    [string]$DataDirectory = 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
    [string]$Cell = 'Unslaad.esm:04FD2B',
    [string]$PluginsFile = (Join-Path $env:USERPROFILE 'AppData\Local\Skyrim Special Edition\Plugins.txt'),
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$artifactRoot = Join-Path $projectRoot 'build\scene-prep'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $artifactRoot ('cells\' + $Cell.Replace(':', '\')) }
$projectFile = Join-Path $PSScriptRoot 'RedstoneScenePrep.csproj'
$artifacts = Join-Path $artifactRoot 'artifacts'
$previousCliHome = $env:DOTNET_CLI_HOME
$previousDxrData = $env:REDSTONE_DXR_HEIGHT_DATA
try {
    $env:DOTNET_CLI_HOME = Join-Path $artifactRoot 'cli'
    $env:REDSTONE_DXR_HEIGHT_DATA = $DataDirectory
    & dotnet restore $projectFile --locked-mode --packages (Join-Path $artifactRoot 'nuget') --artifacts-path $artifacts --verbosity minimal
    if ($LASTEXITCODE -ne 0) { throw 'Standalone preparation tool dependency restore failed.' }
    & dotnet build $projectFile --no-restore --artifacts-path $artifacts -c Release --verbosity minimal
    if ($LASTEXITCODE -ne 0) { throw 'Standalone preparation tool build failed.' }
    & dotnet (Join-Path $artifacts 'bin\RedstoneScenePrep\release\RedstoneScenePrep.dll') $DataDirectory $Cell $OutputDirectory $PluginsFile
    if ($LASTEXITCODE -ne 0) { throw 'Cell preparation failed; inspect the error above.' }
} finally {
    $env:DOTNET_CLI_HOME = $previousCliHome
    $env:REDSTONE_DXR_HEIGHT_DATA = $previousDxrData
}
