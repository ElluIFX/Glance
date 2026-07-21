[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string] $Configuration = "Debug",

    [ValidateSet("x64")]
    [string] $Platform = "x64",

    [switch] $Rebuild,
    [switch] $Clean,
    [switch] $SelfContained,
    [string] $OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

if ($Clean -and $Rebuild) {
    throw "Clean and Rebuild cannot be used together."
}

$repositoryRoot = Get-GlanceRepositoryRoot
$solution = Join-Path $repositoryRoot "Glance.sln"
$msbuild = Get-GlanceMSBuild
$target = if ($Clean) { "Clean" } elseif ($Rebuild) { "Rebuild" } else { "Build" }

Stop-GlanceProcesses

$arguments = @(
    $solution
    "/m"
    "/restore"
    "/t:$target"
    "/p:Configuration=$Configuration"
    "/p:Platform=$Platform"
    "/v:minimal"
)

if ($SelfContained) {
    $arguments += "/p:WindowsAppSDKSelfContained=true"
}

if ($OutputDirectory) {
    $resolvedOutput = Resolve-GlanceWorkspacePath -Path $OutputDirectory
    New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
    $resolvedOutput = $resolvedOutput.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $arguments += "/p:OutDir=$resolvedOutput"
}

Write-Host "Building Glance $Configuration|$Platform ($target)..."
& $msbuild @arguments
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}
