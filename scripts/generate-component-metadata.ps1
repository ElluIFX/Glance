[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $RepositoryRoot,

    [Parameter(Mandatory)]
    [string] $OutputDirectory,

    [string] $ComponentId
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function ConvertTo-CppWideLiteral {
    param([Parameter(Mandatory)] [string] $Value)

    $escaped = $Value.Replace('\', '\\').Replace('"', '\"')
    return "L`"$escaped`""
}

function Read-ComponentDescriptors {
    param([Parameter(Mandatory)] [string] $Root)

    $componentRoot = Join-Path $Root "src\Glance.Components"
    $manifests = @(Get-ChildItem -LiteralPath $componentRoot -Directory |
        ForEach-Object { Join-Path $_.FullName "component.json" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Sort-Object)
    if (-not $manifests) {
        throw "No component manifests were found under '$componentRoot'."
    }

    $descriptors = foreach ($manifest in $manifests) {
        $descriptor = Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json
        foreach ($property in @(
            "schema_version",
            "id",
            "display_name_resource",
            "abi_version",
            "architecture",
            "entry_point",
            "output_kind",
            "payload_files",
            "extensions"
        )) {
            if ($null -eq $descriptor.PSObject.Properties[$property]) {
                throw "Component manifest '$manifest' is missing '$property'."
            }
        }
        if ([int] $descriptor.schema_version -ne 1) {
            throw "Component manifest '$manifest' uses an unsupported schema."
        }
        if ([string] $descriptor.id -notmatch '^[a-z][a-z0-9-]*$') {
            throw "Component manifest '$manifest' has an invalid id."
        }
        if ([int] $descriptor.abi_version -ne 2) {
            throw "Component manifest '$manifest' uses an unsupported ABI."
        }
        if ([string] $descriptor.architecture -ne "x64") {
            throw "Component manifest '$manifest' does not target x64."
        }
        if ([string] $descriptor.output_kind -ne "pdf_file") {
            throw "Component manifest '$manifest' has an unsupported output kind."
        }
        $descriptor
    }

    $duplicate = $descriptors |
        Group-Object -Property id |
        Where-Object Count -gt 1 |
        Select-Object -First 1
    if ($duplicate) {
        throw "Component id '$($duplicate.Name)' is duplicated."
    }
    return @($descriptors)
}

function Write-Utf8File {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Content
    )

    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [System.IO.File]::WriteAllText(
        $Path,
        $Content,
        [System.Text.UTF8Encoding]::new($false))
}

$resolvedRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$descriptors = @(Read-ComponentDescriptors -Root $resolvedRoot)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

if ($ComponentId) {
    $descriptor = $descriptors |
        Where-Object { [string] $_.id -eq $ComponentId } |
        Select-Object -First 1
    if (-not $descriptor) {
        throw "Component '$ComponentId' was not found."
    }

    $content = @"
#pragma once

#define GLANCE_COMPONENT_ID_WSTRING $(ConvertTo-CppWideLiteral ([string] $descriptor.id))
"@
    Write-Utf8File `
        -Path (Join-Path $OutputDirectory "component_metadata.generated.h") `
        -Content $content
    exit 0
}

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("#pragma once")
$lines.Add("")
$lines.Add("#include `"component_catalog.h`"")
$lines.Add("")
$lines.Add("#include <array>")
$lines.Add("")
$lines.Add("namespace glance::app::generated")
$lines.Add("{")
foreach ($descriptor in $descriptors) {
    $symbol = ([string] $descriptor.id).Replace("-", "_")
    $extensions = @(@($descriptor.extensions) | ForEach-Object {
        ConvertTo-CppWideLiteral ([string] $_)
    })
    $lines.Add("    inline constexpr std::array<std::wstring_view, $($extensions.Count)> ${symbol}_extensions{")
    for ($index = 0; $index -lt $extensions.Count; ++$index) {
        $suffix = if ($index + 1 -lt $extensions.Count) { "," } else { "" }
        $lines.Add("        $($extensions[$index])$suffix")
    }
    $lines.Add("    };")
    $lines.Add("")
}
$lines.Add("    inline constexpr std::array supported_components{")
for ($index = 0; $index -lt $descriptors.Count; ++$index) {
    $descriptor = $descriptors[$index]
    $symbol = ([string] $descriptor.id).Replace("-", "_")
    $suffix = if ($index + 1 -lt $descriptors.Count) { "," } else { "" }
    $lines.Add("        SupportedComponent{")
    $lines.Add("            $(ConvertTo-CppWideLiteral ([string] $descriptor.id)),")
    $lines.Add("            $(ConvertTo-CppWideLiteral ([string] $descriptor.display_name_resource)),")
    $lines.Add("            $([int] $descriptor.abi_version)U,")
    $lines.Add("            $(ConvertTo-CppWideLiteral ([string] $descriptor.architecture)),")
    $lines.Add("            $(ConvertTo-CppWideLiteral ([string] $descriptor.entry_point)),")
    $lines.Add("            glance::contracts::components::PreviewOutputKind::pdf_file,")
    $lines.Add("            ${symbol}_extensions }$suffix")
}
$lines.Add("    };")
$lines.Add("}")

Write-Utf8File `
    -Path (Join-Path $OutputDirectory "supported_components.generated.h") `
    -Content (($lines -join [Environment]::NewLine) + [Environment]::NewLine)
