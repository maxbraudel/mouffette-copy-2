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

# 5) Deploy Qt dependencies and all DLLs to build folder
if (Test-Path 'MouffetteClient.exe') {
    Write-Host "Deploying Qt dependencies..." -ForegroundColor Yellow
    
    # First run windeployqt6 if available
    $windeployqt = Join-Path $ucrtBin 'windeployqt6.exe'
    if (Test-Path $windeployqt) {
        & $windeployqt 'MouffetteClient.exe' --no-translations | Out-Host
    }
    
    # Copy all required DLLs from MSYS2 UCRT64 bin
    Write-Host "Copying additional DLLs from MSYS2..." -ForegroundColor Yellow
    
    $buildDir = Get-Location
    
    # Base DLL patterns to copy (using wildcards for version numbers)
    $dllPatterns = @(
        'libmd4c.dll',
        'libgcc_s_seh-1.dll',
        'libstdc++-6.dll',
        'libwinpthread-1.dll',
        'libbrotlicommon.dll',
        'libbrotlidec.dll',
        'libbz2-1.dll',
        'libdouble-conversion.dll',
        'libfreetype-6.dll',
        'libglib-2.0-0.dll',
        'libgraphite2.dll',
        'libharfbuzz-0.dll',
        'libiconv-2.dll',
        'libicudt*.dll',
        'libicuin*.dll',
        'libicuuc*.dll',
        'libintl-8.dll',
        'libpcre2-8-0.dll',
        'libpcre2-16-0.dll',
        'libpng16-16.dll',
        'libzstd.dll',
        'zlib1.dll'
    )
    
    $copiedCount = 0
    foreach ($pattern in $dllPatterns) {
        $dlls = Get-ChildItem -Path $ucrtBin -Filter $pattern -ErrorAction SilentlyContinue
        foreach ($dll in $dlls) {
            Copy-Item $dll.FullName $buildDir -Force -ErrorAction SilentlyContinue
            $copiedCount++
        }
    }
    
    Write-Host "Copied $copiedCount DLL(s) to build directory." -ForegroundColor Green
    Write-Host "Build successful. You can now run MouffetteClient.exe directly from the build folder." -ForegroundColor Green
} else {
    Write-Error "Build finished but MouffetteClient.exe was not found."
}
