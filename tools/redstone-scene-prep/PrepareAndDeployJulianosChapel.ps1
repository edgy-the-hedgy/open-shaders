param(
    [string]$DataDirectory = 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data'
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'PrepareAndDeployGuestRoom.ps1') -DataDirectory $DataDirectory -Cell 'Glenmoril.esm:0099F2' -PreserveSettings
