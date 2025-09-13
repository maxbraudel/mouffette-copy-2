param(
    [switch]$ConsoleLogs
)

$ErrorActionPreference = 'Stop'

Write-Host "Building Mouffette Client (Windows)" -ForegroundColor Cyan

# 1) Ensure MSYS2 UCRT64 toolchain (Qt6) is available
$msysRoot = 'C:\msys64'
$ucrtBin  = Join-Path $msysRoot 'ucrt64\bin'
if (-not (Test-Path $ucrtBin)) {
    Write-Error "MSYS2 UCRT64 not found at $ucrtBin. Install MSYS2 (winget install -e --id MSYS2.MSYS2) and Qt6 packages (pacman -S --needed mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-qt6-websockets)."
}

# Prepend Qt6 toolchain to PATH for this session
$env:Path = "$ucrtBin;" + $env:Path

# 2) Prepare build directory
$clientRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $clientRoot
if (-not (Test-Path 'build')) { New-Item -ItemType Directory -Path 'build' | Out-Null }
Set-Location 'build'

# 3) Configure CMake
$consoleFlag = if ($ConsoleLogs) { 'ON' } else { 'OFF' }
Write-Host "Configuring CMake (CONSOLE_OUTPUT=$consoleFlag)..." -ForegroundColor Yellow
& cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCONSOLE_OUTPUT=$consoleFlag .. | Out-Host

# 4) Build
$jobs = $env:NUMBER_OF_PROCESSORS
if (-not $jobs) { $jobs = 4 }
Write-Host "Building with $jobs job(s)..." -ForegroundColor Yellow
& cmake --build . -j $jobs | Out-Host

# 5) Done
if (Test-Path 'MouffetteClient.exe') {
    Write-Host "Build successful." -ForegroundColor Green
    Write-Host "Run with: $clientRoot\run.ps1" -ForegroundColor Gray
} else {
    Write-Error "Build finished but MouffetteClient.exe was not found."
}
