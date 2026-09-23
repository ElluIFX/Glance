[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ApplicationDirectory,
    [switch]$ExpectFallback
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$applicationRoot = (Resolve-Path -LiteralPath $ApplicationDirectory).Path
$applicationPath = Join-Path $applicationRoot 'Glance.exe'
$corePath = Join-Path $applicationRoot 'Glance.Core.exe'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GlanceAccessTestToken {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token, int type, out int value, int size, out int needed);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static bool Elevated(int pid) {
        IntPtr process = OpenProcess(0x1000, false, pid), token = IntPtr.Zero;
        try {
            if (process == IntPtr.Zero || !OpenProcessToken(process, 8, out token)) throw new System.ComponentModel.Win32Exception();
            int value, needed;
            if (!GetTokenInformation(token, 20, out value, 4, out needed)) throw new System.ComponentModel.Win32Exception();
            return value != 0;
        } finally { if (token != IntPtr.Zero) CloseHandle(token); if (process != IntPtr.Zero) CloseHandle(process); }
    }
}
'@

function Wait-ProcessState([string]$Name, [int]$PreviousId = 0) {
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    do {
        $process = Get-Process -Name $Name -ErrorAction SilentlyContinue | Where-Object Id -NE $PreviousId | Select-Object -First 1
        if ($process) { return $process }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Name"
}

function Wait-CoreConnection([int]$CoreId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    $log = Join-Path $env:LOCALAPPDATA 'Glance\Logs\Glance.Core.log'
    do {
        $lines = Get-Content -LiteralPath $log -Tail 60
        if ($lines | Where-Object {
            $_.Contains("[pid:$CoreId] UI pipe connected.") -or
            $_.Contains("[pid:$CoreId] Keyboard hook recovered: UI connection restored.")
        }) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Core did not establish IPC'
}

if ([GlanceAccessTestToken]::Elevated($PID)) { throw 'Run this test from a normal, non-elevated shell' }
foreach ($existing in @(Get-Process Glance -ErrorAction SilentlyContinue)) {
    if ($existing.Path -ne $applicationPath) { throw 'Another Glance installation is running' }
}

$scheduler = New-Object -ComObject Schedule.Service
$scheduler.Connect()
$folder = $scheduler.GetFolder('\Glance')
$task = @($folder.GetTasks(1)) | Where-Object { @($_.Definition.Actions)[0].Path -eq $corePath } | Select-Object -First 1
if (-not $task) { throw 'Register a protected fixture task before running this test' }

try {
    if (-not $ExpectFallback -and -not (Get-Process Glance,Glance.Core -ErrorAction SilentlyContinue)) {
        $task.Run($null) | Out-Null
        $orphan = Wait-ProcessState 'Glance.Core'
        if (-not $orphan.WaitForExit(25000)) { throw 'Orphaned scheduled Core did not exit' }
        if (Get-Process Glance -ErrorAction SilentlyContinue) { throw 'Orphaned task resurrected the App' }
        'PASS: delayed task exits without resurrecting the App'
    }
    if (-not (Get-Process Glance -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath $applicationPath -WorkingDirectory $applicationRoot -WindowStyle Hidden
    }
    $app = Wait-ProcessState 'Glance'
    $core = Wait-ProcessState 'Glance.Core'
    Wait-CoreConnection $core.Id
    if ([GlanceAccessTestToken]::Elevated($app.Id)) { throw 'App must remain unelevated' }
    if ([GlanceAccessTestToken]::Elevated($core.Id) -eq $ExpectFallback.IsPresent) { throw 'Unexpected Core token elevation' }
    'PASS: launch token separation'
    if (-not $ExpectFallback) {
        $denied = $false
        try { $task.Enabled = $false } catch { $denied = $true }
        if (-not $denied) { throw 'Task definition was writable from a medium token' }
        'PASS: task definition is protected'
        $oldCore = $core.Id
        $task.Stop(0)
        $core = Wait-ProcessState 'Glance.Core' $oldCore
        if (-not [GlanceAccessTestToken]::Elevated($core.Id)) { throw 'Core recovery lost elevation' }
        Wait-CoreConnection $core.Id
        'PASS: elevated Core recovery through scheduled task'
    }
    $oldApp = $app.Id
    Stop-Process -Id $oldApp
    $app = Wait-ProcessState 'Glance' $oldApp
    if ([GlanceAccessTestToken]::Elevated($app.Id)) { throw 'Recovered App must remain unelevated' }
    'PASS: App crash recovery preserves ordinary token'
} finally {
    $shutdown = Start-Process -FilePath $applicationPath -ArgumentList '--shutdown' -WindowStyle Hidden -PassThru
    $shutdown.WaitForExit(10000) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ((Get-Process Glance,Glance.Core -ErrorAction SilentlyContinue) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
}
