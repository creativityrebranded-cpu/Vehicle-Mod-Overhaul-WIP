# Builds SeamlessVehicle.dll (Release) and the offline checks with Visual Studio 2022 Build Tools + CMake.
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "C:\Program Files\CMake\bin\cmake.exe" }
& $cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
& $cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Get-ChildItem "$root\build\Release\*_test.exe" | ForEach-Object { & $_.FullName | Out-Null; if ($LASTEXITCODE -ne 0) { throw "Offline check failed: $($_.Name)" } }
Write-Host "Built: $root\build\Release\SeamlessVehicle.dll (offline checks passed)"
