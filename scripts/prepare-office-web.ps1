[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$repositoryRoot = Get-GlanceRepositoryRoot
$destination = Join-Path $repositoryRoot 'src\Glance.Components\Office\third_party\web'
$archiveRoot = Join-Path $repositoryRoot '.dependency-cache\office-web'
New-Item -ItemType Directory -Path $destination, $archiveRoot -Force | Out-Null

$packages = @(
    @{
        Name = 'ssf'; Version = '0.11.2'
        Integrity = '+idbmIXoYET47hH+d7dfm2epdOMUDjqcB4648sTZ+t2JwoyBFL/insLfB/racrDmsKB3diwsDA696pZMieAC5g=='
        Files = @('ssf.js', 'LICENSE')
    },
    @{
        Name = 'exceljs'; Version = '4.4.0'
        Integrity = 'XctvKaEMaj1Ii9oDOqbW/6e1gXknSY4g/aLCDicOXqBE4M0nRWkUu0PTp++UPNzoFY12BNHMfs/VadKIS6llvg=='
        Files = @('dist/exceljs.bare.js', 'LICENSE')
    },
    @{
        Name = 'pptx-renderer'; Version = '1.3.0'
        Url = 'https://registry.npmjs.org/@aiden0z/pptx-renderer/-/pptx-renderer-1.3.0.tgz'
        Integrity = 'VEXSn2vmdgTwWaw96bhsEyZiSxlrhF5O5dONI+dSBXmg1WH2XM9O1eyY2LFQ68J1SY+iZnE5BVgFGGX6iox9Og=='
        Files = @('dist/aiden0z-pptx-renderer.browser.es.js', 'LICENSE', 'THIRD_PARTY_NOTICES.md',
            'licenses/mtx-decompressor-MPL-2.0.txt', 'licenses/ECMA-text-copyright-notice.txt')
    },
    @{
        Name = 'jszip'; Version = '3.10.1'
        Integrity = 'xXDvecyTpGLrqFrvkrUSoxxfJI5AH7U8zxxtVclpsUtMCq4JQ290LY8AW5c7Ggnr/Y/oK+bQMbqK2qmtk3pN4g=='
        Files = @('dist/jszip.min.js', 'LICENSE.markdown')
    },
    @{
        Name = 'docx-preview'; Version = '0.4.0'
        Integrity = 'OdKtE/uj3M4RfGarLkGjahUzRg8/kBp0Sraj1r1NAY1tp/sTpHOBqDrzVf9onMBt9vxP6SdQ6bpLCUCsFwjgcA=='
        Files = @('dist/docx-preview.mjs', 'LICENSE')
    },
    @{
        Name = 'saxes'; Version = '6.0.0'
        Integrity = 'xAg7SOnEhrm5zI3puOOKyy1OMcMlIJZYNJY7xLBwSze0UjhPLnWfj2GF2EpT0jmzaJKIWKHLsaSSajf35bcYnA=='
        Files = @('saxes.js')
    },
    @{
        Name = 'xmlchars'; Version = '2.2.0'
        Integrity = 'JZnDKK8B0RCDw84FNdDAIpZK+JuJw+s7Lz8nksI7SIuU3UXJJslUthsi+uWBUYOwPFwW7W7PRLRfUKpxjtjFCw=='
        Files = @('xml/1.0/ed5.js', 'xml/1.1/ed2.js', 'xmlns/1.0/ed3.js', 'LICENSE')
    }
)

foreach ($package in $packages) {
    $archive = Join-Path $archiveRoot "$($package.Name)-$($package.Version).tgz"
    $expectedHash = [BitConverter]::ToString([Convert]::FromBase64String($package.Integrity)).Replace('-', '')
    if (!(Test-Path -LiteralPath $archive) -or
        (Get-FileHash -LiteralPath $archive -Algorithm SHA512).Hash -ne $expectedHash) {
        $archiveUrl = if ($package.ContainsKey('Url')) { $package.Url } else {
            "https://registry.npmjs.org/$($package.Name)/-/$($package.Name)-$($package.Version).tgz"
        }
        Invoke-WebRequest -Uri $archiveUrl -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA512).Hash -ne $expectedHash) {
        throw "Office dependency integrity check failed: $($package.Name)"
    }
    $packageRoot = Join-Path $destination "$($package.Name)\$($package.Version)"
    New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
    $entries = @($package.Files | ForEach-Object { "package/$_" })
    & tar.exe -xzf $archive -C $packageRoot --strip-components=1 @entries
    if ($LASTEXITCODE -ne 0) { throw "Cannot extract Office dependency: $($package.Name)" }
    foreach ($file in $package.Files) {
        if (!(Test-Path -LiteralPath (Join-Path $packageRoot $file) -PathType Leaf)) {
            throw "Office dependency file is missing: $($package.Name)/$file"
        }
    }
}

