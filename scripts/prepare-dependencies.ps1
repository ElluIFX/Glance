[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

$repositoryRoot = Get-GlanceRepositoryRoot
$destinationDirectory = Join-Path $repositoryRoot "src\Glance.App\third_party\scintilla\bin\x64"
$dependencies = @(
    @{
        Name = "Scintilla.dll"
        Sha256 = "795B30CC239862E9F7AC1E92118A939B0059FC92073EA65AEB1969D24CD8B12B"
    },
    @{
        Name = "Lexilla.dll"
        Sha256 = "E20D2B61B605A0FEABFCC4562C11FB5D1FC42DC2FC5C143C74D4CFD63972B5D3"
    }
)

function Test-Dependency {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [string] $Sha256
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }

    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    return $actualHash -eq $Sha256
}

$allAvailable = $true
foreach ($dependency in $dependencies) {
    $path = Join-Path $destinationDirectory $dependency.Name
    if (-not (Test-Dependency -Path $path -Sha256 $dependency.Sha256)) {
        $allAvailable = $false
        break
    }
}

if ($allAvailable) {
    Write-Host "Scintilla runtime dependencies are available."
    return
}

$downloadUri = "https://www.scintilla.org/wscite564.zip"
$archiveSha256 = "DB573FB65C7C1979EDA11F36045A59446F87EB282E3DCAA47475F8A19074C094"
$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
    "Glance.Dependencies." + [Guid]::NewGuid().ToString("N"))
$archivePath = Join-Path $temporaryDirectory "wscite564.zip"
$expandedDirectory = Join-Path $temporaryDirectory "expanded"

try {
    New-Item -ItemType Directory -Path $temporaryDirectory -Force | Out-Null

    Write-Host "Downloading Scintilla 5.6.4 and Lexilla 5.5.1..."
    Invoke-WebRequest -Uri $downloadUri -OutFile $archivePath

    $actualArchiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    if ($actualArchiveHash -ne $archiveSha256) {
        throw "Scintilla archive SHA-256 mismatch. Expected $archiveSha256, got $actualArchiveHash."
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $expandedDirectory
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null

    foreach ($dependency in $dependencies) {
        $sourcePath = Join-Path $expandedDirectory ("wscite\" + $dependency.Name)
        if (-not (Test-Dependency -Path $sourcePath -Sha256 $dependency.Sha256)) {
            throw "$($dependency.Name) is missing or failed SHA-256 validation."
        }

        Copy-Item -LiteralPath $sourcePath `
            -Destination (Join-Path $destinationDirectory $dependency.Name) `
            -Force
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}

Write-Host "Scintilla runtime dependencies are ready."
