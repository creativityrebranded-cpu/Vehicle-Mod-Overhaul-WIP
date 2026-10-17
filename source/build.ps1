# Builds SeamlessPlanets.dll (Release) with Visual Studio 2022 Build Tools + CMake.
# Usage: powershell -ExecutionPolicy Bypass -File plugin\build.ps1
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "C:\Program Files\CMake\bin\cmake.exe" }
& $cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
& $cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Write-Host "Built: $root\build\Release\SeamlessPlanets.dll"
