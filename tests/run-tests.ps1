param(
    [string]$VisualStudioPath,
    [string]$CMakePath,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildDirectory = Join-Path $projectRoot 'x64\tests'

if (-not $VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $VisualStudioPath = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    }
    if (-not $VisualStudioPath) {
        $ide = Get-ChildItem -Path "$env:ProgramFiles\Microsoft Visual Studio\*\*\Common7\IDE\devenv.exe" -File |
            Sort-Object { [version]$_.VersionInfo.ProductVersion } -Descending | Select-Object -First 1
        if ($ide) { $VisualStudioPath = $ide.Directory.Parent.Parent.FullName }
    }
}
if (-not $VisualStudioPath) { throw 'Visual Studio with the C++ desktop workload was not found. Pass -VisualStudioPath.' }

$idePath = Join-Path $VisualStudioPath 'Common7\IDE\devenv.exe'
$version = if (Test-Path -LiteralPath $idePath) {
    (Get-Item -LiteralPath $idePath).VersionInfo.ProductVersion
} else {
    throw 'Pass a Visual Studio installation containing Common7\IDE\devenv.exe.'
}
$major = ([version]$version).Major
$generator = switch ($major) {
    18 { 'Visual Studio 18 2026' }
    17 { 'Visual Studio 17 2022' }
    default { throw "Unsupported Visual Studio major version: $major" }
}
if (-not $CMakePath) {
    $CMakePath = Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $CMakePath)) { throw 'CMake was not found. Install the C++ CMake tools or pass -CMakePath.' }
$ctest = Join-Path (Split-Path $CMakePath -Parent) 'ctest.exe'

& $CMakePath -S $PSScriptRoot -B $buildDirectory -G $generator -A x64 "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioPath,version=$version"
if ($LASTEXITCODE -ne 0) { throw 'Test configuration failed.' }
& $CMakePath --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }
& $ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed.' }
