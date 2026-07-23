[CmdletBinding()]
param(
    [ValidateSet("x64")]
    [string] $Platform = "x64",

    [switch] $RunTests
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
$symbolsDirectory = Join-Path $artifactsDirectory "package\symbols"
$installerOutputDirectory = Join-Path $artifactsDirectory "installer"
$installerScript = Join-Path $repositoryRoot "installer\Glance.iss"
$version = Get-GlanceVersion

Remove-GlanceWorkspaceItem -Path (Join-Path $artifactsDirectory "package")
Remove-GlanceWorkspaceItem -Path $installerOutputDirectory
New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $symbolsDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $installerOutputDirectory -Force | Out-Null

& (Join-Path $PSScriptRoot "build.ps1") `
    -Configuration Release `
    -Platform $Platform `
    -SelfContained `
    -OutputDirectory $payloadDirectory

$requiredFiles = @(
    "Glance.exe",
    "Glance.Core.exe",
    "Glance.DialogBroker32.exe",
    "Glance.DialogHook.dll",
    "Glance.DialogHook32.dll",
    "Glance.OfficeHost.exe",
    "Glance.RenderHost.exe",
    "pdfium.dll",
    "Lexilla.dll",
    "Scintilla.dll",
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

if ($RunTests) {
    $tests = Join-Path $payloadDirectory "Glance.Tests.exe"
    if (-not (Test-Path -LiteralPath $tests -PathType Leaf)) {
        throw "Release tests were not built: $tests"
    }
    Write-Host "Running release regression tests..."
    & $tests
    if ($LASTEXITCODE -ne 0) {
        throw "Release regression tests failed with exit code $LASTEXITCODE."
    }
}

$symbolFiles = Get-ChildItem -LiteralPath $payloadDirectory -Recurse -File -Filter "*.pdb" |
    Where-Object { $_.Name -ne "Glance.Tests.pdb" }
foreach ($symbol in $symbolFiles) {
    $relativePath = [System.IO.Path]::GetRelativePath($payloadDirectory, $symbol.FullName)
    $destination = Join-Path $symbolsDirectory $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $symbol.FullName -Destination $destination -Force
}

$developmentArtifacts = Get-ChildItem -LiteralPath $payloadDirectory -Recurse -File | Where-Object {
    $_.Extension -in @(".exp", ".ilk", ".lib", ".pdb") -or $_.Name -eq "Glance.Tests.exe"
}
foreach ($artifact in $developmentArtifacts) {
    Remove-GlanceWorkspaceItem -Path $artifact.FullName
}

$pdfiumLicenseSource = Join-Path $repositoryRoot "licenses\PDFium"
$pdfiumLicenseDestination = Join-Path $payloadDirectory "licenses\PDFium"
New-Item -ItemType Directory -Path $pdfiumLicenseDestination -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot "licenses\PDFium-NOTICE.txt") `
    -Destination (Join-Path $payloadDirectory "licenses\PDFium-NOTICE.txt") -Force
Copy-Item -Path (Join-Path $pdfiumLicenseSource "*") `
    -Destination $pdfiumLicenseDestination -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "licenses\Scintilla-Lexilla.txt") `
    -Destination (Join-Path $payloadDirectory "licenses\Scintilla-Lexilla.txt") -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "licenses\Tinted-Theming.txt") `
    -Destination (Join-Path $payloadDirectory "licenses\Tinted-Theming.txt") -Force

$forbiddenRuntimeFiles = Get-ChildItem -LiteralPath $payloadDirectory -Recurse -File | Where-Object {
    $_.Name -in @(
        "DirectML.dll",
        "NPUDetect.dll",
        "onnxruntime.dll",
        "PerceptiveStreaming.dll",
        "Microsoft.Windows.Widgets.dll",
        "Microsoft.Windows.Widgets.winmd"
    ) -or
    $_.Name -like "Microsoft.Windows.AI.*" -or
    $_.Name -like "Microsoft.Windows.Internal.AI.*" -or
    $_.Name -like "Microsoft.Windows.Workloads*" -or
    $_.Name -like "workloads*.json"
}
if ($forbiddenRuntimeFiles) {
    $relativePaths = $forbiddenRuntimeFiles | ForEach-Object {
        [System.IO.Path]::GetRelativePath($payloadDirectory, $_.FullName)
    }
    throw "Unused Windows App SDK AI, ML, or Widgets files entered the package payload: $($relativePaths -join ', ')"
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
