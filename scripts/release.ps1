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
$payloadDirectory = Join-Path $artifactsDirectory "package-Portable\payload"

Remove-GlanceWorkspaceItem -Path $stagingDirectory
Remove-GlanceWorkspaceItem -Path $releaseDirectory
New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null

& (Join-Path $PSScriptRoot "package.ps1") -Platform $Platform -Distribution Installed -RunTests
& (Join-Path $PSScriptRoot "package.ps1") -Platform $Platform -Distribution Portable -RunTests

$installer = Get-ChildItem -LiteralPath (Join-Path $artifactsDirectory "installer") `
    -Filter "Glance-Setup-$($version.Version)-$Platform.exe" |
    Select-Object -First 1
if (-not $installer) {
    throw "The versioned installer was not generated."
}
Copy-Item -LiteralPath $installer.FullName -Destination $releaseDirectory -Force

$portableName = "Glance-$($version.Version)-$Platform"
$portableRoot = Join-Path $stagingDirectory $portableName
New-Item -ItemType Directory -Path $portableRoot -Force | Out-Null
Get-ChildItem -LiteralPath $payloadDirectory | Copy-Item -Destination $portableRoot -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "LICENSE") -Destination $portableRoot -Force
$portableArchive = Join-Path $releaseDirectory "$portableName.zip"
Compress-Archive -Path $portableRoot -DestinationPath $portableArchive -CompressionLevel Optimal

$symbolsName = "Glance-$($version.Version)-$Platform-symbols"
$symbolsRoot = Join-Path $stagingDirectory $symbolsName
New-Item -ItemType Directory -Path $symbolsRoot -Force | Out-Null
foreach ($distribution in @('Installed', 'Portable')) {
    $symbolsDirectory = Join-Path $artifactsDirectory "package-$distribution\symbols"
    if (-not (Get-ChildItem -LiteralPath $symbolsDirectory -Recurse -File)) { throw "No $distribution symbols were collected" }
    $destination = Join-Path $symbolsRoot $distribution
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -Path (Join-Path $symbolsDirectory '*') -Destination $destination -Recurse -Force
}
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