# Resolve XML namespaces before passing canonical names to the pinned ExcelJS transforms.
$excelPath = Join-Path $destination 'exceljs\4.4.0\dist\exceljs.bare.js'
$excelSource = [IO.File]::ReadAllText($excelPath).Replace("`r`n", "`n")
$namespaceParser = @'
const saxesParser = new SaxesParser({ xmlns: true });
  // Modified by Glance: OOXML prefixes are aliases, not element identities.
  const prefixes = new Map([
    ['http://schemas.openxmlformats.org/spreadsheetml/2006/main', ''],
    ['http://schemas.openxmlformats.org/package/2006/relationships', ''],
    ['http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing', 'xdr'],
    ['http://schemas.openxmlformats.org/drawingml/2006/main', 'a']
  ]);
  const qualifiedName = node => {
    const prefix = prefixes.get(node.uri);
    return prefix === undefined ? node.name : (prefix ? prefix + ':' : '') + node.local;
  };
  const normalizeTag = node => ({ ...node, name: qualifiedName(node),
    attributes: Object.fromEntries(Object.values(node.attributes).map(attribute => [
      attribute.uri === 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
        ? 'r:' + attribute.local : attribute.name, attribute.value
    ]))
  });
'@
$excelPatches = @(
    @{ Before = 'const saxesParser = new SaxesParser();'; After = $namespaceParser.Replace("`r`n", "`n") },
    @{ Before = "eventType: 'opentag',`n    value"; After = "eventType: 'opentag',`n    value: normalizeTag(value)" },
    @{ Before = "eventType: 'closetag',`n    value"; After = "eventType: 'closetag',`n    value: { name: qualifiedName(value) }" }
)
foreach ($patch in $excelPatches) {
    if (!$excelSource.Contains($patch.Before) -or $excelSource.IndexOf($patch.Before) -ne $excelSource.LastIndexOf($patch.Before)) {
        throw 'Office workbook namespace patch no longer matches the pinned parser'
    }
    $excelSource = $excelSource.Replace($patch.Before, $patch.After)
}
[IO.File]::WriteAllText($excelPath, $excelSource, [Text.UTF8Encoding]::new($false))

# Keep the HTML legend aligned with the bundled ECharts palette when no theme is present.
$rendererPath = Join-Path $destination 'pptx-renderer\1.3.0\dist\aiden0z-pptx-renderer.browser.es.js'
$renderer = [System.IO.File]::ReadAllText($rendererPath)
$legendPalette = 'const o = Array.isArray(e.color) ? e.color.filter((S) => typeof S == "string") : [], s = r.data ?? []'
$legendColor = 'O = EV(E, o[H] ?? "#2f6f8f")'
if (!$renderer.Contains($legendPalette) -or !$renderer.Contains($legendColor)) {
    throw 'Office chart legend patch no longer matches the pinned renderer'
}
$renderer = $renderer.Replace($legendPalette,
    'const o = Array.isArray(e.color) ? e.color.filter((S) => typeof S == "string") : ["#5070dd", "#b6d634", "#505372", "#ff994d", "#0ca8df", "#ffd10a", "#fb628b", "#785db0", "#3fbe95"], s = r.data ?? []')
$renderer = $renderer.Replace($legendColor, 'O = EV(E, o[H % o.length] ?? "#2f6f8f")')
[System.IO.File]::WriteAllText($rendererPath, $renderer, [System.Text.UTF8Encoding]::new($false))

# The upstream npm archive omits this license; retain the versioned upstream copy.
$saxesLicense = Join-Path $destination 'saxes\6.0.0\LICENSE'
$saxesLicenseHash = '0FAC2374380621B22E6B50451057721A9C52935B02D16D106A9F04897F061D0E'
if (!(Test-Path -LiteralPath $saxesLicense) -or
    (Get-FileHash -LiteralPath $saxesLicense -Algorithm SHA256).Hash -ne $saxesLicenseHash) {
    Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/lddubeau/saxes/v6.0.0/LICENSE' -OutFile $saxesLicense
}
if ((Get-FileHash -LiteralPath $saxesLicense -Algorithm SHA256).Hash -ne $saxesLicenseHash) {
    throw 'Office XML parser license integrity check failed'
}
