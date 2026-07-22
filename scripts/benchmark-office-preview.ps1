[CmdletBinding()]
param(
    [string] $InputPath = (Join-Path $env:USERPROFILE "Desktop\test.docx"),
    [ValidateRange(1, 10)]
    [int] $Iterations = 2,
    [ValidateRange(30, 600)]
    [int] $TimeoutSeconds = 180,
    [ValidateSet("baseline-full", "screen-full", "progressive", "paged-session", "page-emf", "emf-progressive", "warm-application")]
    [string[]] $Scenarios = @(
        "baseline-full",
        "screen-full",
        "progressive",
        "paged-session",
        "page-emf",
        "emf-progressive",
        "warm-application"
    ),
    [string] $OutputDirectory,
    [switch] $Worker,
    [string] $WorkerScenario,
    [int] $WorkerIteration,
    [string] $WorkerResultPath,
    [string] $WorkerWordPidPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Release-ComObject {
    param([object] $Value)

    if ($null -ne $Value -and [Runtime.InteropServices.Marshal]::IsComObject($Value)) {
        [void] [Runtime.InteropServices.Marshal]::FinalReleaseComObject($Value)
    }
}

function Export-WordPdf {
    param(
        [Parameter(Mandatory)] [object] $Document,
        [Parameter(Mandatory)] [string] $OutputPath,
        [Parameter(Mandatory)] [string] $Name,
        [switch] $Baseline,
        [int] $Range = 0,
        [int] $From = 1,
        [int] $To = 1
    )

    Remove-Item -LiteralPath $OutputPath -Force -ErrorAction SilentlyContinue
    $timer = [Diagnostics.Stopwatch]::StartNew()
    if ($Baseline) {
        $Document.ExportAsFixedFormat($OutputPath, 17)
    } else {
        $Document.ExportAsFixedFormat(
            $OutputPath,
            17,
            $false,
            1,
            $Range,
            $From,
            $To,
            0,
            $false,
            $false,
            0,
            $false,
            $true,
            $false
        )
    }
    $timer.Stop()
    if (-not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
        throw "Word did not produce the expected PDF: $OutputPath"
    }

    return [ordered]@{
        Name = $Name
        DurationMilliseconds = $timer.Elapsed.TotalMilliseconds
        Bytes = (Get-Item -LiteralPath $OutputPath).Length
        Path = $OutputPath
    }
}

function Export-WordPageEmf {
    param(
        [Parameter(Mandatory)] [object] $Pages,
        [Parameter(Mandatory)] [int] $PageNumber,
        [Parameter(Mandatory)] [string] $OutputPath
    )

    $page = $null
    try {
        Remove-Item -LiteralPath $OutputPath -Force -ErrorAction SilentlyContinue
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $page = $Pages.Item($PageNumber)
        [byte[]] $bytes = $page.EnhMetaFileBits
        [IO.File]::WriteAllBytes($OutputPath, $bytes)
        $timer.Stop()
        return [ordered]@{
            Name = "page-$PageNumber-emf"
            DurationMilliseconds = $timer.Elapsed.TotalMilliseconds
            Bytes = $bytes.Length
            Path = $OutputPath
        }
    } finally {
        Release-ComObject $page
    }
}

function Open-WordDocument {
    param(
        [Parameter(Mandatory)] [object] $Application,
        [Parameter(Mandatory)] [string] $Path
    )

    $documents = $Application.Documents
    try {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $document = $documents.Open($Path, $false, $true)
        $timer.Stop()
        return @($document, $timer.Elapsed.TotalMilliseconds)
    } finally {
        Release-ComObject $documents
    }
}

function Invoke-Worker {
    $result = [ordered]@{
        Scenario = $WorkerScenario
        Iteration = $WorkerIteration
        Success = $false
        Error = $null
        StageMilliseconds = $null
        ComStartupMilliseconds = $null
        OpenMilliseconds = $null
        ReopenMilliseconds = $null
        PaginationMilliseconds = $null
        PageCount = $null
        FirstReadyMilliseconds = $null
        FullReadyMilliseconds = $null
        WarmCycleMilliseconds = $null
        TotalMilliseconds = $null
        WordProcessId = $null
        Exports = @()
    }
    $overall = [Diagnostics.Stopwatch]::StartNew()
    $application = $null
    $document = $null
    $activeWindow = $null
    $panes = $null
    $pane = $null
    $pages = $null

    try {
        if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
            throw "Input document was not found: $InputPath"
        }
        $caseDirectory = Split-Path -Parent $WorkerResultPath
        New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
        $stagedPath = Join-Path $caseDirectory ("source" + [IO.Path]::GetExtension($InputPath))

        $stageTimer = [Diagnostics.Stopwatch]::StartNew()
        Copy-Item -LiteralPath $InputPath -Destination $stagedPath -Force
        $stageTimer.Stop()
        $result.StageMilliseconds = $stageTimer.Elapsed.TotalMilliseconds

        $wordProcessIdsBefore = @(
            Get-Process -Name WINWORD -ErrorAction SilentlyContinue |
                Select-Object -ExpandProperty Id
        )
        $startupTimer = [Diagnostics.Stopwatch]::StartNew()
        $application = New-Object -ComObject Word.Application
        $startupTimer.Stop()
        $result.ComStartupMilliseconds = $startupTimer.Elapsed.TotalMilliseconds
        $application.Visible = $false
        $application.DisplayAlerts = 0
        $application.ScreenUpdating = $false
        try {
            $application.AutomationSecurity = 3
        } catch {
        }

        $wordProcess = Get-Process -Name WINWORD -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -notin $wordProcessIdsBefore } |
            Sort-Object StartTime -Descending |
            Select-Object -First 1
        if ($wordProcess) {
            $result.WordProcessId = $wordProcess.Id
            Set-Content -LiteralPath $WorkerWordPidPath -Value $wordProcess.Id -Encoding ascii
        }

        $opened = Open-WordDocument -Application $application -Path $stagedPath
        $document = $opened[0]
        $result.OpenMilliseconds = $opened[1]
        $exports = [Collections.Generic.List[object]]::new()

        switch ($WorkerScenario) {
            "baseline-full" {
                $export = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "baseline-full.pdf") `
                    -Name "full" `
                    -Baseline
                $exports.Add($export)
                $result.FirstReadyMilliseconds = $overall.Elapsed.TotalMilliseconds
                $result.FullReadyMilliseconds = $result.FirstReadyMilliseconds
            }
            "screen-full" {
                $export = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "screen-full.pdf") `
                    -Name "full"
                $exports.Add($export)
                $result.FirstReadyMilliseconds = $overall.Elapsed.TotalMilliseconds
                $result.FullReadyMilliseconds = $result.FirstReadyMilliseconds
            }
            "progressive" {
                $first = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "page-1.pdf") `
                    -Name "page-1" `
                    -Range 3 `
                    -From 1 `
                    -To 1
                $exports.Add($first)
                $result.FirstReadyMilliseconds = $overall.Elapsed.TotalMilliseconds

                $full = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "progressive-full.pdf") `
                    -Name "full"
                $exports.Add($full)
                $result.FullReadyMilliseconds = $overall.Elapsed.TotalMilliseconds
            }
            "paged-session" {
                $first = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "page-1.pdf") `
                    -Name "page-1" `
                    -Range 3 `
                    -From 1 `
                    -To 1
                $exports.Add($first)
                $result.FirstReadyMilliseconds = $overall.Elapsed.TotalMilliseconds

                $paginationTimer = [Diagnostics.Stopwatch]::StartNew()
                $pageCount = [int] $document.ComputeStatistics(2)
                $paginationTimer.Stop()
                $result.PaginationMilliseconds = $paginationTimer.Elapsed.TotalMilliseconds
                $result.PageCount = $pageCount

                foreach ($page in @(2, [Math]::Max(1, [Math]::Min(50, $pageCount)))) {
                    if ($page -gt $pageCount -or $page -eq 1) {
                        continue
                    }
                    $export = Export-WordPdf `
                        -Document $document `
                        -OutputPath (Join-Path $caseDirectory "page-$page.pdf") `
                        -Name "page-$page" `
                        -Range 3 `
                        -From $page `
                        -To $page
                    $exports.Add($export)
                }
            }
            "page-emf" {
                $paginationTimer = [Diagnostics.Stopwatch]::StartNew()
                $activeWindow = $document.ActiveWindow
                $panes = $activeWindow.Panes
                $pane = $panes.Item(1)
                $pages = $pane.Pages
                $paginationTimer.Stop()

                $first = Export-WordPageEmf `
                    -Pages $pages `
                    -PageNumber 1 `
                    -OutputPath (Join-Path $caseDirectory "page-1.emf")
                $exports.Add($first)
                $result.FirstReadyMilliseconds = $overall.Elapsed.TotalMilliseconds

                $countTimer = [Diagnostics.Stopwatch]::StartNew()
                $pageCount = [int] $pages.Count
                $countTimer.Stop()
                $result.PaginationMilliseconds =
                    $paginationTimer.Elapsed.TotalMilliseconds + $countTimer.Elapsed.TotalMilliseconds
                $result.PageCount = $pageCount
                foreach ($pageNumber in @(2, [Math]::Max(1, [Math]::Min(50, $pageCount)))) {
                    if ($pageNumber -gt $pageCount -or $pageNumber -eq 1) {
                        continue
                    }
                    $export = Export-WordPageEmf `
                        -Pages $pages `
                        -PageNumber $pageNumber `
                        -OutputPath (Join-Path $caseDirectory "page-$pageNumber.emf")
                    $exports.Add($export)
                }
            }
            "emf-progressive" {
                $activeWindow = $document.ActiveWindow
                $panes = $activeWindow.Panes
                $pane = $panes.Item(1)
                $pages = $pane.Pages
                $first = Export-WordPageEmf `
                    -Pages $pages `
                    -PageNumber 1 `
                    -OutputPath (Join-Path $caseDirectory "page-1.emf")
                $exports.Add($first)
                $result.FirstReadyMilliseconds = $overall.Elapsed.TotalMilliseconds

                $full = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "emf-progressive-full.pdf") `
                    -Name "full"
                $exports.Add($full)
                $result.FullReadyMilliseconds = $overall.Elapsed.TotalMilliseconds
            }
            "warm-application" {
                $cold = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "cold-full.pdf") `
                    -Name "cold-full"
                $exports.Add($cold)
                $result.FirstReadyMilliseconds = $overall.Elapsed.TotalMilliseconds
                $result.FullReadyMilliseconds = $result.FirstReadyMilliseconds

                $document.Close($false)
                Release-ComObject $document
                $document = $null
                $warmCycle = [Diagnostics.Stopwatch]::StartNew()
                $reopened = Open-WordDocument -Application $application -Path $stagedPath
                $document = $reopened[0]
                $result.ReopenMilliseconds = $reopened[1]
                $warm = Export-WordPdf `
                    -Document $document `
                    -OutputPath (Join-Path $caseDirectory "warm-full.pdf") `
                    -Name "warm-full"
                $exports.Add($warm)
                $warmCycle.Stop()
                $result.WarmCycleMilliseconds = $warmCycle.Elapsed.TotalMilliseconds
            }
            default {
                throw "Unknown worker scenario: $WorkerScenario"
            }
        }

        $result.Exports = @($exports)
        $result.Success = $true
    } catch {
        $result.Error = $_.Exception.ToString()
    } finally {
        Release-ComObject $pages
        Release-ComObject $pane
        Release-ComObject $panes
        Release-ComObject $activeWindow
        if ($null -ne $document) {
            try {
                $document.Close($false)
            } catch {
            }
            Release-ComObject $document
        }
        if ($null -ne $application) {
            try {
                $application.Quit($false)
            } catch {
            }
            Release-ComObject $application
        }
        [GC]::Collect()
        [GC]::WaitForPendingFinalizers()
        $overall.Stop()
        $result.TotalMilliseconds = $overall.Elapsed.TotalMilliseconds
        $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $WorkerResultPath -Encoding utf8
    }

    if (-not $result.Success) {
        exit 1
    }
}

