[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ApplicationDirectory,
    [switch]$ExpectFallback
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$applicationRoot = (Resolve-Path -LiteralPath $ApplicationDirectory).Path
$cliPath = Join-Path $applicationRoot 'Glance.CLI.exe'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GlanceAccessTestToken {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token, int type, out int value, int size, out int needed);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr process, uint flags, System.Text.StringBuilder path, ref int size);
    public static string ImagePath(int pid) {
        IntPtr process = OpenProcess(0x1000, false, pid);
        try {
            var path = new System.Text.StringBuilder(32768); int size = path.Capacity;
            return process != IntPtr.Zero && QueryFullProcessImageName(process, 0, path, ref size) ? path.ToString() : "";
        } finally { if(process != IntPtr.Zero) CloseHandle(process); }
    }
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenSCManager(string machine, string database, uint access);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenService(IntPtr manager, string name, uint access);
    [DllImport("advapi32.dll")] static extern bool CloseServiceHandle(IntPtr handle);
    public static bool Elevated(int pid) {
        IntPtr process = OpenProcess(0x1000, false, pid), token = IntPtr.Zero;
        try {
            if (process == IntPtr.Zero || !OpenProcessToken(process, 8, out token)) throw new System.ComponentModel.Win32Exception();
            int value, needed;
            if (!GetTokenInformation(token, 20, out value, 4, out needed)) throw new System.ComponentModel.Win32Exception();
            return value != 0;
        } finally { if (token != IntPtr.Zero) CloseHandle(token); if (process != IntPtr.Zero) CloseHandle(process); }
    }
    public static bool CanReconfigure() {
        IntPtr manager = OpenSCManager(null, null, 1), service = IntPtr.Zero;
        try {
            if (manager == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
            service = OpenService(manager, "Glance.Access", 2);
            return service != IntPtr.Zero;
        } finally { if (service != IntPtr.Zero) CloseServiceHandle(service); if (manager != IntPtr.Zero) CloseServiceHandle(manager); }
    }
}
'@

function Wait-ProcessState([string]$Name, [int]$PreviousId = 0) {
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    do {
        $process = Get-Process -Name $Name -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -ne $PreviousId -and [GlanceAccessTestToken]::ImagePath($_.Id) -eq (Join-Path $applicationRoot "$Name.exe") } |
            Select-Object -First 1
        if ($process) { return $process }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Name"
}
function Wait-CoreConnection([int]$CoreId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    do {
        $status = (& $cliPath status --no-start --json | ConvertFrom-Json)
        if ($LASTEXITCODE -eq 0 -and $status.data.core_connected -and $status.data.core_process_id -eq $CoreId) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Core did not establish IPC'
}
if ([GlanceAccessTestToken]::Elevated($PID)) { throw 'Run this test from a normal, non-elevated shell' }
try {
    & $cliPath status --json | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CLI failed to start App' }
    $app = Wait-ProcessState 'Glance'
    $core = Wait-ProcessState 'Glance.Core'
    Wait-CoreConnection $core.Id
    if ([GlanceAccessTestToken]::Elevated($app.Id) -or [GlanceAccessTestToken]::Elevated($core.Id)) {
        throw 'Installed App and Core must remain unelevated'
    }
    'PASS: App and Core use ordinary tokens'
    if (-not $ExpectFallback) {
        $helper = Wait-ProcessState 'Glance.AccessHost'
        if ([GlanceAccessTestToken]::CanReconfigure()) { throw 'Service was writable from a normal token' }
        'PASS: access helper exists and service configuration is protected'
        $oldCore = $core.Id
        $oldHelper = $helper.Id
        Stop-Process -Id $oldCore -Force
        $core = Wait-ProcessState 'Glance.Core' $oldCore
        Wait-CoreConnection $core.Id
        $helper = Wait-ProcessState 'Glance.AccessHost' $oldHelper
        if (-not (Get-Process -Id $oldHelper -ErrorAction SilentlyContinue)) {
            'PASS: Core recovery releases the old helper and creates a new lease'
        } else { throw 'Old helper survived Core exit' }
    }
    $oldApp = $app.Id
    Stop-Process -Id $oldApp -Force
    $app = Wait-ProcessState 'Glance' $oldApp
    if ([GlanceAccessTestToken]::Elevated($app.Id)) { throw 'Recovered App must remain unelevated' }
    'PASS: App crash recovery preserves ordinary token'
} finally {
    & $cliPath quit --json | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CLI shutdown failed' }
}
