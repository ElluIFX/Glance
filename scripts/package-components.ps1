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
$componentDependencies = @{}
$componentDescriptors = @{}
foreach ($manifestPath in $manifests) {
    $descriptor = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ([int] $descriptor.schema_version -ne 3) {
        throw "Component manifest '$manifestPath' has an unsupported schema."
    }
    $id = [string] $descriptor.id
    if ($id -notmatch '^[a-z][a-z0-9-]*$' -or -not $componentIds.Add($id)) {
        throw "Component manifest '$manifestPath' has an invalid or duplicate id."
    }
    $componentIdList.Add($id)
    $dependencies = @($descriptor.dependencies | ForEach-Object { [string] $_ })
    $dependencyIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($dependency in $dependencies) {
        if ($dependency -notmatch '^[a-z][a-z0-9-]*$' -or
            $dependency -eq $id -or
            -not $dependencyIds.Add($dependency)) {
            throw "Component '$id' has an invalid dependency '$dependency'."
        }
    }
    $componentDependencies[$id] = $dependencies
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
    $componentDescriptors[$id] = [pscustomobject]@{
        EnglishName = $englishName
        ChineseName = $chineseName
        EntryPoint = $entryPoint
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
}

foreach ($id in $componentIdList) {
    foreach ($dependency in $componentDependencies[$id]) {
        if (-not $componentIds.Contains($dependency)) {
            throw "Component '$id' depends on missing component '$dependency'."
        }
    }
}
$remaining = [System.Collections.Generic.HashSet[string]]::new(
    $componentIdList,
    [System.StringComparer]::OrdinalIgnoreCase)
$componentOrder = [System.Collections.Generic.List[string]]::new()
while ($remaining.Count -gt 0) {
    $resolved = @($remaining | Where-Object {
        $id = $_
        -not @($componentDependencies[$id] | Where-Object { $remaining.Contains($_) })
    } | Sort-Object)
    if (-not $resolved) {
        throw "Component dependency graph contains a cycle: $($remaining -join ', ')."
    }
    foreach ($id in $resolved) {
        $componentOrder.Add($id)
        $remaining.Remove($id) | Out-Null
    }
}

$installerComponentNames = @{}
$dependencyProviders = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($id in $componentOrder) {
    $dependencies = @($componentDependencies[$id])
    foreach ($dependency in $dependencies) {
        $dependencyProviders.Add($dependency) | Out-Null
    }
    $installerId = $id -replace '-', '_'
    $installerComponentNames[$id] = if ($dependencies.Count -eq 1) {
        "$($installerComponentNames[$dependencies[0]])\$installerId"
    }
    else {
        $installerId
    }
}

foreach ($id in $componentOrder) {
    $descriptor = $componentDescriptors[$id]
    $dependencies = @($componentDependencies[$id])
    $englishName = [string] $descriptor.EnglishName
    $chineseName = [string] $descriptor.ChineseName
    if ($dependencies.Count -gt 0) {
        $englishDependencies = @($dependencies | ForEach-Object {
            [string] $componentDescriptors[$_].EnglishName
        })
        $chineseDependencies = @($dependencies | ForEach-Object {
            [string] $componentDescriptors[$_].ChineseName
        })
        $englishName += " (requires $($englishDependencies -join ', '))"
        $chineseName += "（依赖 $($chineseDependencies -join '、')）"
    }

    $messageKey = "Component_" + ($id -replace '[^A-Za-z0-9_]', '_')
    $messageLines.Add("english.$messageKey=$($englishName.Replace('"', '""'))")
    $messageLines.Add("chinesesimplified.$messageKey=$($chineseName.Replace('"', '""'))")
    $flags = if ($dependencyProviders.Contains($id)) { "; Flags: checkablealone" } else { "" }
    $installerName = $installerComponentNames[$id]
    $definitionLines.Add(
        "Name: `"$installerName`"; Description: `"{cm:$messageKey}`"; Types: full$flags")
    $fileLines.Add(
        "Source: `"{#ComponentsDir}\$id\*`"; DestDir: `"{app}\components\$id`"; Components: $installerName; Flags: ignoreversion recursesubdirs createallsubdirs")
    $deleteLines.Add(
        "Type: filesandordirs; Name: `"{app}\components\$id`"")
    $selectNewLines.Add(
        "  SelectComponentIfNew('$id', '$installerName', PreviousComponentCatalog);")
}

$dependencyLines = [System.Collections.Generic.List[string]]::new()
foreach ($id in $componentIdList) {
    foreach ($dependency in $componentDependencies[$id]) {
        $dependencyLines.Add(
            "  ResolveComponentDependency('$($installerComponentNames[$id])', '$($installerComponentNames[$dependency])', PreviousSelection);")
    }
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
[System.IO.File]::WriteAllLines(
    (Join-Path $innoRoot "component-dependencies.iss"),
    $dependencyLines,
    $utf8)
