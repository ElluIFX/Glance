Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-GlanceRepositoryRoot {
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
}

function Get-GlanceMSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "Visual Studio Installer component vswhere.exe was not found."
    }

    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
        throw "MSBuild was not found. Install the Desktop development with C++ workload."
    }

    return $msbuild
}

function Get-GlanceVersion {
    $versionHeader = Join-Path (Get-GlanceRepositoryRoot) "src\version.h"
    $content = Get-Content -LiteralPath $versionHeader -Raw
    $versionMatch = [regex]::Match($content, '#define\s+GLANCE_VERSION_STRING\s+"([^"]+)"')
    $fileVersionMatch = [regex]::Match($content, '#define\s+GLANCE_FILE_VERSION_STRING\s+"([^"]+)"')
    if (-not $versionMatch.Success -or -not $fileVersionMatch.Success) {
        throw "Unable to read the version from src/version.h."
    }

    return [pscustomobject]@{
        Version = $versionMatch.Groups[1].Value
        FileVersion = $fileVersionMatch.Groups[1].Value
    }
}

function Resolve-GlanceWorkspacePath {
    param(
        [Parameter(Mandatory)]
        [string] $Path
    )

    $root = (Get-GlanceRepositoryRoot).TrimEnd('\', '/')
    $resolved = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    if (-not $resolved.StartsWith("$root\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must remain inside the repository: $resolved"
    }

    return $resolved
}

function Remove-GlanceWorkspaceItem {
    param(
        [Parameter(Mandatory)]
        [string] $Path
    )

    $resolved = Resolve-GlanceWorkspacePath -Path $Path
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

function Stop-GlanceProcesses {
    $processes = Get-Process Glance, Glance.Core, Glance.OfficeHost -ErrorAction SilentlyContinue
    if (-not $processes) {
        return
    }

    $processes | Stop-Process -Force
    $processes | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}
