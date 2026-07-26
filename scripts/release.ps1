[CmdletBinding()]
param(
    [ValidateSet("x64")]
    [string] $Platform = "x64",

    [string] $ExpectedTag
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

$repositoryRoot = Get-GlanceRepositoryRoot
$version = Get-GlanceVersion
$tag = "v$($version.Version)"
if ($ExpectedTag -and $ExpectedTag -ne $tag) {
    throw "Release tag '$ExpectedTag' does not match version '$tag'."
}

$artifactsDirectory = Join-Path $repositoryRoot "artifacts"
$releaseDirectory = Join-Path $artifactsDirectory "release"
$stagingDirectory = Join-Path $artifactsDirectory "release-staging"
$payloadDirectory = Join-Path $artifactsDirectory "package\payload"
$symbolsDirectory = Join-Path $artifactsDirectory "package\symbols"

Remove-GlanceWorkspaceItem -Path $stagingDirectory
Remove-GlanceWorkspaceItem -Path $releaseDirectory
New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null

& (Join-Path $PSScriptRoot "package.ps1") -Platform $Platform -RunTests

$installer = Get-ChildItem -LiteralPath (Join-Path $artifactsDirectory "installer") `
    -Filter "Glance-Setup-$($version.Version)-$Platform.exe" |
    Select-Object -First 1
if (-not $installer) {
    throw "The versioned installer was not generated."
}
Copy-Item -LiteralPath $installer.FullName -Destination $releaseDirectory -Force

$componentArchives = @(Get-ChildItem -LiteralPath (Join-Path $artifactsDirectory "components") `
    -Filter "Glance-Component-*-$Platform.zip" -File)
if (-not $componentArchives) {
    throw "No component archives were generated."
}
foreach ($archive in $componentArchives) {
    Copy-Item -LiteralPath $archive.FullName -Destination $releaseDirectory -Force
}

$portableName = "Glance-$($version.Version)-$Platform"
$portableRoot = Join-Path $stagingDirectory $portableName
New-Item -ItemType Directory -Path $portableRoot -Force | Out-Null
Copy-Item -Path (Join-Path $payloadDirectory "*") -Destination $portableRoot -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "LICENSE") -Destination $portableRoot -Force
$portableArchive = Join-Path $releaseDirectory "$portableName.zip"
Compress-Archive -Path $portableRoot -DestinationPath $portableArchive -CompressionLevel Optimal

$symbolFiles = Get-ChildItem -LiteralPath $symbolsDirectory -Recurse -File
if (-not $symbolFiles) {
    throw "No symbol files were collected from the release build."
}
$symbolsName = "Glance-$($version.Version)-$Platform-symbols"
$symbolsRoot = Join-Path $stagingDirectory $symbolsName
New-Item -ItemType Directory -Path $symbolsRoot -Force | Out-Null
Copy-Item -Path (Join-Path $symbolsDirectory "*") -Destination $symbolsRoot -Recurse -Force
$symbolsArchive = Join-Path $releaseDirectory "$symbolsName.zip"
Compress-Archive -Path $symbolsRoot -DestinationPath $symbolsArchive -CompressionLevel Optimal

$checksumPath = Join-Path $releaseDirectory "SHA256SUMS.txt"
$checksumLines = Get-ChildItem -LiteralPath $releaseDirectory -File |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
    Sort-Object Name |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $($_.Name)"
    }
[System.IO.File]::WriteAllLines($checksumPath, $checksumLines, [System.Text.UTF8Encoding]::new($false))

try {
    Remove-GlanceWorkspaceItem -Path $stagingDirectory
}
catch {
    Write-Warning "Release assets are complete, but temporary files could not be removed: $($_.Exception.Message)"
}

Write-Host "Release assets:"
Get-ChildItem -LiteralPath $releaseDirectory -File | Sort-Object Name | ForEach-Object {
    Write-Host "  $($_.FullName)"
}
