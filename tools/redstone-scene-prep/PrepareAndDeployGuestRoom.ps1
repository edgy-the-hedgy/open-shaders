param(
    [string]$DataDirectory = 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
    [ValidatePattern('^[^\\/:*?"<>|]+\.(esm|esp|esl):[0-9A-Fa-f]{6}$')]
    [string]$Cell = 'Unslaad.esm:04FD2B',
    [switch]$PreserveSettings
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$artifactRoot = Join-Path $repoRoot 'build\scene-prep'
$plugin, $localCell = $Cell.Split(':')
$cellPath = Join-Path $plugin $localCell.ToUpperInvariant()
$source = Join-Path $artifactRoot (Join-Path 'cells' $cellPath)
$staged = Join-Path $artifactRoot (Join-Path 'runtime\SKSE\Plugins\CommunityShaders\RedstoneScene\cells' $cellPath)
$destination = Join-Path ([IO.Path]::GetFullPath($DataDirectory)) (Join-Path 'SKSE\Plugins\CommunityShaders\RedstoneScene\cells' $cellPath)
$logName = if ($Cell -eq 'Unslaad.esm:04FD2B') { 'prepare-deploy-guestroom.log' } else { 'prepare-deploy-{0}-{1}.log' -f $plugin, $localCell }
$log = Join-Path $artifactRoot $logName

function Enable-PreparedMaterialPreview {
    $settingsPath = Join-Path $DataDirectory 'SKSE\Plugins\CommunityShaders\SettingsUser.json'
    $original = [IO.File]::ReadAllText($settingsPath)
    $null = $original | ConvertFrom-Json
    $pattern = '("RedstoneShadows"\s*:\s*)(\{[^{}]*\})'
    $featureMatches = [regex]::Matches($original, $pattern)
    if ($featureMatches.Count -ne 1) { throw 'Cannot safely locate the flat RedstoneShadows settings object; installed settings were not edited.' }
    $match = $featureMatches[0]
    $feature = $match.Groups[2].Value | ConvertFrom-Json
    foreach ($name in @('UsePreparedScene', 'PreviewPreparedScene', 'UsePreparedMaterials')) {
        $feature | Add-Member -NotePropertyName $name -NotePropertyValue $true -Force
    }
    if (!$feature.PSObject.Properties['PreparedMaterialView'] -or $feature.PreparedMaterialView -eq 0) {
        $feature | Add-Member -NotePropertyName PreparedMaterialView -NotePropertyValue 1 -Force
    }
    if (!$feature.PSObject.Properties['MaterialTextureBudgetMB']) {
        $feature | Add-Member -NotePropertyName MaterialTextureBudgetMB -NotePropertyValue 512 -Force
    }
    $replacement = $feature | ConvertTo-Json -Depth 10
    $updated = $original.Substring(0, $match.Groups[2].Index) + $replacement + $original.Substring($match.Groups[2].Index + $match.Groups[2].Length)
    $null = $updated | ConvertFrom-Json
    Copy-Item -LiteralPath $settingsPath -Destination (Join-Path $artifactRoot ('SettingsUser-before-materials-{0}.json' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
    [IO.File]::WriteAllText($settingsPath, $updated, (New-Object Text.UTF8Encoding($false)))
    Write-Host 'Enabled prepared geometry, its preview and material maps. Existing nonzero material view/budget selections were preserved.'
}

$null = New-Item -ItemType Directory -Path $artifactRoot -Force
$transcript = $false
Push-Location $repoRoot
try {
    Start-Transcript -Path $log -Force | Out-Null
    $transcript = $true
    if (@(Get-Process -Name SkyrimSE, SkyrimVR, skse64_loader -ErrorAction SilentlyContinue).Count) {
        throw 'Close Skyrim before preparing and deploying its material cache, then run this shortcut again.'
    }
    if (!(Test-Path -LiteralPath (Join-Path $DataDirectory $plugin))) { throw "$plugin was not found in the selected Data directory." }
    Write-Host "1/4 Rebuilding the standalone preparation tool and regenerating $Cell (no tests)."
    & (Join-Path $PSScriptRoot 'PrepareCell.ps1') -DataDirectory $DataDirectory -Cell $Cell -OutputDirectory $source

    $layout = Get-Content -LiteralPath (Join-Path $source 'cell-layout.json') -Raw | ConvertFrom-Json
    $textures = Get-Content -LiteralPath (Join-Path $source 'cell-textures.json') -Raw | ConvertFrom-Json
    if ($layout.cell -ine ('{0}:{1}' -f $localCell, $plugin)) { throw 'Generated layout does not match the requested cell; deployment was skipped.' }
    if ($layout.shadingSchemaVersion -ne 2 -or $layout.materialSchemaVersion -ne 1) { throw 'Preparation did not produce the current material/shading schema.' }
    if ($textures.schemaVersion -ne 1 -or $textures.cell -cne $layout.cell -or $textures.layoutDigest -cne $layout.layoutDigest) { throw 'Generated texture manifest does not match the room layout.' }
    $unresolved = @($textures.textures | Where-Object { $_.status -ne 'cached' })
    if ($unresolved.Count) {
        $unresolved | Select-Object path, status | Format-Table -AutoSize | Out-Host
        throw ('{0} texture sources remain unresolved; deployment was skipped.' -f $unresolved.Count)
    }
    if (@($textures.textures).Count -eq 0) { throw 'Preparation produced no texture sources; deployment was skipped.' }

    Write-Host '2/4 Staging and checking the generated geometry, shading and DDS files.'
    & (Join-Path $PSScriptRoot 'StagePreparedCell.ps1') -SourceDirectory $source

    Write-Host "3/4 Deploying the $Cell cache and verifying installed bytes."
    $payloads = @($layout.instances | ForEach-Object { $_.geometryFile; if ($_.shadingStatus -eq 'compiled') { $_.shadingFile } })
    $payloads += @($textures.textures | ForEach-Object { $_.file })
    $payloads = @($payloads | Sort-Object -Unique)
    foreach ($relative in $payloads) {
        if ($relative -cnotmatch '^(geometry/[0-9a-f]{64}\.rsgm|shading/[0-9a-f]{64}\.rsga|textures/[0-9a-f]{64}\.dds)$') { throw 'Invalid staged payload path.' }
    }
    $filesToDeploy = @($payloads) + @('cell-textures.json', 'cell-layout.json')
    foreach ($relative in $filesToDeploy) {
        $from = Join-Path $staged $relative
        $to = Join-Path $destination $relative
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force
        $expected = (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash
        if (!(Test-Path -LiteralPath $to) -or (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash -ne $expected) {
            Copy-Item -LiteralPath $from -Destination $to -Force
        }
        if ((Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash -ne $expected) { throw "Deployment verification failed: $relative" }
    }

    if ($PreserveSettings) {
        Write-Host '4/4 Preserving saved runtime settings.'
    } else {
        Write-Host '4/4 Enabling the prepared material preview in saved settings.'
        Enable-PreparedMaterialPreview
    }
    Write-Host ('DONE: {0} unique texture paths prepared; cache installed at {1}' -f @($textures.textures).Count, $destination)
    Write-Host 'No Skyrim plugin build, tests or game launch were performed. Launch Skyrim yourself; the preview starts with source base color when no material view was previously selected.'
    Write-Host "Log: $log"
} catch {
    Write-Host ('FAILED: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host "Log: $log"
    exit 1
} finally {
    if ($transcript) { Stop-Transcript | Out-Null }
    Pop-Location
}
