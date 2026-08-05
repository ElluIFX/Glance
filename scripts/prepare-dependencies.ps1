[CmdletBinding()]
param()

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
