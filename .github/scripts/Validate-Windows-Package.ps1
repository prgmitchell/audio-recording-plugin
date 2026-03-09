[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ReleaseRoot,
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

function Assert-PathExists {
    param(
        [Parameter(Mandatory = $true)]
        [string] $PathToCheck,
        [Parameter(Mandatory = $true)]
        [string] $Message
    )

    if (-not (Test-Path $PathToCheck)) {
        throw $Message
    }
}

$ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
$BuildSpecPath = Join-Path $ProjectRoot 'buildspec.json'
$BuildSpec = Get-Content -Path $BuildSpecPath -Raw | ConvertFrom-Json
$PluginRootName = $BuildSpec.name
$ReleaseRoot = Resolve-Path -Path $ReleaseRoot

$TopLevelEntries = Get-ChildItem -Path $ReleaseRoot -Force
$UnexpectedTopLevel = $TopLevelEntries | Where-Object { $_.Name -ne $PluginRootName }
if ($UnexpectedTopLevel) {
    $Names = ($UnexpectedTopLevel | ForEach-Object { $_.Name }) -join ', '
    throw "Release root '$ReleaseRoot' contains unexpected top-level entries: $Names"
}

$PluginRoot = Join-Path $ReleaseRoot $PluginRootName
$BinRoot = Join-Path $PluginRoot 'bin/64bit'
$DataLocaleRoot = Join-Path $PluginRoot 'data/locale'
$LocaleFile = Join-Path $DataLocaleRoot 'en-US.ini'

Assert-PathExists -PathToCheck $PluginRoot -Message "Missing plugin root folder: $PluginRoot"
Assert-PathExists -PathToCheck $BinRoot -Message "Missing bin folder: $BinRoot"
Assert-PathExists -PathToCheck $DataLocaleRoot -Message "Missing locale folder: $DataLocaleRoot"
Assert-PathExists -PathToCheck $LocaleFile -Message "Missing locale file: $LocaleFile"

$DllFiles = Get-ChildItem -Path $BinRoot -Filter *.dll -File
if (-not $DllFiles) {
    throw "No plugin DLL found in $BinRoot"
}

$PdbFiles = Get-ChildItem -Path $BinRoot -Filter *.pdb -File
if (-not $PdbFiles) {
    throw "No plugin PDB found in $BinRoot"
}

Write-Host "Windows package layout validated for '$PluginRootName' ($Configuration)."
