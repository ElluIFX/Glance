[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string] $Configuration = "Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")

$repositoryRoot = Get-GlanceRepositoryRoot
$destinationDirectory = Join-Path $repositoryRoot "src\Glance.App\third_party\scintilla\bin\x64"
$dependencies = @(
    @{
        Name = "Scintilla.dll"
        Sha256 = "795B30CC239862E9F7AC1E92118A939B0059FC92073EA65AEB1969D24CD8B12B"
    },
    @{
        Name = "Lexilla.dll"
        Sha256 = "E20D2B61B605A0FEABFCC4562C11FB5D1FC42DC2FC5C143C74D4CFD63972B5D3"
    }
)

function Test-Dependency {
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [string] $Sha256
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }

    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    return $actualHash -eq $Sha256
}

function Invoke-DependencyDownload {
    param(
        [Parameter(Mandatory)]
        [string] $Uri,

        [Parameter(Mandatory)]
        [string] $OutFile
    )

    $attempts = 3
    for ($attempt = 1; $attempt -le $attempts; $attempt++) {
        try {
            Invoke-WebRequest -Uri $Uri -OutFile $OutFile -ErrorAction Stop
            return
        }
        catch {
            Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
            if ($attempt -eq $attempts) {
                throw
            }
            Write-Warning "Dependency download failed; retrying ($attempt/$attempts)"
            Start-Sleep -Seconds (2 * $attempt)
        }
    }
}

$allAvailable = $true
foreach ($dependency in $dependencies) {
    $path = Join-Path $destinationDirectory $dependency.Name
    if (-not (Test-Dependency -Path $path -Sha256 $dependency.Sha256)) {
        $allAvailable = $false
        break
    }
}

$threeDestination = Join-Path $repositoryRoot `
    "src\Glance.Components\Model3D\third_party\three\bin\r184\package"
$threeFiles = @(
    @{ Name = "LICENSE"; Sha256 = "8B378EBE60E2FE500158CB0AC71CB5E8B7D92953C2ABCC63A0EB90499653B5BC" },
    @{ Name = "build\three.core.min.js"; Sha256 = "6486AA0D719CFA87EC88DC47223B59B1FB8417A1A407FC0E52467C943E2F8CC9" },
    @{ Name = "build\three.module.min.js"; Sha256 = "36A60B0120335F89A80A0DAB70292292B0EC414B3D05E83CD09A3EA428C6712A" },
    @{ Name = "examples\jsm\controls\OrbitControls.js"; Sha256 = "FAABB4E8DFD9235EE4A9FD7C9A3D75F90F1689DBD4944BD6FD32117DACEC5F93" },
    @{ Name = "examples\jsm\curves\NURBSCurve.js"; Sha256 = "BEF2607618A7778455E71A1F0BD206951C382D313E2E245808CC7D3533D60FB6" },
    @{ Name = "examples\jsm\curves\NURBSUtils.js"; Sha256 = "C6BD7C4137D585098923F189687898EA3E8762EA3ECBE255D749A56353894379" },
    @{ Name = "examples\jsm\libs\fflate.module.js"; Sha256 = "209A4412EB48CE609EDB4391992A792FFCC3983D30EE7E2B0B89A8C470F3CD8A" },
    @{ Name = "examples\jsm\libs\meshopt_decoder.module.js"; Sha256 = "D428E73A000057C6C94BFCEDC3412D0C2DC14CA8800E88553AAB05113AD2BF19" },
    @{ Name = "examples\jsm\loaders\3MFLoader.js"; Sha256 = "3498E2C2A3B8FA23B183A5EFEDD56956D4D9A2353F4203B42C494D1606F30D65" },
    @{ Name = "examples\jsm\loaders\DRACOLoader.js"; Sha256 = "18ED04342C7BC7D282157B566F22992329623AB3230030C913D0353D0880B216" },
    @{ Name = "examples\jsm\loaders\FBXLoader.js"; Sha256 = "2C0F9920E80BFB65B43FC7CD3968CC0A760CB89807B67D688C7622CEA0790283" },
    @{ Name = "examples\jsm\loaders\GLTFLoader.js"; Sha256 = "97642D720F16CC9A0C9844934198E4D0C023BEA8E89576D0F7545D03B2D103D2" },
    @{ Name = "examples\jsm\loaders\MTLLoader.js"; Sha256 = "F1F8F0C6FCFA09671CB609954EA436521D835606853C110C5AFF2DC8188F5E61" },
    @{ Name = "examples\jsm\loaders\OBJLoader.js"; Sha256 = "86C8384E269B75A21D502D438E3F0DC2E09DC61B6E6DA6B81947F5B119112BF8" },
    @{ Name = "examples\jsm\loaders\PLYLoader.js"; Sha256 = "B824A6DDA90CBFB37CABF8E5462ED1673E7693852BC1A6A7BAB0465D7B634DFB" },
    @{ Name = "examples\jsm\loaders\STLLoader.js"; Sha256 = "023ED97F848B633D8BCD53D4DB3B996D29D0C644088700691297C552257D480B" },
    @{ Name = "examples\jsm\utils\BufferGeometryUtils.js"; Sha256 = "D700B4C4584B9FF53A04692624D2363019F6D1DF2978A207389168C6AE647A65" },
    @{ Name = "examples\jsm\utils\SkeletonUtils.js"; Sha256 = "B1632A703206C3D830DE9FCBE515696770D04B71A15EE6B50AFA6D2C3298C86F" },
    @{ Name = "examples\jsm\libs\draco\gltf\draco_decoder.wasm"; Sha256 = "A680D927BED9CB864DDBD63521868891AF2BFBE755092761B4837487618DF8AC" },
    @{ Name = "examples\jsm\libs\draco\gltf\draco_wasm_wrapper.js"; Sha256 = "8BB2952D2BA7D67E1414F8DF819410CB0434A666BE53F671FFF75F68843D76F6" }
)
$threeAvailable = $true
foreach ($file in $threeFiles) {
    if (-not (Test-Dependency `
            -Path (Join-Path $threeDestination $file.Name) `
            -Sha256 $file.Sha256)) {
        $threeAvailable = $false
        break
    }
}

if ($threeAvailable) {
    Write-Host "Three.js r184 model preview dependencies are available."
}
else {
    $threeArchiveUri = "https://registry.npmjs.org/three/-/three-0.184.0.tgz"
    $threeArchiveSha256 = "5C8C75278504FF31CEDCD736E043B06C18F2CBF220AD7F85FBFAA1A08D3B3626"
    $threeTemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
        "Glance.Three." + [Guid]::NewGuid().ToString("N"))
    $threeArchivePath = Join-Path $threeTemporaryDirectory "three-0.184.0.tgz"
    $threeExpandedDirectory = Join-Path $threeTemporaryDirectory "expanded"
    try {
        New-Item -ItemType Directory -Path $threeExpandedDirectory -Force | Out-Null
        Write-Host "Downloading Three.js r184 model preview dependencies..."
        Invoke-DependencyDownload -Uri $threeArchiveUri -OutFile $threeArchivePath
        $actualArchiveHash =
            (Get-FileHash -LiteralPath $threeArchivePath -Algorithm SHA256).Hash
        if ($actualArchiveHash -ne $threeArchiveSha256) {
            throw "Three.js archive SHA-256 mismatch. Expected $threeArchiveSha256, got $actualArchiveHash."
        }

        & "$env:SystemRoot\System32\tar.exe" `
            -xf $threeArchivePath `
            -C $threeExpandedDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract the Three.js archive."
        }

        foreach ($file in $threeFiles) {
            $sourcePath =
                Join-Path (Join-Path $threeExpandedDirectory "package") $file.Name
            if (-not (Test-Dependency -Path $sourcePath -Sha256 $file.Sha256)) {
                throw "Three.js dependency '$($file.Name)' is missing or failed SHA-256 validation."
            }
            $destinationPath = Join-Path $threeDestination $file.Name
            New-Item -ItemType Directory `
                -Path (Split-Path -Parent $destinationPath) `
                -Force |
                Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
        }
    }
    finally {
        if (Test-Path -LiteralPath $threeTemporaryDirectory) {
            Remove-Item -LiteralPath $threeTemporaryDirectory -Recurse -Force
        }
    }
    Write-Host "Three.js r184 model preview dependencies are ready."
}

$occtDestination = Join-Path $repositoryRoot `
    "src\Glance.Components\Model3D\third_party\occt-import-js\bin\0.0.23\package"
