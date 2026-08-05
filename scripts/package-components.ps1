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
$innoRoot = Join-Path $OutputDirectory "inno"
$version = (Get-GlanceVersion).Version

Remove-GlanceWorkspaceItem -Path $OutputDirectory
New-Item -ItemType Directory -Path $installerRoot -Force | Out-Null
New-Item -ItemType Directory -Path $innoRoot -Force | Out-Null

$manifests = @(Get-ChildItem -LiteralPath $componentRoot -Directory |
    ForEach-Object { Join-Path $_.FullName "component.json" } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Sort-Object)
if (-not $manifests) {
    throw "No component manifests were found."
}

$messageLines = [System.Collections.Generic.List[string]]::new()
$definitionLines = [System.Collections.Generic.List[string]]::new()
$fileLines = [System.Collections.Generic.List[string]]::new()
$deleteLines = [System.Collections.Generic.List[string]]::new()
$selectNewLines = [System.Collections.Generic.List[string]]::new()
$componentIdList = [System.Collections.Generic.List[string]]::new()
$componentIds = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($manifestPath in $manifests) {
    $descriptor = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ([int] $descriptor.schema_version -ne 2) {
        throw "Component manifest '$manifestPath' has an unsupported schema."
    }
    $id = [string] $descriptor.id
    if ($id -notmatch '^[a-z][a-z0-9-]*$' -or -not $componentIds.Add($id)) {
        throw "Component manifest '$manifestPath' has an invalid or duplicate id."
    }
    $componentIdList.Add($id)
    if ([string] $descriptor.architecture -ne $Platform) {
        throw "Component '$id' does not support platform '$Platform'."
    }
    $entryPoint = [string] $descriptor.entry_point
    if ([System.IO.Path]::GetFileName($entryPoint) -ne $entryPoint) {
        throw "Component '$id' entry point must be a file name."
    }
    $englishName = [string] $descriptor.installer_names.'en-US'
    $chineseName = [string] $descriptor.installer_names.'zh-CN'
    if (-not $englishName -or -not $chineseName) {
        throw "Component '$id' is missing installer names."
    }

    $source = Join-Path $BuildOutput "components\$id"
    $componentStaging = Join-Path $installerRoot $id
    New-Item -ItemType Directory -Path $componentStaging -Force | Out-Null

    $componentFiles = @(
        "component.json"
        $entryPoint
    )
    $componentFiles += @($descriptor.payload_files | ForEach-Object { [string] $_ })
    foreach ($file in $componentFiles) {
        $path = [System.IO.Path]::GetFullPath((Join-Path $source $file))
        $sourceRoot = [System.IO.Path]::GetFullPath($source).TrimEnd('\') + '\'
        if (-not $path.StartsWith($sourceRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Component '$id' payload path escapes its build output: '$file'."
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Component '$id' build output is missing '$file'."
        }
        $destination = Join-Path $componentStaging $file
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force |
            Out-Null
        Copy-Item -LiteralPath $path -Destination $destination -Force
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

    $messageKey = "Component_" + ($id -replace '[^A-Za-z0-9_]', '_')
    $messageLines.Add("english.$messageKey=$($englishName.Replace('"', '""'))")
    $messageLines.Add("chinesesimplified.$messageKey=$($chineseName.Replace('"', '""'))")
    $definitionLines.Add(
        "Name: `"$id`"; Description: `"{cm:$messageKey}`"; Types: full")
    $fileLines.Add(
        "Source: `"{#ComponentsDir}\$id\*`"; DestDir: `"{app}\components\$id`"; Components: $id; Flags: ignoreversion recursesubdirs createallsubdirs")
    $deleteLines.Add(
        "Type: filesandordirs; Name: `"{app}\components\$id`"")
    $selectNewLines.Add(
        "  SelectComponentIfNew('$id', PreviousComponentCatalog);")
}

$utf8 = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllLines(
    (Join-Path $innoRoot "component-catalog.iss"),
    @("#define CurrentComponentCatalog `"$($componentIdList -join ',')`""),
    $utf8)
[System.IO.File]::WriteAllLines(
    (Join-Path $innoRoot "component-messages.iss"),
    $messageLines,
    $utf8)
[System.IO.File]::WriteAllLines(
    (Join-Path $innoRoot "component-definitions.iss"),
    $definitionLines,
    $utf8)
[System.IO.File]::WriteAllLines(
    (Join-Path $innoRoot "component-files.iss"),
    $fileLines,
    $utf8)
[System.IO.File]::WriteAllLines(
    (Join-Path $innoRoot "component-delete.iss"),
    $deleteLines,
    $utf8)
[System.IO.File]::WriteAllLines(
    (Join-Path $innoRoot "component-select-new.iss"),
    $selectNewLines,
    $utf8)
