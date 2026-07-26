[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $BuildOutput,

    [Parameter(Mandatory)]
    [string] $OutputDirectory,

    [ValidateSet("x64")]
    [string] $Platform = "x64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

$repositoryRoot = Get-GlanceRepositoryRoot
$componentRoot = Join-Path $repositoryRoot "src\Glance.Components"
$installerRoot = Join-Path $OutputDirectory "installer"
$version = (Get-GlanceVersion).Version

Remove-GlanceWorkspaceItem -Path $OutputDirectory
New-Item -ItemType Directory -Path $installerRoot -Force | Out-Null

$manifests = @(Get-ChildItem -LiteralPath $componentRoot -Directory |
    ForEach-Object { Join-Path $_.FullName "component.json" } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
foreach ($manifestPath in $manifests) {
    $descriptor = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $id = [string] $descriptor.id
    $source = Join-Path $BuildOutput "components\$id"
    $componentStaging = Join-Path $installerRoot $id
    New-Item -ItemType Directory -Path $componentStaging -Force | Out-Null

    $componentFiles = @(
        "component.json"
        [string] $descriptor.entry_point
    )
    $componentFiles += @($descriptor.payload_files | ForEach-Object { [string] $_ })
    foreach ($file in $componentFiles) {
        $path = Join-Path $source $file
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Component '$id' build output is missing '$file'."
        }
        Copy-Item -LiteralPath $path -Destination (Join-Path $componentStaging $file) -Force
    }

    foreach ($runtime in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
        $runtimePath = Join-Path $BuildOutput $runtime
        if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
            throw "Component '$id' runtime dependency is missing '$runtime'."
        }
        Copy-Item -LiteralPath $runtimePath -Destination (Join-Path $componentStaging $runtime) -Force
    }

    $displayId = $id.Substring(0, 1).ToUpperInvariant() + $id.Substring(1)
    $archive = Join-Path $OutputDirectory "Glance-Component-$displayId-$version-$Platform.zip"
    Compress-Archive -Path $componentStaging -DestinationPath $archive -CompressionLevel Optimal
}
