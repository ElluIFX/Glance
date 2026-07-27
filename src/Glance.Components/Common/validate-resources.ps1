[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ResourceRoot,

    [string] $DefaultLanguage = "en-US"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-ResourceKeys {
    param(
        [Parameter(Mandatory)]
        [string] $Path
    )

    [xml] $document = Get-Content -Raw -LiteralPath $Path
    return @($document.root.data | ForEach-Object { [string] $_.name } | Sort-Object -Unique)
}

$defaultPath = Join-Path $ResourceRoot "$DefaultLanguage\Resources.resw"
if (-not (Test-Path -LiteralPath $defaultPath -PathType Leaf)) {
    throw "Default component resource file is missing: $defaultPath"
}

$defaultKeys = @(Get-ResourceKeys -Path $defaultPath)
if (-not $defaultKeys) {
    throw "Default component resource file contains no strings: $defaultPath"
}

$resourceFiles = @(Get-ChildItem -LiteralPath $ResourceRoot -Directory |
    ForEach-Object { Join-Path $_.FullName "Resources.resw" } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Sort-Object)
foreach ($resourceFile in $resourceFiles) {
    $keys = @(Get-ResourceKeys -Path $resourceFile)
    $difference = @(Compare-Object -ReferenceObject $defaultKeys -DifferenceObject $keys)
    if ($difference) {
        $details = $difference |
            ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }
        throw "Component resource keys differ in '$resourceFile': $($details -join ', ')"
    }
}