$occtFiles = @(
    @{ Name = "dist\occt-import-js.js"; Sha256 = "3FB44CE11D00611F9B3F3C5775D520EBAB48930C1F08279B7B1316F05F0D3379" },
    @{ Name = "dist\occt-import-js.wasm"; Sha256 = "33391FC9D94EA5C869A6718488BF0A9A464222BAC9BDC764DFE1690CEF281952" },
    @{ Name = "LICENSE.md"; Sha256 = "20C17D8B8C48A600800DFD14F95D5CB9FF47066A9641DDEAB48DC54AEC96E331" }
)
$occtAvailable = $true
foreach ($file in $occtFiles) {
    if (-not (Test-Dependency `
            -Path (Join-Path $occtDestination $file.Name) `
            -Sha256 $file.Sha256)) {
        $occtAvailable = $false
        break
    }
}

if ($occtAvailable) {
    Write-Host "occt-import-js 0.0.23 model preview dependencies are available."
}
else {
    $occtArchiveUri =
        "https://registry.npmjs.org/occt-import-js/-/occt-import-js-0.0.23.tgz"
    $occtArchiveSha256 =
        "EAAB9CA7BF02799360D8FC70468DC8BF78EB2073C3FAFB1FF982006E558FC1F3"
    $occtLicenseUri =
        "https://raw.githubusercontent.com/kovacsv/occt-import-js/c2148e54b456b571238d35cac037d304053d64b2/LICENSE.md"
    $occtTemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
        "Glance.OcctImport." + [Guid]::NewGuid().ToString("N"))
    $occtArchivePath = Join-Path $occtTemporaryDirectory "occt-import-js-0.0.23.tgz"
    $occtExpandedDirectory = Join-Path $occtTemporaryDirectory "expanded"
    $occtLicensePath = Join-Path $occtTemporaryDirectory "LICENSE.md"
    try {
        New-Item -ItemType Directory -Path $occtExpandedDirectory -Force | Out-Null
        Write-Host "Downloading occt-import-js 0.0.23 model preview dependencies..."
        Invoke-DependencyDownload -Uri $occtArchiveUri -OutFile $occtArchivePath
        $actualArchiveHash =
            (Get-FileHash -LiteralPath $occtArchivePath -Algorithm SHA256).Hash
        if ($actualArchiveHash -ne $occtArchiveSha256) {
            throw "occt-import-js archive SHA-256 mismatch. Expected $occtArchiveSha256, got $actualArchiveHash."
        }

        & "$env:SystemRoot\System32\tar.exe" `
            -xf $occtArchivePath `
            -C $occtExpandedDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract the occt-import-js archive."
        }
        Invoke-DependencyDownload -Uri $occtLicenseUri -OutFile $occtLicensePath

        foreach ($file in $occtFiles) {
            $sourcePath = if ($file.Name -eq "LICENSE.md") {
                $occtLicensePath
            }
            else {
                Join-Path (Join-Path $occtExpandedDirectory "package") $file.Name
            }
            if (-not (Test-Dependency -Path $sourcePath -Sha256 $file.Sha256)) {
                throw "occt-import-js dependency '$($file.Name)' is missing or failed SHA-256 validation."
            }
            $destinationPath = Join-Path $occtDestination $file.Name
            New-Item -ItemType Directory `
                -Path (Split-Path -Parent $destinationPath) `
                -Force |
                Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
        }
    }
    finally {
        if (Test-Path -LiteralPath $occtTemporaryDirectory) {
            Remove-Item -LiteralPath $occtTemporaryDirectory -Recurse -Force
        }
    }
    Write-Host "occt-import-js 0.0.23 model preview dependencies are ready."
}

