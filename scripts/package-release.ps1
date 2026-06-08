[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+$')]
    [string]$Version,

    [string]$DokanInstallerPath = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Dokan_x64.msi'),
    [string]$PlatformToolsSource = '',
    [string]$OutputDir = '',
    [string]$CertificateThumbprint = $env:AFM_SIGN_CERT_THUMBPRINT,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$SkipBuild,
    [switch]$AllowUnsigned,
    [switch]$RefreshPlatformTools
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $root 'Android File Manager.vcxproj'
$releaseDir = Join-Path $root 'x64\Release'
$exeName = 'Fast Enough - Android File Explorer.exe'
$exePath = Join-Path $releaseDir $exeName
$dokanDllPath = Join-Path $root 'thirdparty\dokan\lib\x64\dokan2.dll'
$readmePath = Join-Path $root 'readme.txt'
$projfsImagePath = Join-Path $root 'projfs_enable.png'
$platformToolsUrl = 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip'
$platformToolsCache = Join-Path $root 'thirdparty\android-platform-tools'

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = $root
}

function Find-MsBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $msbuildMatches = @(& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')
        if ($msbuildMatches.Count -gt 0) {
            return $msbuildMatches[0]
        }
    }
    return 'msbuild.exe'
}

function Find-SignTool {
    $kitsDir = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $kitsDir) {
        $tools = @(Get-ChildItem -LiteralPath $kitsDir -Recurse -Filter signtool.exe |
            Where-Object { $_.FullName -like '*\x64\signtool.exe' } |
            Sort-Object FullName -Descending)
        if ($tools.Count -gt 0) {
            return $tools[0].FullName
        }
    }
    return 'signtool.exe'
}

function Resolve-PlatformToolsDir {
    param(
        [string]$Source,
        [string]$CacheDir,
        [bool]$Refresh
    )

    if (-not [string]::IsNullOrWhiteSpace($Source)) {
        $sourcePath = (Resolve-Path -LiteralPath $Source).ProviderPath
        $item = Get-Item -LiteralPath $sourcePath
        if ($item.PSIsContainer) {
            $directAdb = Join-Path $item.FullName 'adb.exe'
            $nestedAdb = Join-Path $item.FullName 'platform-tools\adb.exe'
            if (Test-Path -LiteralPath $directAdb) {
                return $item.FullName
            }
            if (Test-Path -LiteralPath $nestedAdb) {
                return (Join-Path $item.FullName 'platform-tools')
            }
            throw "Platform tools source does not contain adb.exe: $Source"
        }

        $zipExtractDir = Join-Path $CacheDir 'provided'
        if (Test-Path -LiteralPath $zipExtractDir) {
            Remove-Item -LiteralPath $zipExtractDir -Recurse -Force
        }
        New-Item -ItemType Directory -Path $zipExtractDir | Out-Null
        Expand-Archive -LiteralPath $item.FullName -DestinationPath $zipExtractDir
        $zipAdb = Join-Path $zipExtractDir 'platform-tools\adb.exe'
        if (-not (Test-Path -LiteralPath $zipAdb)) {
            throw "Platform tools ZIP does not contain platform-tools\adb.exe: $Source"
        }
        return (Join-Path $zipExtractDir 'platform-tools')
    }

    $cachedPlatformTools = Join-Path $CacheDir 'platform-tools'
    $cachedAdb = Join-Path $cachedPlatformTools 'adb.exe'
    if ($Refresh -and (Test-Path -LiteralPath $cachedPlatformTools)) {
        Remove-Item -LiteralPath $cachedPlatformTools -Recurse -Force
    }
    if (Test-Path -LiteralPath $cachedAdb) {
        return $cachedPlatformTools
    }

    New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
    $zipPath = Join-Path $CacheDir 'platform-tools-latest-windows.zip'

    Write-Host "Downloading Android Platform Tools..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $platformToolsUrl -OutFile $zipPath -UseBasicParsing

    if (Test-Path -LiteralPath $cachedPlatformTools) {
        Remove-Item -LiteralPath $cachedPlatformTools -Recurse -Force
    }
    Expand-Archive -LiteralPath $zipPath -DestinationPath $CacheDir -Force

    if (-not (Test-Path -LiteralPath $cachedAdb)) {
        throw 'Downloaded Android Platform Tools package did not contain adb.exe.'
    }

    return $cachedPlatformTools
}

function Assert-FileExists {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing required release file: $Path"
    }
}

if (-not $SkipBuild) {
    $msbuild = Find-MsBuild
    & $msbuild $projectPath /m /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw 'Release build failed.'
    }
}

Assert-FileExists $exePath
Assert-FileExists $dokanDllPath
Assert-FileExists $DokanInstallerPath
Assert-FileExists $readmePath
Assert-FileExists $projfsImagePath

$signature = Get-AuthenticodeSignature -LiteralPath $exePath
if ($signature.Status -ne 'Valid') {
    if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
        $signTool = Find-SignTool
        & $signTool sign /fd SHA256 /td SHA256 /tr $TimestampUrl /sha1 $CertificateThumbprint $exePath
        if ($LASTEXITCODE -ne 0) {
            throw 'Code signing failed.'
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $exePath
    }

    if ($signature.Status -ne 'Valid' -and -not $AllowUnsigned) {
        throw 'Release exe is not signed. Set AFM_SIGN_CERT_THUMBPRINT or pass -AllowUnsigned for a local test package.'
    }
}

$platformToolsDir = Resolve-PlatformToolsDir -Source $PlatformToolsSource -CacheDir $platformToolsCache -Refresh $RefreshPlatformTools.IsPresent

$stageRoot = Join-Path $root "release-staging\$Version"
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stageRoot | Out-Null

Copy-Item -LiteralPath $exePath -Destination (Join-Path $stageRoot $exeName)
Copy-Item -LiteralPath $dokanDllPath -Destination (Join-Path $stageRoot 'dokan2.dll')
Copy-Item -LiteralPath $DokanInstallerPath -Destination (Join-Path $stageRoot 'Dokan_x64.msi')
Copy-Item -LiteralPath $readmePath -Destination (Join-Path $stageRoot 'readme.txt')
Copy-Item -LiteralPath $projfsImagePath -Destination (Join-Path $stageRoot 'projfs_enable.png')
Copy-Item -LiteralPath $platformToolsDir -Destination (Join-Path $stageRoot 'platform-tools') -Recurse

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$zipPath = Join-Path $OutputDir "Fast Enough - Android File Manager $Version.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $stageRoot '*') -DestinationPath $zipPath

Write-Host "Release ZIP created: $zipPath" -ForegroundColor Green
