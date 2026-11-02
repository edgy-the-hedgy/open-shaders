param(
    [string]$AtlasSourceRoot = 'D:\pcmaster\openshadersfsrredstone\build\scene-prep\atlas',
    [string]$DataDirectory = 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
    [switch]$Deploy
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$source = Join-Path ([IO.Path]::GetFullPath($AtlasSourceRoot)) 'q0'
if (!(Test-Path -LiteralPath (Join-Path $source 'compiled') -PathType Container)) {
    throw "Q0 compiled source metadata is missing: $source"
}
$project = Join-Path $PSScriptRoot 'RedstoneScenePrep.csproj'
$artifacts = Join-Path $projectRoot 'build\scene-prep\atlas-artifacts'
$compiler = Join-Path $artifacts 'bin\RedstoneScenePrep\release\RedstoneScenePrep.dll'
if (!(Test-Path -LiteralPath $compiler)) {
    & dotnet build $project -c Release --artifacts-path $artifacts --verbosity minimal
    if ($LASTEXITCODE -ne 0) { throw 'Failed to build offline geometry-catalog tool.' }
}
& dotnet $compiler --geometry-catalog $source
if ($LASTEXITCODE -ne 0) { throw 'Failed to generate the Q0 geometry catalogue.' }
if ($Deploy) {
    if (Get-Process -Name 'SkyrimSE', 'SkyrimVR', 'skse64_loader' -ErrorAction SilentlyContinue) {
        throw 'Close Skyrim before deploying the Q0 geometry catalogue.'
    }
    $target = Join-Path ([IO.Path]::GetFullPath($DataDirectory)) 'SKSE\Plugins\CommunityShaders\RedstoneScene\atlas\q0'
    $null = New-Item -ItemType Directory -Path $target -Force
    Copy-Item -LiteralPath (Join-Path $source 'geometry-catalog.json') -Destination (Join-Path $target 'geometry-catalog.json') -Force
    Write-Output "Deployed Q0 geometry catalogue to $target"
}
