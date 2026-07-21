[CmdletBinding()]
param(
    [ValidateSet("x64")]
    [string] $Platform = "x64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

function Get-InnoSetupCompiler {
    if ($env:INNO_SETUP_COMPILER) {
        if (Test-Path -LiteralPath $env:INNO_SETUP_COMPILER -PathType Leaf) {
            return $env:INNO_SETUP_COMPILER
        }
        throw "INNO_SETUP_COMPILER does not point to an existing file."
    }

    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "Inno Setup 6 was not found. Install JRSoftware.InnoSetup with winget or set INNO_SETUP_COMPILER."
}

$repositoryRoot = Get-GlanceRepositoryRoot
$artifactsDirectory = Join-Path $repositoryRoot "artifacts"
$payloadDirectory = Join-Path $artifactsDirectory "package\payload"
$installerOutputDirectory = Join-Path $artifactsDirectory "installer"
$installerScript = Join-Path $repositoryRoot "installer\Glance.iss"
$version = Get-GlanceVersion

Remove-GlanceWorkspaceItem -Path (Join-Path $artifactsDirectory "package")
Remove-GlanceWorkspaceItem -Path $installerOutputDirectory
New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $installerOutputDirectory -Force | Out-Null

& (Join-Path $PSScriptRoot "build.ps1") `
    -Configuration Release `
    -Platform $Platform `
    -SelfContained `
    -OutputDirectory $payloadDirectory

$requiredFiles = @(
    "Glance.exe",
    "Glance.Core.exe",
    "Glance.OfficeHost.exe",
    "Glance.pri",
    "App.xbf",
    "MainWindow.xbf",
    "SettingsWindow.xbf"
)
foreach ($file in $requiredFiles) {
    $path = Join-Path $payloadDirectory $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required package file is missing: $path"
    }
}

$developmentArtifacts = Get-ChildItem -LiteralPath $payloadDirectory -Recurse -File | Where-Object {
    $_.Extension -in @(".exp", ".ilk", ".lib", ".pdb") -or $_.Name -eq "Glance.Tests.exe"
}
foreach ($artifact in $developmentArtifacts) {
    Remove-GlanceWorkspaceItem -Path $artifact.FullName
}

$compiler = Get-InnoSetupCompiler
$environment = @{
    GLANCE_SOURCE_DIR = $payloadDirectory
    GLANCE_OUTPUT_DIR = $installerOutputDirectory
    GLANCE_REPO_ROOT = $repositoryRoot
    GLANCE_VERSION = $version.Version
    GLANCE_FILE_VERSION = $version.FileVersion
}
$previousEnvironment = @{}

try {
    foreach ($entry in $environment.GetEnumerator()) {
        $previousEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, "Process")
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }

    Write-Host "Packaging Glance $($version.Version) ($Platform)..."
    & $compiler $installerScript
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup failed with exit code $LASTEXITCODE."
    }
}
finally {
    foreach ($entry in $previousEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }
}

$installer = Get-ChildItem -LiteralPath $installerOutputDirectory -Filter "Glance-Setup-*.exe" |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
if (-not $installer) {
    throw "The installer was not generated."
}

Write-Host "Installer: $($installer.FullName)"
