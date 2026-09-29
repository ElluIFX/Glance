[CmdletBinding()]
param([string] $GoExecutable = 'go')
$ErrorActionPreference = 'Stop'
$payload = Join-Path $PSScriptRoot '../third_party'
$sources = Get-Content (Join-Path $payload 'sources.json') -Raw | ConvertFrom-Json
foreach ($file in $sources.files) {
    if ((Get-FileHash (Join-Path $payload $file.path) -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "File format data failed verification: $($file.path)"
    }
}
[xml] $registry = Get-Content (Join-Path $payload 'formats.xml') -Raw
$extensions = [ordered]@{}
foreach ($format in $registry.SelectNodes('//*[local-name()="FileFormat"]')) {
    $values = @($format.SelectNodes('*[local-name()="Extension"]') | ForEach-Object { '.' + $_.InnerText.Trim().ToLowerInvariant() } | Sort-Object -Unique)
    if ($values.Count) { $extensions[$format.PUID] = $values }
}
# PRONOM's FBX Binary record omits its conventional extension.
$extensions['fmt/1009'] = @('.fbx')
[System.IO.File]::WriteAllText((Join-Path $payload 'formats.json'), ($extensions | ConvertTo-Json -Compress), [System.Text.UTF8Encoding]::new($false))
Push-Location $PSScriptRoot
try {
    & $GoExecutable build -trimpath -ldflags='-s -w' -o ../third_party/Glance.FileFormatHost.exe .
    if ($LASTEXITCODE -ne 0) { throw 'File format host build failed.' }
}
finally { Pop-Location }
