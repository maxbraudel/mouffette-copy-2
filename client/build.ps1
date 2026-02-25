param(
    [switch]$ConsoleLogs
)

$ErrorActionPreference = 'Stop'

Write-Host "Building Mouffette Client (Windows)" -ForegroundColor Cyan


# 1) Ensure MSYS2 UCRT64 toolchain (Qt6) is available
$msysRoot = 'C:\msys64'
$ucrtBin  = Join-Path $msysRoot 'ucrt64\bin'
if (-not (Test-Path $ucrtBin)) {
    Write-Error "MSYS2 UCRT64 not found at $ucrtBin. Install MSYS2 (winget install -e --id MSYS2.MSYS2) and Qt6 packages (pacman -S --needed mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-qt6-websockets mingw-w64-ucrt-x86_64-qt6-declarative mingw-w64-ucrt-x86_64-qt6-multimedia)."
}

$qtCoreConfig  = Join-Path $msysRoot 'ucrt64\lib\cmake\Qt6\Qt6Config.cmake'
$qtQuickConfig = Join-Path $msysRoot 'ucrt64\lib\cmake\Qt6Quick\Qt6QuickConfig.cmake'
if (-not (Test-Path $qtCoreConfig)) {
    Write-Error "Qt6 CMake package not found at $qtCoreConfig. Install: pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base"
}
if (-not (Test-Path $qtQuickConfig)) {
    Write-Error "Qt6 Quick CMake package not found at $qtQuickConfig. Install: pacman -S --needed mingw-w64-ucrt-x86_64-qt6-declarative"
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
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed. If Qt Quick is missing, install: pacman -S --needed mingw-w64-ucrt-x86_64-qt6-declarative"
}

# 4) Build
$jobs = $env:NUMBER_OF_PROCESSORS
if (-not $jobs) { $jobs = 4 }
Write-Host "Building with $jobs job(s)..." -ForegroundColor Yellow
& cmake --build . -j $jobs | Out-Host
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed. Resolve the errors above and rerun ./build.ps1"
}

# 5) Deploy Qt dependencies and all DLLs to build folder
if (Test-Path 'MouffetteClient.exe') {
    Write-Host "Deploying Qt dependencies..." -ForegroundColor Yellow
    
    # First run windeployqt6 if available
    $windeployqt = Join-Path $ucrtBin 'windeployqt6.exe'
    if (Test-Path $windeployqt) {
        $windeployArgs = @('MouffetteClient.exe', '--no-translations', '--qmldir', (Join-Path $clientRoot 'resources\qml'))
        $deployOut = Join-Path $env:TEMP 'mouffette_windeployqt_stdout.log'
        $deployErr = Join-Path $env:TEMP 'mouffette_windeployqt_stderr.log'
        if (Test-Path $deployOut) { Remove-Item $deployOut -Force -ErrorAction SilentlyContinue }
        if (Test-Path $deployErr) { Remove-Item $deployErr -Force -ErrorAction SilentlyContinue }

        $deployProc = Start-Process -FilePath $windeployqt -ArgumentList $windeployArgs -NoNewWindow -Wait -PassThru -RedirectStandardOutput $deployOut -RedirectStandardError $deployErr

        if (Test-Path $deployOut) { Get-Content $deployOut | Out-Host }
        if (Test-Path $deployErr) { Get-Content $deployErr | Out-Host }

        if ($deployProc.ExitCode -ne 0) {
            Write-Warning "windeployqt6 exited with code $($deployProc.ExitCode). Continuing because manual runtime copy is performed below."
        }
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

    # Deploy Qt QML runtime modules (QtQuick, QtQml, QtMultimedia, etc.) required by QQuickWidget
    # Discovered from: grep "^import " resources/qml/*.qml
    $qtQmlSource = Join-Path $msysRoot 'ucrt64\share\qt6\qml'
    $qmlTarget   = Join-Path $buildDir 'share\qt6\qml'
    if (Test-Path $qtQmlSource) {
        # All top-level QML modules referenced by our .qml files
        # QtQuick includes sub-modules: Window, Controls, Layouts
        $qmlModules = @('QtQuick', 'QtQml', 'QtMultimedia', 'QtCore', 'QML', 'Qt')
        foreach ($mod in $qmlModules) {
            $modSrc = Join-Path $qtQmlSource $mod
            $modDst = Join-Path $qmlTarget   $mod
            if (Test-Path $modSrc) {
                if (-not (Test-Path $modDst)) {
                    New-Item -ItemType Directory -Path $modDst -Force | Out-Null
                }
                Copy-Item -Path (Join-Path $modSrc '*') -Destination $modDst -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        Write-Host "Deployed Qt QML runtime modules to build/share/qt6/qml/." -ForegroundColor Green
    } else {
        Write-Warning "Qt QML modules not found at $qtQmlSource. Quick Canvas may fall back to legacy."
    }

    Write-Host "Build successful. You can now run MouffetteClient.exe directly from the build folder." -ForegroundColor Green
} else {
    Write-Error "Build finished but MouffetteClient.exe was not found."
}