function Stop-WorkerProcess {
    param(
        [Parameter(Mandatory)] [Diagnostics.Process] $Process,
        [Parameter(Mandatory)] [string] $WordPidPath
    )

    if (Test-Path -LiteralPath $WordPidPath -PathType Leaf) {
        $wordProcessId = Get-Content -LiteralPath $WordPidPath -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($wordProcessId -as [int]) {
            Stop-Process -Id ([int] $wordProcessId) -Force -ErrorAction SilentlyContinue
        }
    }
    try {
        $Process.Kill($true)
    } catch {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
    $Process.WaitForExit(5000) | Out-Null
}

function Invoke-BenchmarkCase {
    param(
        [Parameter(Mandatory)] [string] $Scenario,
        [Parameter(Mandatory)] [int] $Iteration,
        [Parameter(Mandatory)] [string] $RootDirectory
    )

    $caseDirectory = Join-Path $RootDirectory ("{0}-{1}" -f $Iteration, $Scenario)
    New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
    $resultPath = Join-Path $caseDirectory "result.json"
    $wordPidPath = Join-Path $caseDirectory "word.pid"
    $processPath = (Get-Process -Id $PID).Path
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $processPath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy", "Bypass",
        "-File", $PSCommandPath,
        "-Worker",
        "-InputPath", $InputPath,
        "-WorkerScenario", $Scenario,
        "-WorkerIteration", $Iteration,
        "-WorkerResultPath", $resultPath,
        "-WorkerWordPidPath", $wordPidPath
    )) {
        $startInfo.ArgumentList.Add([string] $argument)
    }

    $process = [Diagnostics.Process]::Start($startInfo)
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-WorkerProcess -Process $process -WordPidPath $wordPidPath
        return [pscustomobject]@{
            Scenario = $Scenario
            Iteration = $Iteration
            Success = $false
            Error = "Timed out after $TimeoutSeconds seconds."
            TotalMilliseconds = $TimeoutSeconds * 1000
        }
    }

    $standardOutput = $process.StandardOutput.ReadToEnd()
    $standardError = $process.StandardError.ReadToEnd()
    if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        return [pscustomobject]@{
            Scenario = $Scenario
            Iteration = $Iteration
            Success = $false
            Error = "Worker exited with code $($process.ExitCode). $standardOutput $standardError".Trim()
            TotalMilliseconds = $null
        }
    }
    return Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
}

