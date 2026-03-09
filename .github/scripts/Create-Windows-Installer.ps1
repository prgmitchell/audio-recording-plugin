[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Create-Windows-Installer.ps1 requires CI environment"
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The installer script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function New-Installer {
    trap {
        Write-Error $_
        exit 2
    }

    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"
    $InnoScriptFile = "${PSScriptRoot}/windows-installer.iss"
    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = if ($BuildSpec.displayName) { $BuildSpec.displayName } else { $BuildSpec.name }
    $PluginDirName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version
    $VersionNumeric = ($ProductVersion -split '-', 2)[0]
    $VersionParts = $VersionNumeric -split '\.'
    $VersionParts = @($VersionParts | ForEach-Object { if ($_ -match '^\d+$') { $_ } else { '0' } })
    while ($VersionParts.Count -lt 4) {
        $VersionParts += '0'
    }
    if ($VersionParts.Count -gt 4) {
        $VersionParts = $VersionParts[0..3]
    }
    $ProductVersionWin = ($VersionParts -join '.')
    $ProductAuthor = $BuildSpec.author
    $ProductWebsite = $BuildSpec.website
    $ReleaseConfigRoot = "${ProjectRoot}/release/${Configuration}"
    $PluginPayloadRoot = "${ReleaseConfigRoot}/${PluginDirName}"
    $OutputBaseName = "${PluginDirName}-${ProductVersion}-windows-${Target}-installer"
    $OutputDir = "${ProjectRoot}/release"

    if (-not (Get-Command iscc -ErrorAction SilentlyContinue)) {
        throw "iscc was not found in PATH. Install Inno Setup before running installer packaging."
    }

    & "${PSScriptRoot}/Validate-Windows-Package.ps1" -ReleaseRoot $ReleaseConfigRoot -Configuration $Configuration

    # Avoid stale installer binaries being picked up by artifact globbing.
    Remove-Item -Path "${ProjectRoot}/release/*-windows-*-installer.exe" -ErrorAction SilentlyContinue

    $escapedProductName = $ProductName -replace '"', '""'
    $escapedProductVersion = $ProductVersion -replace '"', '""'
    $escapedProductVersionWin = $ProductVersionWin -replace '"', '""'
    $escapedProductAuthor = $ProductAuthor -replace '"', '""'
    $escapedProductWebsite = $ProductWebsite -replace '"', '""'
    $escapedPluginDirName = $PluginDirName -replace '"', '""'
    $escapedPluginPayloadRoot = $PluginPayloadRoot -replace '"', '""'
    $escapedOutputDir = $OutputDir -replace '"', '""'
    $escapedOutputBaseName = $OutputBaseName -replace '"', '""'

    Write-Host "Installer metadata: Name='${ProductName}', Version='${ProductVersion}', Author='${ProductAuthor}'"

    $TempScript = Join-Path $env:TEMP ("windows-installer-generated-" + [guid]::NewGuid().ToString("N") + ".iss")
    $IncludeScript = $InnoScriptFile -replace '\\', '\\'
    $Generated = @"
#define PRODUCT_NAME "$escapedProductName"
#define PRODUCT_VERSION "$escapedProductVersion"
#define PRODUCT_VERSION_WIN "$escapedProductVersionWin"
#define PRODUCT_AUTHOR "$escapedProductAuthor"
#define PRODUCT_WEBSITE "$escapedProductWebsite"
#define PLUGIN_DIR_NAME "$escapedPluginDirName"
#define PLUGIN_PAYLOAD_DIR "$escapedPluginPayloadRoot"
#define OUTPUT_DIR "$escapedOutputDir"
#define OUTPUT_BASENAME "$escapedOutputBaseName"
#include "$IncludeScript"
"@

    Set-Content -Path $TempScript -Value $Generated -Encoding Ascii
    try {
        & iscc $TempScript
        if ($LASTEXITCODE -ne 0) {
            throw "Inno Setup installer generation failed with exit code $LASTEXITCODE"
        }
    } finally {
        Remove-Item -Path $TempScript -ErrorAction SilentlyContinue
    }
}

New-Installer