$esbuildDestination = Join-Path $repositoryRoot `
    "src\Glance.Components\Model3D\third_party\esbuild\bin\0.25.6\esbuild.exe"
$esbuildSha256 = "95D5529236270ADA558C344675C533D863F7760F491CF2B7ACC35C06BA63417D"
if (-not (Test-Dependency -Path $esbuildDestination -Sha256 $esbuildSha256)) {
    $esbuildArchiveUri =
        "https://registry.npmjs.org/@esbuild/win32-x64/-/win32-x64-0.25.6.tgz"
    $esbuildArchiveSha256 =
        "F07E7151A4595ECF5A87D6F0C8D1E0606F2C5486F3C09469EC203371B22F3D1C"
    $esbuildTemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
        "Glance.Esbuild." + [Guid]::NewGuid().ToString("N"))
    $esbuildArchivePath = Join-Path $esbuildTemporaryDirectory "esbuild-0.25.6.tgz"
    $esbuildExpandedDirectory = Join-Path $esbuildTemporaryDirectory "expanded"
    try {
        New-Item -ItemType Directory -Path $esbuildExpandedDirectory -Force | Out-Null
        Write-Host "Downloading esbuild 0.25.6 for the model preview bundle..."
        Invoke-DependencyDownload -Uri $esbuildArchiveUri -OutFile $esbuildArchivePath
        $actualArchiveHash =
            (Get-FileHash -LiteralPath $esbuildArchivePath -Algorithm SHA256).Hash
        if ($actualArchiveHash -ne $esbuildArchiveSha256) {
            throw "esbuild archive SHA-256 mismatch. Expected $esbuildArchiveSha256, got $actualArchiveHash."
        }

        & "$env:SystemRoot\System32\tar.exe" `
            -xf $esbuildArchivePath `
            -C $esbuildExpandedDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract the esbuild archive."
        }

        $esbuildSource = Join-Path $esbuildExpandedDirectory "package\esbuild.exe"
        if (-not (Test-Dependency -Path $esbuildSource -Sha256 $esbuildSha256)) {
            throw "esbuild.exe is missing or failed SHA-256 validation."
        }
        New-Item -ItemType Directory `
            -Path (Split-Path -Parent $esbuildDestination) `
            -Force |
            Out-Null
        Copy-Item -LiteralPath $esbuildSource -Destination $esbuildDestination -Force
    }
    finally {
        if (Test-Path -LiteralPath $esbuildTemporaryDirectory) {
            Remove-Item -LiteralPath $esbuildTemporaryDirectory -Recurse -Force
        }
    }
}

$modelWebRoot = Join-Path $repositoryRoot "src\Glance.Components\Model3D\web"
$generatedWebRoot = Join-Path $repositoryRoot `
    "src\Glance.Components\Model3D\third_party\web\bin"
$modelBundlePath = Join-Path $generatedWebRoot "viewer.bundle.js"
$cadLoaderSourcePath = Join-Path $generatedWebRoot "cad-loader.generated.js"
$cadLoaderBundlePath = Join-Path $generatedWebRoot "cad-loader.bundle.js"
New-Item -ItemType Directory -Path $generatedWebRoot -Force | Out-Null
& $esbuildDestination `
    (Join-Path $modelWebRoot "viewer.js") `
    "--bundle" `
    "--format=iife" `
    "--platform=browser" `
    "--target=es2022" `
    "--minify" `
    "--alias:three=$(Join-Path $threeDestination 'build\three.module.min.js')" `
    "--outfile=$modelBundlePath"
if ($LASTEXITCODE -ne 0) {
    throw "Unable to build the model preview script."
}

$occtRuntime = [System.IO.File]::ReadAllText(
    (Join-Path $occtDestination "dist\occt-import-js.js"))
$cadWorker = [System.IO.File]::ReadAllText(
    (Join-Path $modelWebRoot "cad-worker.js"))
$workerLiteral = ConvertTo-Json -InputObject "$occtRuntime`n$cadWorker" -Compress
$cadLoaderSource = [System.IO.File]::ReadAllText(
    (Join-Path $modelWebRoot "cad-loader.js"))
$workerMarker = '"__GLANCE_OCCT_WORKER_SOURCE__"'
$generatedCadLoader = $cadLoaderSource.Replace($workerMarker, $workerLiteral)
if ($generatedCadLoader -eq $cadLoaderSource) {
    throw "The OCCT worker source marker is missing."
}
[System.IO.File]::WriteAllText(
    $cadLoaderSourcePath,
    $generatedCadLoader,
    [System.Text.UTF8Encoding]::new($false))
