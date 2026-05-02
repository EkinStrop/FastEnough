# Setup script - downloads Dear ImGui into thirdparty/imgui
$ErrorActionPreference = "Stop"

$IMGUI_VERSION = "v1.91.8"
$IMGUI_DIR = "$PSScriptRoot\thirdparty\imgui"

if (Test-Path "$IMGUI_DIR\imgui.h") {
    Write-Host "ImGui already present in $IMGUI_DIR" -ForegroundColor Green
    exit 0
}

Write-Host "Downloading Dear ImGui $IMGUI_VERSION..." -ForegroundColor Cyan

$zipUrl = "https://github.com/ocornut/imgui/archive/refs/tags/$IMGUI_VERSION.zip"
$zipPath = "$env:TEMP\imgui-$IMGUI_VERSION.zip"
$extractPath = "$env:TEMP\imgui-extract"

# Download
Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing

# Extract
if (Test-Path $extractPath) { Remove-Item $extractPath -Recurse -Force }
Expand-Archive -Path $zipPath -DestinationPath $extractPath

# Copy to thirdparty
if (Test-Path $IMGUI_DIR) { Remove-Item $IMGUI_DIR -Recurse -Force }
$extractedFolder = Get-ChildItem $extractPath | Select-Object -First 1
Copy-Item $extractedFolder.FullName -Destination $IMGUI_DIR -Recurse

# Cleanup
Remove-Item $zipPath -Force
Remove-Item $extractPath -Recurse -Force

Write-Host "ImGui installed to $IMGUI_DIR" -ForegroundColor Green
