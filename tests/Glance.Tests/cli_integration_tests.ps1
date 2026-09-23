[CmdletBinding()]
param([Parameter(Mandatory)][string] $BuildOutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$cli = Join-Path $BuildOutputDirectory 'Glance.CLI.exe'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$fixture = Join-Path $root ('.tmp\cli-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null

function Invoke-Cli {
    param([string[]] $Arguments, [int] $Expected = 0)
    $output = & $cli @Arguments --json
    $code = $LASTEXITCODE
    if ($code -ne $Expected) { throw "CLI exit $code (expected $Expected): $Arguments`n$output" }
    $result = ($output -join "`n") | ConvertFrom-Json
    if ($result.schema_version -ne 1 -or $result.ok -ne ($Expected -eq 0)) { throw 'Invalid CLI result envelope' }
    return $result
}
function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
}

$setting = 'PathCopy/QuotePath'
$original = $null
$wasStored = $false
try {
    Invoke-Cli -Arguments @('quit') | Out-Null
    Invoke-Cli -Arguments @('status', '--no-start') -Expected 4 | Out-Null
    $help = (& $cli --help --json | ConvertFrom-Json)
    Assert-True ($help.ok -and $help.command -eq 'help') 'JSON help is invalid'
    Invoke-Cli -Arguments @('preview', '--position', '1', '2', '--center-offset', '0', '0', 'unused.txt') -Expected 2 | Out-Null
    Invoke-Cli -Arguments @('preview', '--size', '1', 'bad') -Expected 2 | Out-Null
    Invoke-Cli -Arguments @('status', '--no-start') -Expected 4 | Out-Null
    $cold = [Diagnostics.Stopwatch]::StartNew()
    $status = (Invoke-Cli -Arguments @('status')).data
    $cold.Stop()
    Assert-True ($status.monitors.Count -gt 0) 'No monitors returned'
    $session = $status.session_id
    Invoke-Cli -Arguments @('quit') | Out-Null
    $clients = foreach ($index in 1..4) {
        $client = Start-Process -FilePath $cli -ArgumentList 'status --json' -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $fixture "client-$index.json")
        # Keep a process handle so Windows PowerShell can retrieve the exit code.
        $null = $client.Handle
        $client
    }
    foreach ($client in $clients) {
        if (-not $client.WaitForExit(20000) -or $client.ExitCode -ne 0) {
            $responses = Get-Content -Path (Join-Path $fixture 'client-*.json') -ErrorAction SilentlyContinue
            throw "Concurrent startup failed (exit $($client.ExitCode))`n$($responses -join "`n")"
        }
    }
    $sessions = @(foreach ($index in 1..4) {
        (Get-Content -LiteralPath (Join-Path $fixture "client-$index.json") -Raw | ConvertFrom-Json).data.session_id
    })
    Assert-True (@($sessions | Select-Object -Unique).Count -eq 1) 'Concurrent startup produced multiple sessions'
    $session = $sessions[0]
    $samples = @()
    foreach ($iteration in 1..20) {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Cli -Arguments @('status', '--no-start') | Out-Null
        $timer.Stop()
        $samples += $timer.Elapsed.TotalMilliseconds
    }
    $sorted = @($samples | Sort-Object)
    Write-Host ('CLI cold status: {0:N1} ms; hot median: {1:N1} ms; P95: {2:N1} ms' -f $cold.Elapsed.TotalMilliseconds, $sorted[9], $sorted[18])

    $first = Join-Path $fixture '中文 file.txt'
    $second = Join-Path $fixture 'second.txt'
    Set-Content -LiteralPath $first -Value 'First preview' -Encoding utf8
    Set-Content -LiteralPath $second -Value 'Second preview' -Encoding utf8
    $opened = (Invoke-Cli -Arguments @('preview', $first, $second, '--wait', '--timeout', '20', '--size', '1000', '700', '--position', '100', '100')).data
    Assert-True ($opened.state -eq 'ready' -and $opened.paths.Count -eq 2) 'Multi-path preview not ready'
    Assert-True ($opened.bounds.width -eq 1000 -and $opened.bounds.height -eq 700 -and $opened.bounds.x -eq 100) 'Explicit geometry not preserved'
    $id = $opened.id
    Invoke-Cli -Arguments @('preview', $second, '--size', '1', '1') -Expected 2 | Out-Null
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.generation -eq $opened.generation) 'Invalid preview replaced content'
    Invoke-Cli -Arguments @('window', 'move', '--position', '-50', '100') | Out-Null
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.bounds.x -eq -50) 'Negative position failed'
    Invoke-Cli -Arguments @('window', 'move', '--center-offset', '0', '0', '--monitor', '0') | Out-Null
    $pinned = (Invoke-Cli -Arguments @('window', 'pin', 'on')).data
    Assert-True ($pinned.id -eq $id) 'Pin changed window ID'
    Invoke-Cli -Arguments @('window', 'topmost', 'off', '--id', $id) -Expected 8 | Out-Null
    $windows = (Invoke-Cli -Arguments @('windows')).data.windows
    Assert-True ($windows.Count -eq 2) 'Pin did not create a new dynamic window'
    Invoke-Cli -Arguments @('window', 'resize', '--id', $id, '--size', '1100', '750') | Out-Null
    Invoke-Cli -Arguments @('window', 'pin', 'off', '--id', $id) | Out-Null
    Invoke-Cli -Arguments @('window', 'get', '--id', $id) -Expected 3 | Out-Null

    $delayed = (Invoke-Cli -Arguments @('preview', $first, '--pin', '--wait', '--close-after', '0.3')).data
    Start-Sleep -Milliseconds 700
    Invoke-Cli -Arguments @('window', 'get', '--id', $delayed.id) -Expected 3 | Out-Null
    Invoke-Cli -Arguments @('preview', $first, '--wait', '--close-after', '0.4') | Out-Null
    Invoke-Cli -Arguments @('preview', $second, '--wait') | Out-Null
    Start-Sleep -Milliseconds 700
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.visible) 'Old close timer closed replacement preview'
    Invoke-Cli -Arguments @('preview', (Join-Path $fixture 'missing.txt')) -Expected 3 | Out-Null
    Invoke-Cli -Arguments @('preview', $fixture, '--wait') | Out-Null
    $brokenImage = Join-Path $fixture 'broken.png'
    Set-Content -LiteralPath $brokenImage -Value 'invalid image'
    Invoke-Cli -Arguments @('preview', $brokenImage, '--wait') -Expected 9 | Out-Null

    $allSettings = (Invoke-Cli -Arguments @('settings', 'list')).data.settings
    Assert-True ($allSettings.Count -ge 40) 'Public settings catalog is incomplete'
    Assert-True (@($allSettings | Where-Object key -Match 'LastSuccessfulCheck|RetryAfter|WindowSize').Count -eq 0) 'Private state exposed'
    $original = (Invoke-Cli -Arguments @('settings', 'get', $setting)).data.settings[0].value
    $rawKey = Get-Item 'HKCU:\Software\Glance\PathCopy' -ErrorAction SilentlyContinue
    $wasStored = $null -ne $rawKey -and $rawKey.GetValueNames() -contains 'QuotePath'
    Invoke-Cli -Arguments @('settings', 'set', $setting, 'true') | Out-Null
    Assert-True ((Invoke-Cli -Arguments @('settings', 'get', $setting)).data.settings[0].value) 'Setting change was not applied'
    Invoke-Cli -Arguments @('settings', 'reset', $setting) | Out-Null
    Assert-True (-not (Invoke-Cli -Arguments @('settings', 'get', $setting)).data.settings[0].value) 'Setting reset failed'
    Invoke-Cli -Arguments @('settings', 'set', 'Window/DefaultWidth', '0') -Expected 2 | Out-Null
    Invoke-Cli -Arguments @('settings', 'get', 'Update/RetryAfter') -Expected 3 | Out-Null

    Invoke-Cli -Arguments @('quit') | Out-Null
    Invoke-Cli -Arguments @('status', '--no-start') -Expected 4 | Out-Null
    $restarted = (Invoke-Cli -Arguments @('status')).data
    Assert-True ($restarted.session_id -ne $session) 'Session identifier reused'
    Invoke-Cli -Arguments @('window', 'get', '--id', $id) -Expected 3 | Out-Null
    Write-Host 'CLI integration tests passed'
}
finally {
    if ($null -ne $original) {
        if ($wasStored) { Invoke-Cli -Arguments @('settings', 'set', $setting, $original.ToString().ToLowerInvariant()) | Out-Null }
        else { Invoke-Cli -Arguments @('settings', 'reset', $setting) | Out-Null }
    }
    Invoke-Cli -Arguments @('quit') | Out-Null
    $resolved = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath((Join-Path $root '.tmp')) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid fixture cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