& $esbuildDestination `
    $cadLoaderSourcePath `
    "--bundle" `
    "--format=iife" `
    "--platform=browser" `
    "--target=es2022" `
    "--minify" `
    "--outfile=$cadLoaderBundlePath"
if ($LASTEXITCODE -ne 0) {
    throw "Unable to build the CAD preview script."
}

$modelTemplate =
    [System.IO.File]::ReadAllText((Join-Path $modelWebRoot "index.html"))
$modelBundle = [System.IO.File]::ReadAllText($modelBundlePath)
$modelBundle = $modelBundle.Replace("</script", "<\/script")
$cadLoaderBundle = [System.IO.File]::ReadAllText($cadLoaderBundlePath)
$cadLoaderBundle = $cadLoaderBundle.Replace("</script", "<\/script")
$scriptMarker = "<!-- GLANCE_VIEWER_SCRIPT -->"
$modelHtml = $modelTemplate.Replace(
    $scriptMarker,
    "<script>`n$modelBundle`n</script>")
if ($modelHtml -eq $modelTemplate) {
    throw "The model preview script marker is missing."
}
$cadHtml = $modelTemplate.Replace(
    $scriptMarker,
    "<script>`n$cadLoaderBundle`n</script>`n<script>`n$modelBundle`n</script>")
[System.IO.File]::WriteAllText(
    (Join-Path $generatedWebRoot "index.html"),
    $modelHtml,
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    (Join-Path $generatedWebRoot "cad.html"),
    $cadHtml,
    [System.Text.UTF8Encoding]::new($false))
Write-Host "The model preview web bundles are ready."

$sevenZipVersion = "26.02"
$sevenZipDestination = Join-Path $repositoryRoot `
    "src\Glance.Components\Archive\third_party\7zip\bin\$sevenZipVersion"
$sevenZipDll = Join-Path $sevenZipDestination "7z.dll"
$sevenZipLicense = Join-Path $sevenZipDestination "License.txt"
$sevenZipArchiveHeader = Join-Path $sevenZipDestination "sdk\CPP\7zip\Archive\IArchive.h"
$sevenZipAvailable =
    (Test-Dependency `
        -Path $sevenZipDll `
        -Sha256 "69FD4DF057985C40E510E2FAC182881C7F85E90AA13EC703F763A8FDB2CE61F8") -and
    (Test-Dependency `
        -Path $sevenZipLicense `
        -Sha256 "519AC0A4BDED9C18EA02E0AFB71F663D8C47373BD9FACD3AC96A79F51D77765D") -and
    (Test-Dependency `
        -Path $sevenZipArchiveHeader `
        -Sha256 "A10F38D599DAD4926E2F219A577E3B11AA7214D1B4A02FA88ED9C71A5047CAEC")
if ($sevenZipAvailable) {
    Write-Host "7-Zip $sevenZipVersion archive preview dependencies are available."
}
else {
    $sevenZipBaseUri = "https://github.com/ip7z/7zip/releases/download/$sevenZipVersion"
    $sevenZipExtraSha256 =
        "081DF9E9311DFD9C9E0E98C1C80180B99BB51E4CB24156B5F3057FE3C259D70A"
    $sevenZipInstallerSha256 =
        "6745FA76DC2EA031596D8678F6F6B99C3C1B435B4164A63485ADBBC7B8D82EF0"
    $sevenZipSourceSha256 =
        "CF967C98BCA02A4B8B16375F441825A8E141362F14BE1969BBEC8E1CA0BFF9DD"
    $sevenZipTemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
        "Glance.SevenZip." + [Guid]::NewGuid().ToString("N"))
    $sevenZipExtra = Join-Path $sevenZipTemporaryDirectory "7z2602-extra.7z"
    $sevenZipInstaller = Join-Path $sevenZipTemporaryDirectory "7z2602-x64.exe"
    $sevenZipSource = Join-Path $sevenZipTemporaryDirectory "7z2602-src.tar.xz"
    $sevenZipExtraDirectory = Join-Path $sevenZipTemporaryDirectory "extra"
    $sevenZipInstallDirectory = Join-Path $sevenZipTemporaryDirectory "installed"
    $sevenZipSourceDirectory = Join-Path $sevenZipTemporaryDirectory "source"
    try {
        New-Item -ItemType Directory -Path $sevenZipExtraDirectory -Force | Out-Null
        New-Item -ItemType Directory -Path $sevenZipInstallDirectory -Force | Out-Null
        New-Item -ItemType Directory -Path $sevenZipSourceDirectory -Force | Out-Null
        Write-Host "Downloading 7-Zip $sevenZipVersion archive preview dependencies..."
        Invoke-DependencyDownload `
            -Uri "$sevenZipBaseUri/7z2602-extra.7z" `
            -OutFile $sevenZipExtra
        Invoke-DependencyDownload `
            -Uri "$sevenZipBaseUri/7z2602-x64.exe" `
            -OutFile $sevenZipInstaller
        Invoke-DependencyDownload `
            -Uri "$sevenZipBaseUri/7z2602-src.tar.xz" `
            -OutFile $sevenZipSource
        foreach ($archive in @(
                @{ Path = $sevenZipExtra; Hash = $sevenZipExtraSha256 },
                @{ Path = $sevenZipInstaller; Hash = $sevenZipInstallerSha256 },
                @{ Path = $sevenZipSource; Hash = $sevenZipSourceSha256 })) {
            $actualHash = (Get-FileHash -LiteralPath $archive.Path -Algorithm SHA256).Hash
            if ($actualHash -ne $archive.Hash) {
                throw "7-Zip dependency SHA-256 mismatch for '$($archive.Path)'."
            }
        }

        & "$env:SystemRoot\System32\tar.exe" `
            -xf $sevenZipExtra `
            -C $sevenZipExtraDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract the 7-Zip Extra archive."
        }
        & (Join-Path $sevenZipExtraDirectory "x64\7za.exe") `
            x $sevenZipInstaller `
            "-o$sevenZipInstallDirectory" `
            -y | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract the 7-Zip x64 package."
        }
        & "$env:SystemRoot\System32\tar.exe" `
            -xf $sevenZipSource `
            -C $sevenZipSourceDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract the 7-Zip source archive."
        }

        New-Item -ItemType Directory -Path $sevenZipDestination -Force | Out-Null
        Copy-Item `
            -LiteralPath (Join-Path $sevenZipInstallDirectory "7z.dll") `
            -Destination $sevenZipDll `
            -Force
        Copy-Item `
            -LiteralPath (Join-Path $sevenZipInstallDirectory "License.txt") `
            -Destination $sevenZipLicense `
            -Force
        $sdkDestination = Join-Path $sevenZipDestination "sdk"
        foreach ($header in Get-ChildItem `
                -LiteralPath $sevenZipSourceDirectory `
                -Filter "*.h" `
                -File `
                -Recurse) {
            $relative = [System.IO.Path]::GetRelativePath(
                $sevenZipSourceDirectory,
                $header.FullName)
            $destination = Join-Path $sdkDestination $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force |
                Out-Null
            Copy-Item -LiteralPath $header.FullName -Destination $destination -Force
        }
        if (-not (
                (Test-Dependency `
                    -Path $sevenZipDll `
                    -Sha256 "69FD4DF057985C40E510E2FAC182881C7F85E90AA13EC703F763A8FDB2CE61F8") -and
                (Test-Dependency `
                    -Path $sevenZipLicense `
                    -Sha256 "519AC0A4BDED9C18EA02E0AFB71F663D8C47373BD9FACD3AC96A79F51D77765D") -and
                (Test-Dependency `
                    -Path $sevenZipArchiveHeader `
                    -Sha256 "A10F38D599DAD4926E2F219A577E3B11AA7214D1B4A02FA88ED9C71A5047CAEC"))) {
            throw "7-Zip archive preview dependencies failed validation."
        }
    }
    finally {
        if (Test-Path -LiteralPath $sevenZipTemporaryDirectory) {
            Remove-Item -LiteralPath $sevenZipTemporaryDirectory -Recurse -Force
        }
    }
    Write-Host "7-Zip $sevenZipVersion archive preview dependencies are ready."
}
# ---------------------------------------------------------------------------
# HEIC / AVIF / camera RAW preview codec dependencies.
# Source archives are pinned by SHA-256; Release build artifacts are checked in
# under each component's third_party directory, while Debug artifacts are
# regenerated locally only when the current configuration is incomplete.
# ---------------------------------------------------------------------------
$codecTemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("Glance.Codec." + [Guid]::NewGuid().ToString("N"))
$codecSourcesRoot = Join-Path $codecTemporaryDirectory "src"
New-Item -ItemType Directory -Path $codecSourcesRoot -Force | Out-Null

