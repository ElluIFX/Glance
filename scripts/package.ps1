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
$ffmpegVersion = "8.1.1"
$ffmpegArchiveName = "ffmpeg-$ffmpegVersion-essentials_build.zip"
$ffmpegArchiveUrl = "https://github.com/GyanD/codexffmpeg/releases/download/$ffmpegVersion/$ffmpegArchiveName"
$ffmpegArchiveSha256 = "6f58ce889f59c311410f7d2b18895b33c03456463486f3b1ebc93d97a0f54541"
$dependencyCacheDirectory = Join-Path $artifactsDirectory "dependencies"
$ffmpegArchive = Join-Path $dependencyCacheDirectory $ffmpegArchiveName
$ffmpegExtractDirectory = Join-Path $artifactsDirectory "package\ffmpeg"

New-Item -ItemType Directory -Path $dependencyCacheDirectory -Force | Out-Null
if (Test-Path -LiteralPath $ffmpegArchive -PathType Leaf) {
    $actualHash = (Get-FileHash -LiteralPath $ffmpegArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ffmpegArchiveSha256) {
        Remove-GlanceWorkspaceItem -Path $ffmpegArchive
    }
}
if (-not (Test-Path -LiteralPath $ffmpegArchive -PathType Leaf)) {
    $download = "$ffmpegArchive.download"
    Remove-GlanceWorkspaceItem -Path $download
    Write-Host "Downloading FFmpeg $ffmpegVersion essentials build..."
    Invoke-WebRequest -UseBasicParsing -Uri $ffmpegArchiveUrl -OutFile $download
    $actualHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ffmpegArchiveSha256) {
        Remove-GlanceWorkspaceItem -Path $download
        throw "FFmpeg archive SHA-256 mismatch. Expected $ffmpegArchiveSha256, received $actualHash."
    }
    Move-Item -LiteralPath $download -Destination $ffmpegArchive
}

Remove-GlanceWorkspaceItem -Path (Join-Path $artifactsDirectory "package")
Remove-GlanceWorkspaceItem -Path $installerOutputDirectory
New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $installerOutputDirectory -Force | Out-Null

& (Join-Path $PSScriptRoot "build.ps1") `
    -Configuration Release `
    -Platform $Platform `
    -SelfContained `
    -OutputDirectory $payloadDirectory

Expand-Archive -LiteralPath $ffmpegArchive -DestinationPath $ffmpegExtractDirectory -Force
$ffprobe = Get-ChildItem -LiteralPath $ffmpegExtractDirectory -Filter "ffprobe.exe" -Recurse -File |
    Select-Object -First 1
if (-not $ffprobe) {
    throw "The pinned FFmpeg archive does not contain ffprobe.exe."
}
Copy-Item -LiteralPath $ffprobe.FullName -Destination (Join-Path $payloadDirectory "ffprobe.exe") -Force

$licenseDirectory = Join-Path $payloadDirectory "licenses"
New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot "licenses\FFmpeg-GPL-3.0.txt") `
    -Destination (Join-Path $licenseDirectory "FFmpeg-GPL-3.0.txt") -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "licenses\FFmpeg-NOTICE.txt") `
    -Destination (Join-Path $licenseDirectory "FFmpeg-NOTICE.txt") -Force
Remove-GlanceWorkspaceItem -Path $ffmpegExtractDirectory

$requiredFiles = @(
    "Glance.exe",
    "Glance.Core.exe",
    "Glance.DialogHook.dll",
    "Glance.OfficeHost.exe",
    "ffprobe.exe",
    "Glance.pri",
    "App.xbf",
    "MainWindow.xbf",
    "SettingsWindow.xbf",
    "licenses\FFmpeg-GPL-3.0.txt",
    "licenses\FFmpeg-NOTICE.txt"
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
