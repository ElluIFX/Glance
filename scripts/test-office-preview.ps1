[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$BuildOutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$repositoryRoot = Get-GlanceRepositoryRoot
$buildOutput = if ($BuildOutputDirectory) { [System.IO.Path]::GetFullPath($BuildOutputDirectory) }
    else { Join-Path $repositoryRoot "bin\$Configuration\x64" }
& (Join-Path $buildOutput 'Glance.Tests.exe') --office-package-tests
if ($LASTEXITCODE -ne 0) { throw 'Office package regression tests failed' }
$node = (Get-Command node.exe -ErrorAction Stop).Source
foreach ($test in @('office_document_projection_tests.mjs', 'office_workbook_archive_tests.mjs',
    'office_workbook_drawings_tests.mjs', 'office_workbook_worker_tests.mjs')) {
    & $node (Join-Path $repositoryRoot "tests\Glance.Tests\$test")
    if ($LASTEXITCODE -ne 0) { throw "Office preview regression failed: $test" }
}
& $node --experimental-vm-modules (Join-Path $repositoryRoot 'tests\Glance.Tests\office_document_cancellation_tests.mjs')
if ($LASTEXITCODE -ne 0) { throw 'Office document cancellation regression tests failed' }