try {

function Invoke-CodecCommand {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Executable,
        [Parameter(Mandatory)] [string[]] $Arguments
    )
    Write-Host "===== $Name ====="
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE."
    }
}

function Get-CodecSource {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Uri,
        [Parameter(Mandatory)] [string] $Sha256
    )
    $archive = Join-Path $codecTemporaryDirectory "$Name.tar.gz"
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading $Name codec sources..."
        Invoke-DependencyDownload -Uri $Uri -OutFile $archive
        $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($actualHash -ne $Sha256) {
            throw "$Name source archive SHA-256 mismatch."
        }
    }
    $directory = Join-Path $codecSourcesRoot $Name
    if (-not (Test-Path -LiteralPath $directory)) {
        & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $codecSourcesRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to extract the $Name source archive."
        }
    }
    return $directory
}

$msbuildExe = Get-GlanceMSBuild
$codecCmakeArgs = @("-G", "Visual Studio 18 2026", "-A", "x64")
# --- libde265 + libheif (Heic component) ---
$libde265Version = "1.1.1"
$libde265Out = Join-Path $repositoryRoot "src\Glance.Components\Heic\third_party\libde265\$libde265Version"
$libheifVersion = "1.23.1"
$libheifOut = Join-Path $repositoryRoot "src\Glance.Components\Heic\third_party\libheif\$libheifVersion"
$libde265Destination = Join-Path $libde265Out $Configuration
$libheifDestination = Join-Path $libheifOut $Configuration
$codecBuildNeeded =
    -not (Test-Path (Join-Path $libde265Destination "libde265.lib")) -or
    -not (Test-Path (Join-Path $libde265Destination "libde265.dll")) -or
    -not (Test-Path (Join-Path $libde265Destination "include\libde265\de265.h")) -or
    -not (Test-Path (Join-Path $libde265Destination "include\libde265\de265-version.h")) -or
    -not (Test-Path (Join-Path $libheifDestination "libheif.lib")) -or
    -not (Test-Path (Join-Path $libheifDestination "heif.dll")) -or
    -not (Test-Path (Join-Path $libheifDestination "include\libheif\heif.h")) -or
    -not (Test-Path (Join-Path $libheifDestination "include\libheif\heif_version.h"))
