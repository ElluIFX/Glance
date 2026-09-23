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
    $boundedByRunner = $env:CI -and $Arguments[0] -in @('preview', 'window') -and '--timeout' -notin $Arguments
    if ($env:CI) { Write-Host "CLI: $($Arguments -join ' ')" }
    if ($boundedByRunner) { $Arguments += @('--timeout', '20') }
    $output = & $cli @Arguments --json
    $code = $LASTEXITCODE
    if ($code -ne $Expected) { throw "CLI exit $code (expected $Expected): $Arguments`n$output" }
    $result = ($output -join "`n") | ConvertFrom-Json
    if ($result.schema_version -ne 1 -or $result.ok -ne ($Expected -eq 0)) { throw 'Invalid CLI result envelope' }
    if ($boundedByRunner -and $result.ok -and $result.data.PSObject.Properties['wait_completed'] -and -not $result.data.wait_completed) {
        throw "CLI readiness timed out: $Arguments`n$output"
    }
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
    foreach ($topic in @(@('preview', '-h'), @('window', 'set', '-h'), @('help', 'window', 'resize'))) {
        $page = (& $cli @topic --json | ConvertFrom-Json)
        Assert-True ($page.ok -and $page.data.text.StartsWith('usage:')) 'Command help is invalid'
    }
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
    $opened = (Invoke-Cli -Arguments @('preview', $first, $second, '--timeout', '20', '--size', '1000', '700', '--position', '100', '100')).data
    Assert-True ($opened.state -eq 'ready' -and $opened.paths.Count -eq 2) 'Multi-path preview not ready'
    Assert-True ($opened.bounds.width -eq 1000 -and $opened.bounds.height -eq 700 -and $opened.bounds.x -eq 100) 'Explicit geometry not preserved'
    $id = $opened.id
    Assert-True $opened.wait_completed 'Default preview did not wait for readiness'
    $next = (Invoke-Cli -Arguments @('window', 'next')).data
    Assert-True ($next.current_index -eq 1) 'Next file did not navigate'
    Invoke-Cli -Arguments @('window', 'next') -Expected 3 | Out-Null
    Invoke-Cli -Arguments @('window', 'previous') | Out-Null
    $opened = (Invoke-Cli -Arguments @('window', 'get')).data
    Invoke-Cli -Arguments @('window', 'line', '1') | Out-Null
    Invoke-Cli -Arguments @('window', 'line', '1000000') -Expected 3 | Out-Null
    Invoke-Cli -Arguments @('window', 'page', '1') -Expected 8 | Out-Null
    Assert-True ($id -match '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$') 'Window ID is not a UUID'
    Assert-True (-not $opened.topmost) 'Preview should default to non-topmost'
    Invoke-Cli -Arguments @('window', 'get', '--id', '0') -Expected 2 | Out-Null
    Invoke-Cli -Arguments @('preview', $second, '--size', '1', '1') -Expected 2 | Out-Null
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.generation -eq $opened.generation) 'Invalid preview replaced content'
    Invoke-Cli -Arguments @('window', 'move', '--position', '-50', '100') | Out-Null
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.bounds.x -eq -50) 'Negative position failed'
    Invoke-Cli -Arguments @('window', 'move', '--center-offset', '0', '0', '--monitor', '0') | Out-Null
    $pinned = (Invoke-Cli -Arguments @('window', 'pin', 'on')).data
    Assert-True ($pinned.id -eq $id) 'Pin changed window ID'
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.id -eq $id) 'Pin changed the default target'
    Invoke-Cli -Arguments @('window', 'topmost', 'off', '--id', $id) -Expected 8 | Out-Null
    $windows = (Invoke-Cli -Arguments @('windows')).data.windows
    Assert-True ($windows.Count -eq 2) 'Pin did not create a new dynamic window'
    $resized = (Invoke-Cli -Arguments @('window', 'resize', '--id', $id, '--size', '900', '650')).data
    Assert-True ($resized.bounds.width -eq 900 -and $resized.bounds.height -eq 650) "Resize failed: $($resized | ConvertTo-Json -Depth 6 -Compress)"
    $replaced = (Invoke-Cli -Arguments @('window', 'set', $second)).data
    Assert-True ($replaced.id -eq $id -and $replaced.paths[0] -eq $second) 'Set changed the window ID or failed to replace the file'
    Assert-True ($replaced.bounds.width -eq $resized.bounds.width -and $replaced.bounds.height -eq $resized.bounds.height -and $replaced.pinned -and $replaced.topmost) "Set changed geometry or pinning: $($replaced | ConvertTo-Json -Depth 6 -Compress)"
    Invoke-Cli -Arguments @('window', 'set', (Join-Path $fixture 'missing.txt')) -Expected 3 | Out-Null
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.generation -eq $replaced.generation) 'Invalid set replaced content'
    $other = (Invoke-Cli -Arguments @('preview', $first, '--topmost')).data
    Assert-True ($other.id -ne $id -and $other.topmost) 'New window UUID or topmost flag failed'
    $targeted = (Invoke-Cli -Arguments @('window', 'set', $first, $second, '--id', $id.ToUpperInvariant())).data
    Assert-True ($targeted.id -eq $id -and $targeted.paths.Count -eq 2) 'Explicit UUID set failed'
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.id -eq $other.id) 'Explicit target changed the default UUID'
    Invoke-Cli -Arguments @('window', 'close') | Out-Null
    Invoke-Cli -Arguments @('window', 'resize', '--size', '1100', '750') -Expected 8 | Out-Null
    Invoke-Cli -Arguments @('window', 'pin', 'off', '--id', $id) | Out-Null
    $defaultPinned = (Invoke-Cli -Arguments @('preview', $second, '--pin')).data
    Invoke-Cli -Arguments @('window', 'pin', 'off') | Out-Null
    Invoke-Cli -Arguments @('window', 'get') -Expected 3 | Out-Null
    Invoke-Cli -Arguments @('window', 'get', '--id', $id) -Expected 3 | Out-Null

    $delayed = (Invoke-Cli -Arguments @('preview', $first, '--pin', '--close-after', '0.3')).data
    Start-Sleep -Milliseconds 700
    Invoke-Cli -Arguments @('window', 'get', '--id', $delayed.id) -Expected 3 | Out-Null
    Invoke-Cli -Arguments @('preview', $first, '--close-after', '0.4') | Out-Null
    Invoke-Cli -Arguments @('preview', $second) | Out-Null
    Start-Sleep -Milliseconds 700
    Assert-True ((Invoke-Cli -Arguments @('window', 'get')).data.visible) 'Old close timer closed replacement preview'
    Invoke-Cli -Arguments @('preview', (Join-Path $fixture 'missing.txt')) -Expected 3 | Out-Null
    Invoke-Cli -Arguments @('preview', $fixture) | Out-Null
    $brokenImage = Join-Path $fixture 'broken.png'
    Set-Content -LiteralPath $brokenImage -Value 'invalid image'
    Invoke-Cli -Arguments @('preview', $brokenImage) -Expected 9 | Out-Null

    $closed = (Invoke-Cli -Arguments @('preview', $first, '--pin', '--wait', '--close-after', '0.2')).data
    Assert-True ($closed.state -eq 'closed' -and $closed.wait_completed) 'Wait did not return the closed snapshot'
    $bounded = (Invoke-Cli -Arguments @('preview', $first, '--wait', '--timeout', '0.1')).data
    Assert-True ($bounded.visible -and -not $bounded.wait_completed) 'Bounded close wait did not return current state'
    $immediate = (Invoke-Cli -Arguments @('preview', $first, '--wait', '--timeout', '0')).data
    Assert-True (-not $immediate.wait_completed) 'Timeout zero waited for close'
    $textOutput = & $cli window get
    Assert-True (($textOutput -join "`n") -match 'bounds.width:') 'Text output is incomplete'

    # Six concurrent waiters must leave the four App workers available.
    $waiters = foreach ($index in 1..6) {
        $process = Start-Process -FilePath $cli -ArgumentList "window get --id $($immediate.id) --wait --json" -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $fixture "wait-$index.json")
        $null = $process.Handle
        $process
    }
    Start-Sleep -Milliseconds 500
    Invoke-Cli -Arguments @('window', 'resize', '--size', '1000', '700') | Out-Null
    Invoke-Cli -Arguments @('window', 'close') | Out-Null
    foreach ($process in $waiters) {
        Assert-True ($process.WaitForExit(10000) -and $process.ExitCode -eq 0) 'Concurrent close wait failed'
    }

    $info = [Diagnostics.ProcessStartInfo]::new($cli)
    $info.Arguments = 'preview - --raw --name "binary sample.dat" --timeout 20 --json'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $inputProcess = [Diagnostics.Process]::Start($info)
    $bytes = [byte[]](0..255) * 8192
    $inputProcess.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $inputProcess.StandardInput.Close()
    $pipeResult = $inputProcess.StandardOutput.ReadToEnd() | ConvertFrom-Json
    Assert-True ($inputProcess.WaitForExit(15000) -and $inputProcess.ExitCode -eq 0) 'Raw pipe preview failed'
    $temporary = $pipeResult.data.paths[0]
    Assert-True ((Get-Item -LiteralPath $temporary).Length -eq $bytes.Length) 'Pipe bytes were changed'
    $actualBytes = [IO.File]::ReadAllBytes($temporary)
    Assert-True ([Convert]::ToBase64String($actualBytes) -eq [Convert]::ToBase64String($bytes)) 'Raw pipe content differs'
    Invoke-Cli -Arguments @('window', 'close', '--id', $pipeResult.data.id) | Out-Null
    Assert-True (-not (Test-Path -LiteralPath $temporary)) 'Temporary input was retained after close'
    foreach ($sample in @(
        @{ Name = 'empty.txt'; Raw = ''; Bytes = [byte[]]::new(0) },
        @{ Name = 'unicode.txt'; Raw = ''; Bytes = [Text.Encoding]::UTF8.GetBytes('中文内容') },
        @{ Name = 'pixel.gif'; Raw = '--raw'; Bytes = [Convert]::FromBase64String('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7') }
    )) {
        $info.Arguments = "preview - $($sample.Raw) --name $($sample.Name) --timeout 20 --json"
        $inputProcess = [Diagnostics.Process]::Start($info)
        $inputProcess.StandardInput.BaseStream.Write($sample.Bytes, 0, $sample.Bytes.Length)
        $inputProcess.StandardInput.Close()
        $pipeResult = $inputProcess.StandardOutput.ReadToEnd() | ConvertFrom-Json
        Assert-True ($inputProcess.WaitForExit(15000) -and $inputProcess.ExitCode -eq 0 -and -not $pipeResult.data.fallback) 'Typed stdin preview failed'
        Invoke-Cli -Arguments @('window', 'close') | Out-Null
        $inputProcess.Dispose()
    }

    $longText = Join-Path $fixture 'long.txt'
    [IO.File]::WriteAllLines($longText, [string[]](1..40000 | ForEach-Object { "Line $_ with enough content to exceed one chunk" }))
    Invoke-Cli -Arguments @('preview', $longText) | Out-Null
    $located = (Invoke-Cli -Arguments @('window', 'line', '35000')).data
    Assert-True ($located.line -eq 35000 -and $located.wait_completed) 'Incremental line navigation failed'
    Invoke-Cli -Arguments @('window', 'line', '90000') -Expected 3 | Out-Null

    $wave = Join-Path $fixture 'silence.wav'
    $writer = [IO.BinaryWriter]::new([IO.File]::Create($wave))
    try {
        $dataSize = 8000 * 2 * 5
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([int](36 + $dataSize))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([int]16)
        $writer.Write([short]1); $writer.Write([short]1)
        $writer.Write([int]8000); $writer.Write([int]16000)
        $writer.Write([short]2); $writer.Write([short]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([int]$dataSize)
        $writer.Write([byte[]]::new($dataSize))
    } finally { $writer.Dispose() }
    Invoke-Cli -Arguments @('preview', $wave) | Out-Null
    Invoke-Cli -Arguments @('window', 'pause') | Out-Null
    $volume = (Invoke-Cli -Arguments @('window', 'volume', '37')).data
    Assert-True ([Math]::Abs($volume.volume - 37) -lt 0.01) 'Media volume failed'
    Assert-True ((Invoke-Cli -Arguments @('window', 'mute', 'on')).data.muted) 'Media mute failed'
    $seek = (Invoke-Cli -Arguments @('window', 'seek', '00:00:02')).data
    Assert-True ([Math]::Abs($seek.position - 2) -lt 0.2) 'Media seek failed'
    Invoke-Cli -Arguments @('window', 'seek', '99999') -Expected 2 | Out-Null
    Invoke-Cli -Arguments @('window', 'play') | Out-Null
    Invoke-Cli -Arguments @('window', 'pause') | Out-Null

    $pdf = Join-Path $fixture 'pages.pdf'
    $objects = @('<< /Type /Catalog /Pages 2 0 R >>', '<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>',
        '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>',
        '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>')
    $pdfText = "%PDF-1.4`n"
    $offsets = @()
    for ($i = 0; $i -lt $objects.Count; $i++) {
        $offsets += $pdfText.Length
        $pdfText += "$($i + 1) 0 obj`n$($objects[$i])`nendobj`n"
    }
    $xref = $pdfText.Length
    $pdfText += "xref`n0 5`n0000000000 65535 f `n"
    foreach ($offset in $offsets) { $pdfText += ('{0:D10} 00000 n ' -f $offset) + "`n" }
    $pdfText += "trailer`n<< /Size 5 /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n"
    [IO.File]::WriteAllText($pdf, $pdfText, [Text.Encoding]::ASCII)
    $document = (Invoke-Cli -Arguments @('preview', $pdf)).data
    if (-not $document.fallback) {
        $page = (Invoke-Cli -Arguments @('window', 'page', '2')).data
        Assert-True ($page.page -eq 2 -and $page.page_count -eq 2 -and $page.wait_completed) 'PDF page navigation failed'
        Invoke-Cli -Arguments @('window', 'page', '3') -Expected 3 | Out-Null
    }

    $allSettings = (Invoke-Cli -Arguments @('settings', 'list')).data.settings
    Assert-True ($allSettings.Count -ge 40) 'Public settings catalog is incomplete'
    Assert-True (@($allSettings | Where-Object key -Match 'LastSuccessfulCheck|RetryAfter|WindowSize').Count -eq 0) 'Private state exposed'
    Assert-True (@($allSettings | Where-Object key -Match 'MonitorFile|RefreshIntervalMs').Count -eq 0) 'File monitoring was exposed as a global setting'
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