if ($Worker) {
    Invoke-Worker
    exit 0
}

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$InputPath = $resolvedInput
if ([IO.Path]::GetExtension($InputPath) -notin @(".doc", ".docx")) {
    throw "This benchmark currently supports Word documents only."
}

if (-not $OutputDirectory) {
    $repositoryRoot = Split-Path -Parent $PSScriptRoot
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path $repositoryRoot ".codex\office-benchmark\$timestamp"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$results = [Collections.Generic.List[object]]::new()
for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
    $orderedScenarios = if (($iteration % 2) -eq 0) {
        @($Scenarios)[($Scenarios.Count - 1)..0]
    } else {
        @($Scenarios)
    }
    foreach ($scenario in $orderedScenarios) {
        Write-Host "Running $scenario, iteration $iteration..."
        $caseResult = Invoke-BenchmarkCase `
            -Scenario $scenario `
            -Iteration $iteration `
            -RootDirectory $OutputDirectory
        $results.Add($caseResult)
        Start-Sleep -Seconds 1
    }
}

$results | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory "results.json") -Encoding utf8
function Get-RoundedResultValue {
    param(
        [Parameter(Mandatory)] [object] $Result,
        [Parameter(Mandatory)] [string] $Name
    )

    $property = $Result.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return $null
    }
    return [Math]::Round([double] $property.Value, 1)
}

$rows = foreach ($result in $results) {
    $outputBytes = $null
    $exportsProperty = $result.PSObject.Properties["Exports"]
    if ($null -ne $exportsProperty -and $null -ne $exportsProperty.Value) {
        $exportItems = @($exportsProperty.Value) | Where-Object { $null -ne $_ }
        if ($exportItems.Count -gt 0) {
            $outputBytes = ($exportItems | Measure-Object -Property Bytes -Sum).Sum
        }
    }
    [pscustomobject]@{
        Scenario = $result.Scenario
        Iteration = $result.Iteration
        Success = $result.Success
        ComStartupMs = Get-RoundedResultValue -Result $result -Name "ComStartupMilliseconds"
        OpenMs = Get-RoundedResultValue -Result $result -Name "OpenMilliseconds"
        FirstReadyMs = Get-RoundedResultValue -Result $result -Name "FirstReadyMilliseconds"
        FullReadyMs = Get-RoundedResultValue -Result $result -Name "FullReadyMilliseconds"
        WarmCycleMs = Get-RoundedResultValue -Result $result -Name "WarmCycleMilliseconds"
        TotalMs = Get-RoundedResultValue -Result $result -Name "TotalMilliseconds"
        OutputMiB = if ($null -ne $outputBytes) { [Math]::Round($outputBytes / 1MB, 2) } else { $null }
        Error = $result.Error
    }
}
$rows | Export-Csv -LiteralPath (Join-Path $OutputDirectory "results.csv") -NoTypeInformation
$rows | Format-Table -AutoSize
Write-Host "Benchmark results: $OutputDirectory"