if ($codecBuildNeeded) {
    $libde265Src = Get-CodecSource -Name "libde265-$libde265Version" -Uri "https://github.com/strukturag/libde265/archive/refs/tags/v$libde265Version.tar.gz" -Sha256 "5B4FAC677018E6074196E8F9889F3E4A5310E46AFBF22A893F620D4E24D3510E"
    $libheifSrc = Get-CodecSource -Name "libheif-$libheifVersion" -Uri "https://github.com/strukturag/libheif/archive/refs/tags/v$libheifVersion.tar.gz" -Sha256 "0B14D6BDF5680488E3AEDE354B1E11BE1444B3FC4A30FCF2AE06BD6B601466BE"
    foreach ($config in @($Configuration)) {
        $rt = if ($config -eq "Release") { "MultiThreadedDLL" } else { "MultiThreadedDebugDLL" }
        $dest = Join-Path $libde265Out $config
        if (-not (Test-Path (Join-Path $dest "libde265.lib")) -or
            -not (Test-Path (Join-Path $dest "libde265.dll")) -or
            -not (Test-Path (Join-Path $dest "include\libde265\de265.h")) -or
            -not (Test-Path (Join-Path $dest "include\libde265\de265-version.h"))) {
            $build = Join-Path $codecTemporaryDirectory "de265-build-$config"
            Invoke-CodecCommand "libde265 $config configure" "cmake" @($codecCmakeArgs + @("-DBUILD_SHARED_LIBS=ON", "-S", $libde265Src, "-B", $build, "-DCMAKE_MSVC_RUNTIME_LIBRARY=$rt", "-DENABLE_SDL=OFF", "-DENABLE_ENCODER=OFF"))
            Invoke-CodecCommand "libde265 $config build" "cmake" @("--build", $build, "--config", $config, "--parallel", "8", "--target", "de265")
            New-Item -ItemType Directory -Path $dest -Force | Out-Null
            $de265Artifact = Get-ChildItem $build -Recurse -Filter "de265.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
            $de265Runtime = Get-ChildItem $build -Recurse -Filter "libde265.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $de265Artifact -or -not $de265Runtime) { throw "The shared libde265 artifacts were not produced for $config." }
            Copy-Item $de265Artifact.FullName (Join-Path $dest "libde265.lib") -Force
            Copy-Item $de265Runtime.FullName (Join-Path $dest "libde265.dll") -Force
            $de265HeaderDirectory = Join-Path $dest "include\libde265"
            New-Item -ItemType Directory -Path $de265HeaderDirectory -Force | Out-Null
            Copy-Item (Join-Path $libde265Src "libde265\de265.h") (Join-Path $de265HeaderDirectory "de265.h") -Force
            $de265VersionHeader = Get-ChildItem $build -Recurse -Filter "de265-version.h" -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $de265VersionHeader) { throw "The generated libde265 version header was not produced for $config." }
            Copy-Item $de265VersionHeader.FullName (Join-Path $de265HeaderDirectory "de265-version.h") -Force
            Write-Host "libde265 $config -> $dest"
        }
    }
    $de265Include = Join-Path $libde265Destination "include"
    foreach ($config in @($Configuration)) {
        $rt = if ($config -eq "Release") { "MultiThreadedDLL" } else { "MultiThreadedDebugDLL" }
        $dest = Join-Path $libheifOut $config
        if (-not (Test-Path (Join-Path $dest "libheif.lib")) -or
            -not (Test-Path (Join-Path $dest "heif.dll")) -or
            -not (Test-Path (Join-Path $dest "include\libheif\heif.h")) -or
            -not (Test-Path (Join-Path $dest "include\libheif\heif_version.h"))) {
            $build = Join-Path $codecTemporaryDirectory "heif-build-$config"
            $de265Lib = Join-Path $libde265Out "$config\libde265.lib"
            Invoke-CodecCommand "libheif $config configure" "cmake" @($codecCmakeArgs + @("-DBUILD_SHARED_LIBS=ON", "-S", $libheifSrc, "-B", $build, "-DCMAKE_MSVC_RUNTIME_LIBRARY=$rt", "-DWITH_LIBDE265=ON", "-DWITH_LIBDE265_PLUGIN=OFF", "-DENABLE_PLUGIN_LOADING=OFF", "-DLIBDE265_INCLUDE_DIR=$de265Include", "-DLIBDE265_LIBRARY=$de265Lib", "-DWITH_X265=OFF", "-DWITH_AOM_DECODER=OFF", "-DWITH_AOM_ENCODER=OFF", "-DWITH_DAV1D=OFF", "-DWITH_RAV1E=OFF", "-DWITH_SVT_ENCODER=OFF", "-DWITH_KVAZAAR=OFF", "-DWITH_OPENJPEG=OFF", "-DWITH_JPEG_DECODER=OFF", "-DWITH_JPEG_ENCODER=OFF", "-DWITH_PNG=OFF", "-DWITH_ZLIB=OFF"))
            Invoke-CodecCommand "libheif $config build" "cmake" @("--build", $build, "--config", $config, "--parallel", "8", "--target", "heif")
            New-Item -ItemType Directory -Path $dest -Force | Out-Null
            $heifArtifact = Get-ChildItem $build -Recurse -Filter "heif.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
            $heifRuntime = Get-ChildItem $build -Recurse -Filter "heif.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $heifArtifact -or -not $heifRuntime) { throw "The shared libheif artifacts were not produced for $config." }
            Copy-Item $heifArtifact.FullName (Join-Path $dest "libheif.lib") -Force
            Copy-Item $heifRuntime.FullName (Join-Path $dest "heif.dll") -Force
            $include = Join-Path $dest "include\libheif"
            New-Item -ItemType Directory -Path $include -Force | Out-Null
            Copy-Item (Join-Path $libheifSrc "libheif\api\libheif\*.h") $include -Force
            $heifVersionHeader = Get-ChildItem $build -Recurse -Filter "heif_version.h" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($heifVersionHeader) {
                Copy-Item $heifVersionHeader.FullName (Join-Path $include "heif_version.h") -Force
            }
            Write-Host "libheif $config -> $dest"
        }
    }
    Write-Host "HEIC codec dependencies are ready."
}
# --- dav1d + libavif (Avif component) ---
$dav1dVersion = "1.5.0"
$dav1dOut = Join-Path $repositoryRoot "src\Glance.Components\Avif\third_party\dav1d\$dav1dVersion"
$libavifVersion = "1.4.2"
$libavifOut = Join-Path $repositoryRoot "src\Glance.Components\Avif\third_party\libavif\$libavifVersion"
$codecBuildNeeded =
    -not (Test-Path (Join-Path $dav1dOut "$Configuration\dav1d.lib")) -or
    -not (Test-Path (Join-Path $dav1dOut "$Configuration\dav1d\dav1d.h")) -or
    -not (Test-Path (Join-Path $libavifOut "$Configuration\avif.lib")) -or
    -not (Test-Path (Join-Path $libavifOut "$Configuration\avif.h"))
if ($codecBuildNeeded) {
    $mesonCommand = Get-Command meson -ErrorAction SilentlyContinue
    if (-not $mesonCommand) {
        $pythonScriptsDirectory = Join-Path $env:APPDATA "Python\Python312\Scripts"
        if (Test-Path -LiteralPath (Join-Path $pythonScriptsDirectory "meson.exe")) {
            $env:PATH = $pythonScriptsDirectory + ";" + $env:PATH
        }
        else {
            throw "meson and ninja were not found. Install them with: python -m pip install meson ninja"
        }
    }
    $previousPath = $env:PATH
    try {
        $dav1dSrc = Get-CodecSource -Name "dav1d-$dav1dVersion" -Uri "https://github.com/videolan/dav1d/archive/refs/tags/$dav1dVersion.tar.gz" -Sha256 "78B15D9954B513EA92D27F39362535DED2243E1B0924FDE39F37A31EBED5F76B"
        foreach ($config in @(@{ Name = $Configuration; Type = $(if ($Configuration -eq "Release") { "release" } else { "debug" }) })) {
            $dest = Join-Path $dav1dOut $config.Name
            if (-not (Test-Path (Join-Path $dest "dav1d.lib")) -or
                -not (Test-Path (Join-Path $dest "dav1d\dav1d.h"))) {
                $build = Join-Path $codecTemporaryDirectory "dav1d-build-$($config.Name)"
                Invoke-CodecCommand "dav1d $($config.Name) meson" "meson" @("setup", $build, $dav1dSrc, "--vsenv", "--buildtype=$($config.Type)", "--default-library=static", "-Denable_tools=false", "-Denable_tests=false", "-Denable_examples=false", "-Denable_asm=false")
                Invoke-CodecCommand "dav1d $($config.Name) compile" "meson" @("compile", "-C", $build)
                New-Item -ItemType Directory -Path $dest -Force | Out-Null
                $dav1dArtifact = Get-ChildItem $build -Recurse -Filter "libdav1d.a" -ErrorAction SilentlyContinue | Select-Object -First 1
                if (-not $dav1dArtifact) { throw "libdav1d.a was not produced for $($config.Name)." }
                Copy-Item $dav1dArtifact.FullName (Join-Path $dest "dav1d.lib") -Force
                $dav1dInc = Join-Path $dest "dav1d"
                New-Item -ItemType Directory -Path $dav1dInc -Force | Out-Null
                Copy-Item (Join-Path $dav1dSrc "include\dav1d\*.h") $dav1dInc -Force
                $dav1dVersionHeader = Get-ChildItem $build -Recurse -Filter "version.h" | Where-Object { $_.FullName -match "dav1d" } | Select-Object -First 1
                if ($dav1dVersionHeader) { Copy-Item $dav1dVersionHeader.FullName (Join-Path $dav1dInc "version.h") -Force }
                Write-Host "dav1d $($config.Name) -> $dest"
            }
        }
        $libavifSrc = Get-CodecSource -Name "libavif-$libavifVersion" -Uri "https://github.com/AOMediaCodec/libavif/archive/refs/tags/v$libavifVersion.tar.gz" -Sha256 "2B645287340BA5A631D268B551DC2D72BD73AC33335962DD36DCDB6D8366921D"
        foreach ($config in @($Configuration)) {
            $rt = if ($config -eq "Release") { "MultiThreadedDLL" } else { "MultiThreadedDebugDLL" }
            $dest = Join-Path $libavifOut $config
            if (-not (Test-Path (Join-Path $dest "avif.lib")) -or
                -not (Test-Path (Join-Path $dest "avif.h"))) {
                $build = Join-Path $codecTemporaryDirectory "avif-build-$config"
                $dav1dLib = Join-Path $dav1dOut "$config\dav1d.lib"
                $dav1dInclude = Join-Path $dav1dSrc "include"
                Invoke-CodecCommand "libavif $config configure" "cmake" @($codecCmakeArgs + @("-DBUILD_SHARED_LIBS=OFF", "-S", $libavifSrc, "-B", $build, "-DCMAKE_MSVC_RUNTIME_LIBRARY=$rt", "-DAVIF_CODEC_DAV1D=SYSTEM", "-DDAV1D_INCLUDE_DIR=$(Join-Path $dav1dOut $config)", "-DDAV1D_LIBRARY=$dav1dLib", "-DAVIF_LIBYUV=OFF", "-DAVIF_BUILD_APPS=OFF", "-DAVIF_BUILD_TESTS=OFF", "-DAVIF_BUILD_UTILS=OFF", "-DAVIF_ENABLE_WERROR=OFF"))
                Invoke-CodecCommand "libavif $config build" "cmake" @("--build", $build, "--config", $config, "--parallel", "8", "--target", "avif")
                New-Item -ItemType Directory -Path $dest -Force | Out-Null
                $avifArtifact = Get-ChildItem $build -Recurse -Filter "avif_internal.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
                if (-not $avifArtifact) { throw "avif_internal.lib was not produced for $config." }
                Copy-Item $avifArtifact.FullName (Join-Path $dest "avif.lib") -Force
                Copy-Item (Join-Path $libavifSrc "include\avif\avif.h") (Join-Path $dest "avif.h") -Force
                Write-Host "libavif $config -> $dest"
            }
        }
    }
    finally {
        $env:PATH = $previousPath
    }
    Write-Host "AVIF codec dependencies are ready."
}

# --- libraw (Raw component) ---
$librawVersion = "0.22.2"
$librawOut = Join-Path $repositoryRoot "src\Glance.Components\Raw\third_party\libraw\$librawVersion"
$librawDestination = Join-Path $librawOut $Configuration
$librawNeeded =
    -not (Test-Path (Join-Path $librawDestination "libraw.lib")) -or
    -not (Test-Path (Join-Path $librawDestination "libraw.dll")) -or
    -not (Test-Path (Join-Path $librawDestination "libraw\libraw.h"))
if ($librawNeeded) {
    $librawSrc = Get-CodecSource -Name "LibRaw-$librawVersion" -Uri "https://github.com/LibRaw/LibRaw/archive/refs/tags/$librawVersion.tar.gz" -Sha256 "627928088300ECDE6CA91FFD202E189203F04AD61AD12F0FE9DC57B9A7A0FB3C"
    foreach ($config in @($Configuration)) {
        $dest = Join-Path $librawOut $config
        if (-not (Test-Path (Join-Path $dest "libraw.lib")) -or
            -not (Test-Path (Join-Path $dest "libraw.dll")) -or
            -not (Test-Path (Join-Path $dest "libraw\libraw.h"))) {
            Invoke-CodecCommand "libraw $config build" $msbuildExe @((Join-Path $librawSrc "buildfiles\libraw.vcxproj"), "/p:Configuration=$config", "/p:Platform=x64", "/p:PlatformToolset=v145", "/p:WindowsTargetPlatformVersion=10.0.26100.0", "/m", "/v:minimal", "/nologo")
            $outputDirectory = Join-Path $librawSrc ("buildfiles\" + $(if ($config -eq "Release") { "release-x86_64" } else { "debug-x86_64" }))
            New-Item -ItemType Directory -Path $dest -Force | Out-Null
            Copy-Item (Join-Path $outputDirectory "libraw.dll") (Join-Path $dest "libraw.dll") -Force
            Copy-Item (Join-Path $outputDirectory "libraw.lib") (Join-Path $dest "libraw.lib") -Force
            $librawHeaderDir = Join-Path $dest "libraw"
            New-Item -ItemType Directory -Path $librawHeaderDir -Force | Out-Null
            Copy-Item (Join-Path $librawSrc "libraw\*.h") $librawHeaderDir -Force
            Copy-Item (Join-Path $librawSrc "internal") (Join-Path $dest "internal") -Recurse -Force
            Write-Host "libraw $config -> $dest"
        }
    }
    Write-Host "Camera RAW codec dependencies are ready."
}

}
finally {
    if (Test-Path -LiteralPath $codecTemporaryDirectory) {
        Remove-Item -LiteralPath $codecTemporaryDirectory -Recurse -Force
    }
}

if ($allAvailable) {
    Write-Host "Scintilla runtime dependencies are available."
    return
}

$downloadUri = "https://www.scintilla.org/wscite564.zip"
$archiveSha256 = "DB573FB65C7C1979EDA11F36045A59446F87EB282E3DCAA47475F8A19074C094"
$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
    "Glance.Dependencies." + [Guid]::NewGuid().ToString("N"))
$archivePath = Join-Path $temporaryDirectory "wscite564.zip"
$expandedDirectory = Join-Path $temporaryDirectory "expanded"

try {
    New-Item -ItemType Directory -Path $temporaryDirectory -Force | Out-Null

    Write-Host "Downloading Scintilla 5.6.4 and Lexilla 5.5.1..."
    Invoke-DependencyDownload -Uri $downloadUri -OutFile $archivePath

    $actualArchiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    if ($actualArchiveHash -ne $archiveSha256) {
        throw "Scintilla archive SHA-256 mismatch. Expected $archiveSha256, got $actualArchiveHash."
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $expandedDirectory
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null

    foreach ($dependency in $dependencies) {
        $sourcePath = Join-Path $expandedDirectory ("wscite\" + $dependency.Name)
        if (-not (Test-Dependency -Path $sourcePath -Sha256 $dependency.Sha256)) {
            throw "$($dependency.Name) is missing or failed SHA-256 validation."
        }

        Copy-Item -LiteralPath $sourcePath `
            -Destination (Join-Path $destinationDirectory $dependency.Name) `
            -Force
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}

Write-Host "Scintilla runtime dependencies are ready."
